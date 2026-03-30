#include <Storages/TimeSeries/PrometheusQueryToSQL/applyAggregationOperator.h>

#include <Common/Exception.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Storages/TimeSeries/PrometheusQueryToSQL/ConverterContext.h>
#include <Storages/TimeSeries/PrometheusQueryToSQL/SelectQueryBuilder.h>
#include <Storages/TimeSeries/PrometheusQueryToSQL/toVectorGrid.h>

#include <algorithm>


namespace DB::ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int CANNOT_EXECUTE_PROMQL_QUERY;
}


namespace DB::PrometheusQueryToSQL
{

namespace
{
    struct SimpleAggInfo
    {
        std::string_view ch_aggregate_function;
        bool needs_cast_to_float = false;
    };

    const SimpleAggInfo * getSimpleAggInfo(std::string_view operator_name)
    {
        static const std::unordered_map<std::string_view, SimpleAggInfo> simple_agg_map = {
            {"sum",    {"sum",        false}},
            {"avg",    {"avg",        false}},
            {"min",    {"min",        false}},
            {"max",    {"max",        false}},
            {"count",  {"count",      true}},
            {"stddev", {"stddevPop",  false}},
            {"stdvar", {"varPop",     false}},
        };

        auto it = simple_agg_map.find(operator_name);
        if (it == simple_agg_map.end())
            return nullptr;
        return &it->second;
    }

    bool isGroupOperator(std::string_view operator_name)
    {
        return operator_name == "group";
    }

    bool isQuantileOperator(std::string_view operator_name)
    {
        return operator_name == "quantile";
    }

    /// Builds the AST expression for computing the new group based on by/without/neither.
    /// In all cases, __name__ is dropped from the result (standard PromQL behavior for aggregation).
    ASTPtr computeNewGroupExpression(const PQT::AggregationOperator * agg_node)
    {
        if (agg_node->by)
        {
            /// by(label1, label2): keep only these labels.
            /// timeSeriesRemoveAllTagsExcept implicitly removes __name__ unless it's in the list.
            return makeASTFunction(
                "timeSeriesRemoveAllTagsExcept",
                make_intrusive<ASTIdentifier>(ColumnNames::Group),
                make_intrusive<ASTLiteral>(Array{agg_node->labels.begin(), agg_node->labels.end()}));
        }
        else if (agg_node->without)
        {
            /// without(label1, label2): remove these labels AND __name__.
            std::vector<std::string_view> tags_to_remove{agg_node->labels.begin(), agg_node->labels.end()};

            /// Always remove __name__ in aggregation.
            if (std::find(tags_to_remove.begin(), tags_to_remove.end(), kMetricName) == tags_to_remove.end())
                tags_to_remove.emplace_back(kMetricName);

            return makeASTFunction(
                "timeSeriesRemoveTags",
                make_intrusive<ASTIdentifier>(ColumnNames::Group),
                make_intrusive<ASTLiteral>(Array{tags_to_remove.begin(), tags_to_remove.end()}));
        }
        else
        {
            /// No clause: aggregate everything into one group (remove all tags).
            return makeASTFunction(
                "timeSeriesRemoveAllTagsExcept",
                make_intrusive<ASTIdentifier>(ColumnNames::Group),
                make_intrusive<ASTLiteral>(Array{}));
        }
    }

