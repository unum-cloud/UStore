/**
 *  @file include/ustore/ustore.h
 *  @author Ash Vardanian
 *  @date June 12, 2022
 *  @brief The binary interface of UStore, a GPU-accelerated embedded transactional database for
 *      high-throughput NVMe devices.
 *
 *  The engines and the modalities are written in C++, and so are the templated wrappers, but the
 *  layer between them is C, which keeps the binary interface stable across engines and minor
 *  releases and lets any language bind to it without instantiating templates. The conventions every
 *  header shares are documented in @c types.h under Verbs, Asynchrony, Memory and Shapes.
 */

#ifndef UNUM_USTORE_USTORE_H
#define UNUM_USTORE_USTORE_H

#include "ustore/types.h"
#include "ustore/db.h"

#include "ustore/blobs.h"
#include "ustore/docs.h"
#include "ustore/graph.h"
#include "ustore/pages.h"
#include "ustore/strings.h"

#endif // UNUM_USTORE_USTORE_H
