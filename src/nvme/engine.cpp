/**
 *  @file src/nvme/engine.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The open of the NVMe engine, which picks the variant of <tt>src/nvme/variants.hpp</tt>
 *      the running CPU admits.
 *
 *  Every CPU class this build compiled has a variant, its calls instantiated in
 *  <tt>src/nvme/variants_&lt;class&gt;.cpp</tt> with the class's atomic reference and waiter. An
 *  open walks the classes newest first and takes the first one the machine confirms, ending on the
 *  serial floor, and every handle the variant hands out carries its table, so a call reaches the
 *  class it was opened with through that one pointer.
 */

#include "variants.hpp" // `nvme_variant`, `cpu_class_t`, `rung_advances`, `forkunion_admits`

namespace unum::ustore::nvme {

#if USTORE_TARGET_INTEL_SIERRA
extern template struct nvme_variant<cpu_class_t::intel_sierra_k>;
#endif // USTORE_TARGET_INTEL_SIERRA
#if USTORE_TARGET_INTEL_HASWELL
extern template struct nvme_variant<cpu_class_t::intel_haswell_k>;
#endif // USTORE_TARGET_INTEL_HASWELL
#if USTORE_TARGET_AWS_G5
extern template struct nvme_variant<cpu_class_t::aws_g5_k>;
#endif // USTORE_TARGET_AWS_G5
#if USTORE_TARGET_APPLE_M2
extern template struct nvme_variant<cpu_class_t::apple_m2_k>;
#endif // USTORE_TARGET_APPLE_M2
#if USTORE_TARGET_AWS_G2
extern template struct nvme_variant<cpu_class_t::aws_g2_k>;
#endif // USTORE_TARGET_AWS_G2
extern template struct nvme_variant<cpu_class_t::serial_k>;

/** The unconditional serial floor - every ladder's last rung. */
template <cpu_class_t cpu_class_>
status_t open_admitted(ustore_database_open_nvme_t const& arguments, view_t const& view) noexcept {
    return nvme_variant<cpu_class_>::open(arguments, view);
}

/** One rung: taken when the machine confirms the class, else falls through; a class the build left out, or one
 *  picking its parent's waiter and atomics, is skipped without instantiating its variant. */
template <cpu_class_t cpu_class_, cpu_class_t next_class_, cpu_class_t... lower_classes_>
status_t open_admitted(ustore_database_open_nvme_t const& arguments, view_t const& view) noexcept {
    if constexpr (rung_advances<cpu_class_>()) {
        if (forkunion_admits<cpu_class_>()) return nvme_variant<cpu_class_>::open(arguments, view);
    }
    else static_assert(!cpu_class_compiled(cpu_class_), "A compiled class no longer advances past the one below");
    return open_admitted<next_class_, lower_classes_...>(arguments, view);
}

/** Implements @c ustore_database_open_nvme with the newest class the machine admits, each lineage newest first. */
status_t open(ustore_database_open_nvme_t const& arguments, view_t const& view) noexcept {
    return open_admitted<cpu_class_t::intel_sierra_k, cpu_class_t::intel_haswell_k, cpu_class_t::aws_g5_k,
                         cpu_class_t::apple_m2_k, cpu_class_t::aws_g2_k, cpu_class_t::serial_k>(arguments, view);
}

} // namespace unum::ustore::nvme

using namespace unum::ustore::nvme;

#pragma region C ABI

void ustore_database_open_nvme(ustore_database_open_nvme_t* arguments) { route_init<&nvme::open>(arguments); }

#pragma endregion C ABI
