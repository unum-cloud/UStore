/**
 *  @file src/ram/variants_serial.cpp
 *  @author Ash Vardanian
 *  @date October 25, 2026
 *  @brief The serial unit of the RAM variants: the floor every ladder ends on, its partition locks over the
 *      standard atomics, yielding to the scheduler between attempts.
 */
#include "blobs.hpp"   // `ram_variant::blobs_verbs_k`
#include "strings.hpp" // `ram_variant::strings_verbs_k`
#include "docs.hpp"    // `ram_variant::docs_verbs_k`
#include "graph.hpp"   // `ram_variant::graph_verbs_k`
#include "pages.hpp"   // `ram_variant::pages_verbs_k`

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::serial_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::serial_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
