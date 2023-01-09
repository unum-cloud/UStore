# @file tabular_graph.cpp
# @brief Imports a big Parquet/CSV/ORCA dataset as a labeled graph.
# @version 0.1
# @date 2022-10-02
#
# Every row is treated as a separate edge.
# All of its columns are treated as different document fields, except for
# > Integer column for source node ID.
# > Integer column for target node ID.
# > Optional integer column for document/edge ID.
# If the last one isn't provided, the row number is used as the document ID.
#
# https://arrow.apache.org/docs/python/dataset.html#dataset-discovery
# https://arrow.apache.org/docs/python/parquet.html

import fire
import ukv.umem as ukv


if __name__ == '__main__':
    pass
