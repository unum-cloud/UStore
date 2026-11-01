/**
 *  @file src/nvme/engine.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The open of the NVMe engine, which picks the variant of <tt>src/nvme/variants.hpp</tt>
 *      the running CPU admits.
 *
 *  Every CPU class this build compiled has a variant, its calls instantiated in
 *  <tt>src/nvme/variants_&lt;class&gt;.cpp</tt> with the class's atomic reference and waiter. An open walks the
 *  classes newest first and takes the first one its ceiling fits, ending on the serial floor, and
 *  every handle the variant hands out carries its table, so a call reaches the class it was opened
 *  with through that one pointer.
 */

#include "variants.hpp" // `nvme_variant`, `cpu_class_t`, `cpu_ladder_t`, `open_fitting_rung`

namespace unum::ustore::nvme {

#if USTORE_TARGET_INTEL_ICELAKE
extern template struct nvme_variant<cpu_class_t::intel_icelake_k>;
#endif // USTORE_TARGET_INTEL_ICELAKE
#if USTORE_TARGET_INTEL_SKYLAKE
extern template struct nvme_variant<cpu_class_t::intel_skylake_k>;
#endif // USTORE_TARGET_INTEL_SKYLAKE
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
#if USTORE_TARGET_ALIBABA_C930
extern template struct nvme_variant<cpu_class_t::alibaba_c930_k>;
#endif // USTORE_TARGET_ALIBABA_C930
#if USTORE_TARGET_SERIAL
extern template struct nvme_variant<cpu_class_t::serial_k>;
#endif // USTORE_TARGET_SERIAL
#if USTORE_TARGET_MONOVERSIONED
extern template struct nvme_variant<cpu_class_t::monoversioned_k>;
#endif // USTORE_TARGET_MONOVERSIONED

/** Implements @c ustore_database_open_nvme with the newest class @p ceiling fits, each lineage newest first. */
status_t open(ustore_database_open_nvme_t const& arguments, cpu_ceiling_t ceiling, view_t const& view) noexcept {
    return open_fitting_rung(cpu_ladder_t {}, ceiling, [&]<cpu_class_t cpu_class_>() noexcept {
        return nvme_variant<cpu_class_>::open(arguments, view);
    });
}

} // namespace unum::ustore::nvme

using namespace unum::ustore::nvme;

#pragma region C ABI

void ustore_database_open_nvme(ustore_database_open_nvme_t* arguments) { route_init<&nvme::open>(arguments); }

#pragma endregion C ABI
