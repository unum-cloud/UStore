/**
 *  @file src/ram/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded in-memory engine on SmashTable, strictly serializable or snapshot-isolated as configured at open.
 *
 *  Every collection keeps a partitioned multi-version store of its own, of the kind its modality picks: key UUIDs
 *  mapped onto values for blobs, onto JSON text for documents and onto dense element trees for pages, strings mapped
 *  onto 16-byte keys for strings, and the half-edges of a graph. Documents and pages are edited in place, each edit
 *  reading, modifying and writing them back inside one transaction. No stored key carries its collection. The
 *  @c _collections system collection maps every collection's UUID onto its name and its store, whose kind says its
 *  modality, and the @c _names system collection, a store of the strings kind, maps every name onto its collection's
 *  UUID. Every store is built into one commit order, so a transaction spanning any number of them publishes under one
 *  stamp and a snapshot reads all of them at it. Dropping a collection retires its store at the drop's stamp, and the
 *  store is freed once no reader holds an older one. A database with a directory persists as one binary image,
 *  rewritten whole on a flush and on close.
 *
 *  Every CPU class this build compiled has a variant per isolation level, its calls instantiated in
 *  <tt>src/ram/variants_&lt;class&gt;.cpp</tt> with the class's atomic reference and waiter, which
 *  the partition locks own their words through. An open walks the classes newest first and takes
 *  the first one the machine confirms, ending on the serial floor, and every handle the variant
 *  hands out carries its table.
 */

#include <string_view> // `std::string_view`

#include "ustore/db.h"

#include "lib.hpp" // `route_init`, `cpu_class_t`, `rung_advances`, `forkunion_admits`

#include "database.hpp" // `ram_variant`

namespace unum::ustore::ram {

#if USTORE_TARGET_INTEL_SIERRA
extern template struct ram_variant<cpu_class_t::intel_sierra_k, isolation_t::snapshot_k>;
extern template struct ram_variant<cpu_class_t::intel_sierra_k, isolation_t::strict_serializable_k>;
#endif // USTORE_TARGET_INTEL_SIERRA
#if USTORE_TARGET_INTEL_HASWELL
extern template struct ram_variant<cpu_class_t::intel_haswell_k, isolation_t::snapshot_k>;
extern template struct ram_variant<cpu_class_t::intel_haswell_k, isolation_t::strict_serializable_k>;
#endif // USTORE_TARGET_INTEL_HASWELL
#if USTORE_TARGET_AWS_G5
extern template struct ram_variant<cpu_class_t::aws_g5_k, isolation_t::snapshot_k>;
extern template struct ram_variant<cpu_class_t::aws_g5_k, isolation_t::strict_serializable_k>;
#endif // USTORE_TARGET_AWS_G5
#if USTORE_TARGET_APPLE_M2
extern template struct ram_variant<cpu_class_t::apple_m2_k, isolation_t::snapshot_k>;
extern template struct ram_variant<cpu_class_t::apple_m2_k, isolation_t::strict_serializable_k>;
#endif // USTORE_TARGET_APPLE_M2
#if USTORE_TARGET_AWS_G2
extern template struct ram_variant<cpu_class_t::aws_g2_k, isolation_t::snapshot_k>;
extern template struct ram_variant<cpu_class_t::aws_g2_k, isolation_t::strict_serializable_k>;
#endif // USTORE_TARGET_AWS_G2
extern template struct ram_variant<cpu_class_t::serial_k, isolation_t::snapshot_k>;
extern template struct ram_variant<cpu_class_t::serial_k, isolation_t::strict_serializable_k>;

/** Opens a database of @p cpu_class_ at @p isolation into @p database: the class a rung takes, and the unconditional
 *  serial floor every ladder ends on. */
template <cpu_class_t cpu_class_>
status_t open_admitted(ustore_database_t* database, ustore_isolation_t isolation, std::string_view directory) noexcept {
    if (isolation == ustore_isolation_snapshot_k)
        return ram_variant<cpu_class_, isolation_t::snapshot_k>::open(database, directory);
    return ram_variant<cpu_class_, isolation_t::strict_serializable_k>::open(database, directory);
}

/** One rung: taken when the machine confirms the class, else falls through; a class the build left out, or one
 *  picking its parent's waiter and atomics, is skipped without instantiating its variant. */
template <cpu_class_t cpu_class_, cpu_class_t next_class_, cpu_class_t... lower_classes_>
status_t open_admitted(ustore_database_t* database, ustore_isolation_t isolation, std::string_view directory) noexcept {
    if constexpr (rung_advances<cpu_class_>()) {
        if (forkunion_admits<cpu_class_>()) return open_admitted<cpu_class_>(database, isolation, directory);
    }
    else static_assert(!cpu_class_compiled(cpu_class_), "A compiled class no longer advances past the one below");
    return open_admitted<next_class_, lower_classes_...>(database, isolation, directory);
}

/** Opens the database @p arguments describe into their output, which stays null unless the open succeeds, with the
 *  newest class the machine admits, each lineage newest first, persisting in @p directory unless it is empty. */
template <typename arguments_type_>
status_t open_in(arguments_type_ const& arguments, std::string_view directory) noexcept {
    if (!arguments.database) return {status_code_t::invalid_argument_k, "Need an output for the database"};
    if (arguments.isolation != ustore_isolation_strict_serializable_k &&
        arguments.isolation != ustore_isolation_snapshot_k)
        return {status_code_t::invalid_argument_k, "Unknown isolation"};
    return open_admitted<cpu_class_t::intel_sierra_k, cpu_class_t::intel_haswell_k, cpu_class_t::aws_g5_k,
                         cpu_class_t::apple_m2_k, cpu_class_t::aws_g2_k, cpu_class_t::serial_k>(
        arguments.database, arguments.isolation, directory);
}

status_t init(ustore_database_init_ram_t const& arguments, view_t const&) noexcept { return open_in(arguments, {}); }

status_t open(ustore_database_open_ram_t const& arguments, view_t const&) noexcept {
    if (!arguments.directory.length) return {status_code_t::invalid_argument_k, "A database needs a directory"};
    return open_in(arguments, string_of(arguments.directory));
}

} // namespace unum::ustore::ram

using namespace unum::ustore::ram;

#pragma region C ABI

void ustore_database_init_ram(ustore_database_init_ram_t* arguments) { route_init<&init>(arguments); }

void ustore_database_open_ram(ustore_database_open_ram_t* arguments) { route_init<&ram::open>(arguments); }

#pragma endregion C ABI
