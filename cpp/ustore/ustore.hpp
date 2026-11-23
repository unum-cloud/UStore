/**
 *  @file cpp/ustore/ustore.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief The C++ wrappers of the C ABI in @c ustore.h, with RAII handles, typed
 *      proxies and iterators.
 */
#pragma once

#include "ustore/types.hpp" // statuses, deferred calls, executors and arenas

#include "ustore/arrow.hpp"   // the Apache Arrow views and exports
#include "ustore/blobs.hpp"   // binary collections, their proxies, cursors and streams
#include "ustore/db.hpp"      // databases, transactions, snapshots, feeds of changes and ingests
#include "ustore/docs.hpp"    // collections of JSON documents and their tables
#include "ustore/graph.hpp"   // graph collections
#include "ustore/pages.hpp"   // collections of XML pages
#include "ustore/strings.hpp" // strings collections
