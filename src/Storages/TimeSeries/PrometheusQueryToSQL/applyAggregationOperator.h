#pragma once

#include <Storages/TimeSeries/PrometheusQueryToSQL/SQLQueryPiece.h>


namespace DB::PrometheusQueryToSQL
{

/// Applies an aggregation operator (sum, avg, min, max, count, stddev, stdvar, group, quantile).
SQLQueryPiece applyAggregationOperator(
    const PQT::AggregationOperator * agg_node,
    std::vector<SQLQueryPiece> && arguments,
    ConverterContext & context);

}
