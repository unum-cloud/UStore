/**
 *  @file src/engine.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The table of calls every engine fills, which the C ABI in `c/lib.cpp` routes each handle through.
 */

#pragma once

#include <cstdint> // `std::uint8_t`
#include <cstring> // `std::memcpy`

#include <new>         // `std::bad_alloc`
#include <string_view> // `std::string_view`

#include <ustore/blobs.h>   // `ustore_read_t`, `ustore_write_t`, `ustore_scan_t`, `ustore_sample_t`, `ustore_measure_t`
#include <ustore/db.h>      // `ustore_database_t`, `ustore_transaction_t` and the calls on them
#include <ustore/strings.h> // `ustore_strings_read_t`, `ustore_strings_write_t`, `ustore_strings_scan_t`

namespace unum::ustore {

/**
 *  @brief One engine's implementation of every call of the C ABI but its own init.
 *
 *  Every call is `noexcept`: the in-house engines never throw, and an oracle wraps each call of its library in
 *  @c guarded.
 */
struct engine_t {
    void (*database_capabilities)(ustore_database_capabilities_t&) noexcept;
    void (*database_control)(ustore_database_control_t&) noexcept;
    void (*snapshot_list)(ustore_snapshot_list_t&) noexcept;
    void (*snapshot_create)(ustore_snapshot_create_t&) noexcept;
    void (*snapshot_export)(ustore_snapshot_export_t&) noexcept;
    void (*snapshot_drop)(ustore_snapshot_drop_t&) noexcept;
    void (*read)(ustore_read_t&) noexcept;
    void (*write)(ustore_write_t&) noexcept;
    void (*scan)(ustore_scan_t&) noexcept;
    void (*sample)(ustore_sample_t&) noexcept;
    void (*measure)(ustore_measure_t&) noexcept;
    void (*collection_create)(ustore_collection_create_t&) noexcept;
    void (*collection_drop)(ustore_collection_drop_t&) noexcept;
    void (*collection_list)(ustore_collection_list_t&) noexcept;
    void (*collection_find)(ustore_collection_find_t&) noexcept;
    void (*strings_write)(ustore_strings_write_t&) noexcept;
    void (*strings_read)(ustore_strings_read_t&) noexcept;
    void (*strings_scan)(ustore_strings_scan_t&) noexcept;
    void (*transaction_init)(ustore_transaction_init_t&) noexcept;
    void (*transaction_stage)(ustore_transaction_stage_t&) noexcept;
    void (*transaction_commit)(ustore_transaction_commit_t&) noexcept;
    void (*transaction_free)(ustore_transaction_t) noexcept;
    void (*database_free)(ustore_database_t) noexcept;
};

/** Leads every database and transaction handle an engine hands out, naming the engine that serves it. */
struct handle_t {
    engine_t const* engine = nullptr;
};

/** The engine's own handle behind @p handle, which the engine handed out as the @c handle_t it derives from. */
template <typename handle_type_>
handle_type_* handle_cast(void* handle) noexcept {
    return static_cast<handle_type_*>(static_cast<handle_t*>(handle));
}

/** Serves @p call_ of an oracle, whose library may throw, turning whatever it throws into a status. */
template <typename arguments_type_, void (*call_)(arguments_type_&)>
void guarded(arguments_type_& arguments) noexcept {
    try {
        call_(arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments.error = {ustore_error_out_of_memory_k, "Out of memory"};
    }
    catch (...) {
        *arguments.error = {ustore_error_internal_k, "Unexpected engine failure"};
    }
}

/** How a call keys the collections it addresses. */
enum class keying_t : std::uint8_t {
    integers_k,
    strings_k,
};

/** How a collection holding @p modality is keyed. */
constexpr keying_t keying_of(ustore_modality_t modality) noexcept {
    return modality == ustore_modality_strings_k ? keying_t::strings_k : keying_t::integers_k;
}

/** The record a collection's name maps onto: the collection's identifier in bytes @b [0,8), its modality in byte 8. */
struct name_record_t {
    char bytes[sizeof(ustore_collection_t) + 1] {};

    name_record_t(ustore_collection_t id, ustore_modality_t modality) noexcept {
        std::memcpy(bytes, &id, sizeof(id));
        bytes[sizeof(id)] = static_cast<char>(modality);
    }

    [[nodiscard]] std::string_view view() const noexcept { return {bytes, sizeof(bytes)}; }
};

/** Reads the identifier and the modality the bytes of @p record hold, answering false for bytes of any other length. */
template <typename bytes_type_>
bool read_name_record(bytes_type_ const& record, ustore_collection_t& id, ustore_modality_t& modality) noexcept {
    if (record.size() != sizeof(name_record_t::bytes)) return false;
    std::memcpy(&id, record.data(), sizeof(id));
    modality = static_cast<ustore_modality_t>(record[sizeof(id)]);
    return true;
}

} // namespace unum::ustore
