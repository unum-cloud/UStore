/**
 *  @file src/ram/variants_aws_g2.cpp
 *  @author Ash Vardanian
 *  @date October 25, 2026
 *  @brief The aws_g2 unit of the RAM variants: its partition locks spin with @c YIELD between attempts over the
 *      standard atomics.
 */
#include "blobs.hpp"   // `ram_variant::blobs_verbs_k`
#include "strings.hpp" // `ram_variant::strings_verbs_k`
#include "docs.hpp"    // `ram_variant::docs_verbs_k`
#include "graph.hpp"   // `ram_variant::graph_verbs_k`
#include "pages.hpp"   // `ram_variant::pages_verbs_k`

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::aws_g2_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::aws_g2_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
