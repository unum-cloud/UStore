/**
 *  @file include/ustore/ustore.h
 *  @author Ash Vardanian
 *  @date June 12, 2022
 *  @brief The binary interface of UStore, a GPU-accelerated embedded transactional database for
 *      high-throughput NVMe devices.
 *
 *  The engines and the modalities are written in C++, and so are the templated wrappers, but the
 *  layer between them is C99, which keeps the binary interface stable across engines and minor
 *  releases and lets any language bind to it without instantiating templates.
 *
 *  Every call takes a single structure of named arguments, so optional arguments can be skipped and
 *  their order never matters. Batches pass their arguments as arrays with byte strides, as BLAS does,
 *  so callers that pack arguments into arrays of structures pass them without copies, and a zero
 *  stride repeats one value for every task.
 */

#pragma once

#include <ustore/blobs.h>
#include <ustore/db.h>
#include <ustore/docs.h>
#include <ustore/graph.h>
