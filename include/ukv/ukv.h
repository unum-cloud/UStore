/**
 * @file ukv.h
 * @author Ashot Vardanian
 * @date 12 Jun 2022
 * @addtogroup C
 *
 * @brief Binary Interface Standard for Multi-Modal Databases.
 *
 * Provides:
 * - @b ABI stability for the essential CRUD operations,
 * - @b Interoperability with higher-level languages.
 * - @b Flexibility in choosing the underlying engine.
 * - @b ACID transactions and snapshots support.
 * - @b Batch operations support.
 *
 * ## Hourglass Design
 *
 * Most of todays advanced software is written in C++, both the programs
 * and the libraries. Still, maintaining shared ABI-stable C++ interface
 * is essentially impossible in todays reality.
 *
 * That is why the "hourglass" pattern is used. The implementation is in
 * C++, the heavily templated convenient wrappers are in C++, but the
 * intermediate layer is C99 to maintain ABI stability and allow rolling
 * minor updates without recompilation of the user code.
 *
 * ## Interface Conventions
 *
 * - Choosing more arguments over more functions.
 *   Aiming for flexibility, we have functions have 4 allowed outputs or more.
 *   To cover all the combinations requests we will need 4!=24 functions just
 *   to replace that one.
 *
 * - Strides! Higher level systems may pack groups of arguments into AoS
 *   instead of SoA. To minimize the need of copies and data re-layout,
 *   we use @b byte-length strides arguments, similar to BLAS libraries.
 *   Passing Zero as a "stride" means repeating the same value.
 *
 * - Wrapping function arguments into structs.
 *   Some functions have over 20 arguments, accounting for all the options
 *   and strides. The majority of those are optional. This makes it impossible
 *   to remember the exact order. Using `struct`, the order becomes irrelevant.
 *   All arguments get names and default values can be skipped.
 */

#pragma once
#include "ukv/db.h"
#include "ukv/blobs.h"
#include "ukv/paths.h"
#include "ukv/docs.h"
#include "ukv/graph.h"
#include "ukv/vectors.h"