    /// Builds the two-step SQL for a simple aggregation operator on VECTOR_GRID data.
    /// Step 1: SELECT new_group, <agg>ForEach(values) AS values FROM <subquery> GROUP BY new_group
    /// Step 2: SELECT new_group AS group, values FROM <step1>
    SQLQueryPiece applySimpleAggregation(
        const PQT::AggregationOperator * agg_node,
        SQLQueryPiece && expression,
        std::string_view ch_aggregate_function,
        bool needs_cast_to_float,
        ConverterContext & context)
    {
        /// Convert the expression to VECTOR_GRID if it isn't already.
        expression = toVectorGrid(std::move(expression), context);

        if (expression.store_method == StoreMethod::EMPTY)
            return SQLQueryPiece{agg_node, agg_node->result_type, StoreMethod::EMPTY};

        /// Step 1: Aggregate with ForEach combinator.
        ASTPtr aggregation_query;
        {
            SelectQueryBuilder builder;

            /// new_group expression
            auto new_group_expr = computeNewGroupExpression(agg_node);
            new_group_expr->setAlias(ColumnNames::NewGroup);
            builder.select_list.push_back(std::move(new_group_expr));

            /// <agg>ForEach(values) AS values
            String foreach_function_name = String(ch_aggregate_function) + "ForEach";
            ASTPtr agg_expr = makeASTFunction(foreach_function_name, make_intrusive<ASTIdentifier>(ColumnNames::Values));

            if (needs_cast_to_float)
            {
                /// countForEach returns Array(UInt64), we need Array(Nullable(Float64)).
                String target_type = fmt::format("Array(Nullable({}))", context.scalar_data_type->getName());
                agg_expr = makeASTFunction("CAST", std::move(agg_expr), make_intrusive<ASTLiteral>(target_type));
            }

            agg_expr->setAlias(ColumnNames::Values);
            builder.select_list.push_back(std::move(agg_expr));

            context.subqueries.emplace_back(SQLSubquery{context.subqueries.size(), std::move(expression.select_query), SQLSubqueryType::TABLE});
            builder.from_table = context.subqueries.back().name;

            builder.group_by.push_back(make_intrusive<ASTIdentifier>(ColumnNames::NewGroup));

            aggregation_query = builder.getSelectQuery();
        }

        /// Step 2: Rename new_group -> group.
        ASTPtr column_renaming_query;
        {
            SelectQueryBuilder builder;

            builder.select_list.push_back(make_intrusive<ASTIdentifier>(ColumnNames::NewGroup));
            builder.select_list.back()->setAlias(ColumnNames::Group);

            builder.select_list.push_back(make_intrusive<ASTIdentifier>(ColumnNames::Values));

            context.subqueries.emplace_back(SQLSubquery{context.subqueries.size(), std::move(aggregation_query), SQLSubqueryType::TABLE});
            builder.from_table = context.subqueries.back().name;

            column_renaming_query = builder.getSelectQuery();
        }

        SQLQueryPiece res{agg_node, agg_node->result_type, StoreMethod::VECTOR_GRID};
        res.select_query = std::move(column_renaming_query);
        res.start_time = expression.start_time;
        res.end_time = expression.end_time;
        res.step = expression.step;
        res.metric_name_dropped = true;

        return res;
    }

    /// Handles the PromQL `group` operator, which returns 1 for each group that has a value.
    SQLQueryPiece applyGroupOperator(
        const PQT::AggregationOperator * agg_node,
        SQLQueryPiece && expression,
        ConverterContext & context)
    {
        expression = toVectorGrid(std::move(expression), context);

        if (expression.store_method == StoreMethod::EMPTY)
            return SQLQueryPiece{agg_node, agg_node->result_type, StoreMethod::EMPTY};

        /// Step 1: Group and normalize to 1/NULL.
        /// SELECT new_group,
        ///        arrayMap(x -> if(x > 0, toNullable(toFloat64(1)), NULL), countForEach(values)) AS values
        /// FROM <subquery>
        /// GROUP BY new_group
        ASTPtr aggregation_query;
        {
            SelectQueryBuilder builder;

            auto new_group_expr = computeNewGroupExpression(agg_node);
            new_group_expr->setAlias(ColumnNames::NewGroup);
            builder.select_list.push_back(std::move(new_group_expr));

            /// arrayMap(x -> if(x > 0, toNullable(toFloat64(1)), NULL), countForEach(values))
            auto count_expr = makeASTFunction("countForEach", make_intrusive<ASTIdentifier>(ColumnNames::Values));
            auto array_map_expr = makeASTFunction(
                "arrayMap",
                makeASTFunction(
                    "lambda",
                    makeASTFunction("tuple", make_intrusive<ASTIdentifier>("x")),
                    makeASTFunction(
                        "if",
                        makeASTFunction("greater", make_intrusive<ASTIdentifier>("x"), make_intrusive<ASTLiteral>(0u)),
                        makeASTFunction("toNullable", makeASTFunction("toFloat64", make_intrusive<ASTLiteral>(1))),
                        make_intrusive<ASTLiteral>(Field{}))),
                std::move(count_expr));

            array_map_expr->setAlias(ColumnNames::Values);
            builder.select_list.push_back(std::move(array_map_expr));

            context.subqueries.emplace_back(SQLSubquery{context.subqueries.size(), std::move(expression.select_query), SQLSubqueryType::TABLE});
            builder.from_table = context.subqueries.back().name;

            builder.group_by.push_back(make_intrusive<ASTIdentifier>(ColumnNames::NewGroup));

            aggregation_query = builder.getSelectQuery();
        }

        /// Step 2: Rename new_group -> group.
        ASTPtr column_renaming_query;
        {
            SelectQueryBuilder builder;

            builder.select_list.push_back(make_intrusive<ASTIdentifier>(ColumnNames::NewGroup));
            builder.select_list.back()->setAlias(ColumnNames::Group);

            builder.select_list.push_back(make_intrusive<ASTIdentifier>(ColumnNames::Values));

            context.subqueries.emplace_back(SQLSubquery{context.subqueries.size(), std::move(aggregation_query), SQLSubqueryType::TABLE});
            builder.from_table = context.subqueries.back().name;

            column_renaming_query = builder.getSelectQuery();
        }

        SQLQueryPiece res{agg_node, agg_node->result_type, StoreMethod::VECTOR_GRID};
        res.select_query = std::move(column_renaming_query);
        res.start_time = expression.start_time;
        res.end_time = expression.end_time;
        res.step = expression.step;
        res.metric_name_dropped = true;

        return res;
    }

