/**
 *  @file src/ram/variants_apple_m2.cpp
 *  @author Ash Vardanian
 *  @date October 25, 2026
 *  @brief The apple_m2 unit of the RAM variants: its partition locks move every word with LSE
 *      read-modify-writes and RCpc acquiring loads.
 */
#include "blobs.hpp"   // `ram_variant::blobs_verbs_k`
#include "strings.hpp" // `ram_variant::strings_verbs_k`
#include "docs.hpp"    // `ram_variant::docs_verbs_k`
#include "graph.hpp"   // `ram_variant::graph_verbs_k`
#include "pages.hpp"   // `ram_variant::pages_verbs_k`

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::apple_m2_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::apple_m2_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