    /// Handles the PromQL `quantile` aggregation operator.
    /// quantile(phi, instant_vector) computes the phi-quantile across all series in each group.
    SQLQueryPiece applyQuantileOperator(
        const PQT::AggregationOperator * agg_node,
        std::vector<SQLQueryPiece> && arguments,
        ConverterContext & context)
    {
        if (arguments.size() != 2)
        {
            throw Exception(ErrorCodes::CANNOT_EXECUTE_PROMQL_QUERY,
                "Aggregation operator '{}' expects 2 arguments, but got {}",
                agg_node->operator_name, arguments.size());
        }

        auto & phi_arg = arguments[0];
        auto & expression = arguments[1];

        if (phi_arg.store_method != StoreMethod::CONST_SCALAR)
        {
            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                "Aggregation operator '{}' currently requires a constant level parameter",
                agg_node->operator_name);
        }

        expression = toVectorGrid(std::move(expression), context);

        if (expression.store_method == StoreMethod::EMPTY)
            return SQLQueryPiece{agg_node, agg_node->result_type, StoreMethod::EMPTY};

        Float64 phi = phi_arg.scalar_value;

        /// Step 1: Aggregate using quantileForEach(phi)(values).
        ASTPtr aggregation_query;
        {
            SelectQueryBuilder builder;

            auto new_group_expr = computeNewGroupExpression(agg_node);
            new_group_expr->setAlias(ColumnNames::NewGroup);
            builder.select_list.push_back(std::move(new_group_expr));

            /// quantileForEach(phi)(values)
            auto agg_expr = addParametersToAggregateFunction(
                makeASTFunction("quantileForEach", make_intrusive<ASTIdentifier>(ColumnNames::Values)),
                make_intrusive<ASTLiteral>(phi));

            agg_expr->setAlias(ColumnNames::Values);
            builder.select_list.push_back(std::move(agg_expr));

            context.subqueries.emplace_back(SQLSubquery{context.subqueries.size(), std::move(expression.select_query), SQLSubqueryType::TABLE});
            builder.from_table = context.subqueries.back().name;

            builder.group_by.push_back(make_intrusive<ASTIdentifier>(ColumnNames::NewGroup));

            aggregation_query = builder.getSelectQuery();
        }

        /// Step 2: Rename new_group -> group.
        ASTPtr column_renaming_query;
        {
            SelectQueryBuilder builder;

            builder.select_list.push_back(make_intrusive<ASTIdentifier>(ColumnNames::NewGroup));
            builder.select_list.back()->setAlias(ColumnNames::Group);

            builder.select_list.push_back(make_intrusive<ASTIdentifier>(ColumnNames::Values));

            context.subqueries.emplace_back(SQLSubquery{context.subqueries.size(), std::move(aggregation_query), SQLSubqueryType::TABLE});
            builder.from_table = context.subqueries.back().name;

            column_renaming_query = builder.getSelectQuery();
        }

        SQLQueryPiece res{agg_node, agg_node->result_type, StoreMethod::VECTOR_GRID};
        res.select_query = std::move(column_renaming_query);
        res.start_time = expression.start_time;
        res.end_time = expression.end_time;
        res.step = expression.step;
        res.metric_name_dropped = true;

        return res;
    }
}


SQLQueryPiece applyAggregationOperator(
    const PQT::AggregationOperator * agg_node,
    std::vector<SQLQueryPiece> && arguments,
    ConverterContext & context)
{
    const auto & operator_name = agg_node->operator_name;

    /// Simple aggregation operators: sum, avg, min, max, count, stddev, stdvar.
    if (const auto * info = getSimpleAggInfo(operator_name))
    {
        if (arguments.size() != 1)
        {
            throw Exception(ErrorCodes::CANNOT_EXECUTE_PROMQL_QUERY,
                "Aggregation operator '{}' expects 1 argument, but got {}",
                operator_name, arguments.size());
        }

        return applySimpleAggregation(agg_node, std::move(arguments[0]), info->ch_aggregate_function, info->needs_cast_to_float, context);
    }

    /// The `group` operator: returns 1 for each group that has at least one series with a value.
    if (isGroupOperator(operator_name))
    {
        if (arguments.size() != 1)
        {
            throw Exception(ErrorCodes::CANNOT_EXECUTE_PROMQL_QUERY,
                "Aggregation operator '{}' expects 1 argument, but got {}",
                operator_name, arguments.size());
        }

        return applyGroupOperator(agg_node, std::move(arguments[0]), context);
    }

    /// The `quantile` operator: quantile(phi, instant_vector).
    if (isQuantileOperator(operator_name))
    {
        return applyQuantileOperator(agg_node, std::move(arguments), context);
    }

    throw Exception(ErrorCodes::NOT_IMPLEMENTED,
        "PromQL aggregation operator '{}' is not implemented", operator_name);
}

}
