/**
 *  @file include/ustore/docs/json_patch.hpp
 *  @author Ash Vardanian
 *  @date December 26, 2022
 *  @brief Modifications of JSON documents: RFC 6902 patches, RFC 7386 merges, and inserts, updates and upserts of a
 *      whole document or of one field, applied with yyjson in an arena.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <string_view> // `std::string_view`

#include <yyjson.h> // `yyjson_mut_doc`, `yyjson_mut_merge_patch`

#include "ustore/docs.h" // `ustore_docs_modify_upsert_k`

#include "ustore/docs/json_values.hpp" // `json_t`, `json_mutable`, `field_pointer`
#include "ustore/utilities/types.hpp"  // `status_t`

namespace unum::ustore::docs {

#pragma region Modifications

/** Modifications a request applies, extending @c ustore_docs_modification_t with reads. */
enum class modification_t {
    nothing_k = -1,
    upsert_k = ustore_docs_modify_upsert_k,
    update_k = ustore_docs_modify_update_k,
    insert_k = ustore_docs_modify_insert_k,
    patch_k = ustore_docs_modify_patch_k,
    merge_k = ustore_docs_modify_merge_k,
};

/** Adds @p value at the non-empty @p path of @p root, replacing an object member and inserting into an array. */
inline bool patch_add(yyjson_mut_doc* document, yyjson_mut_val* root, std::string_view path,
                      yyjson_mut_val* value) noexcept {
    std::size_t const parent_length = path.rfind('/');
    if (parent_length == std::string_view::npos) return false;
    yyjson_mut_val const* const parent = parent_length
                                             ? yyjson_mut_ptr_getx(root, path.data(), parent_length, nullptr, nullptr)
                                             : root;
    if (yyjson_mut_is_obj(parent))
        return yyjson_mut_ptr_setx(root, path.data(), path.size(), value, document, false, nullptr, nullptr);
    return yyjson_mut_ptr_addx(root, path.data(), path.size(), value, document, false, nullptr, nullptr);
}

/** Applies the RFC 6902 @p patch to @p root in place, answering the patched root or @c nullptr on failure. */
inline yyjson_mut_val* patch_value(yyjson_mut_doc* document, yyjson_mut_val* root, yyjson_mut_val* patch) noexcept {
    if (!root || !yyjson_mut_is_arr(patch)) return nullptr;

    yyjson_mut_arr_iter iterator;
    yyjson_mut_arr_iter_init(patch, &iterator);
    while (yyjson_mut_val* const operation = yyjson_mut_arr_iter_next(&iterator)) {
        yyjson_mut_val* const name_value = yyjson_mut_obj_get(operation, "op");
        yyjson_mut_val* const path_value = yyjson_mut_obj_get(operation, "path");
        if (!yyjson_mut_is_str(name_value) || !yyjson_mut_is_str(path_value)) return nullptr;
        std::string_view const name {yyjson_mut_get_str(name_value), yyjson_mut_get_len(name_value)};
        std::string_view const path {yyjson_mut_get_str(path_value), yyjson_mut_get_len(path_value)};
        yyjson_mut_val* const value = yyjson_mut_obj_get(operation, "value");
        yyjson_mut_val* const from_value = yyjson_mut_obj_get(operation, "from");
        std::string_view const from = yyjson_mut_is_str(from_value) ? std::string_view {yyjson_mut_get_str(from_value),
                                                                                        yyjson_mut_get_len(from_value)}
                                                                    : std::string_view {};

        if (name == "test") {
            yyjson_mut_val const* const target =
                path.empty() ? root : yyjson_mut_ptr_getx(root, path.data(), path.size(), nullptr, nullptr);
            if (!value || !target || !yyjson_mut_equals(value, target)) return nullptr;
            continue;
        }
        if (name == "remove") {
            if (path.empty() || !yyjson_mut_ptr_removex(root, path.data(), path.size(), nullptr, nullptr))
                return nullptr;
            continue;
        }

        // Every other operation places one value at `path`
        yyjson_mut_val* placed = nullptr;
        if (name == "add" || name == "replace") placed = value ? yyjson_mut_val_mut_copy(document, value) : nullptr;
        else if (name == "copy" && from_value) {
            yyjson_mut_val const* const source =
                from.empty() ? root : yyjson_mut_ptr_getx(root, from.data(), from.size(), nullptr, nullptr);
            placed = source ? yyjson_mut_val_mut_copy(document, source) : nullptr;
        }
        else if (name == "move" && from_value) {
            if (from == path) continue;
            placed = from.empty() ? nullptr : yyjson_mut_ptr_removex(root, from.data(), from.size(), nullptr, nullptr);
        }
        if (!placed) return nullptr;

        if (path.empty()) root = placed;
        else if (name == "replace") {
            if (!yyjson_mut_ptr_replacex(root, path.data(), path.size(), placed, nullptr, nullptr)) return nullptr;
        }
        else if (!patch_add(document, root, path, placed)) return nullptr;
    }
    return root;
}

/** Applies @p modifier to @p field of @p original, or to the whole document when @p field is empty. */
inline status_t modify(json_t& original, yyjson_mut_val* modifier, std::string_view field, modification_t modification,
                       ustore_arena_t arena) noexcept {

    auto [document, status] = json_mutable(original, arena);
    if (status.failed()) return status;

    if (field.empty()) {
        yyjson_mut_val* const root = yyjson_mut_doc_get_root(document);
        yyjson_mut_val* modified = nullptr;
        switch (modification) {
        case modification_t::merge_k: modified = yyjson_mut_merge_patch(document, root, modifier); break;
        case modification_t::patch_k: modified = patch_value(document, root, modifier); break;
        case modification_t::upsert_k:
        case modification_t::update_k:
        case modification_t::insert_k: modified = yyjson_mut_val_mut_copy(document, modifier); break;
        default: break;
        }
        if (!modified && modification == modification_t::patch_k)
            return {status_code_t::invalid_argument_k, "The patch does not apply to the document"};
        if (!modified) return {status_code_t::out_of_memory_k, "Failed to modify the document"};
        yyjson_mut_doc_set_root(document, modified);
        return {};
    }

    auto [pointer, pointer_status] = field_pointer(field, arena);
    if (pointer_status.failed()) return pointer_status;

    yyjson_mut_val* replacement = nullptr;
    switch (modification) {
    case modification_t::merge_k: {
        yyjson_mut_val* const existing = yyjson_mut_doc_ptr_getn(document, pointer.data(), pointer.size());
        replacement = yyjson_mut_merge_patch(document, existing, modifier);
        break;
    }
    case modification_t::patch_k: {
        yyjson_mut_val* const existing = yyjson_mut_doc_ptr_getn(document, pointer.data(), pointer.size());
        replacement = patch_value(document, yyjson_mut_val_mut_copy(document, existing), modifier);
        break;
    }
    default: replacement = yyjson_mut_val_mut_copy(document, modifier); break;
    }
    if (!replacement) return {status_code_t::invalid_argument_k, "The modifier does not apply to the field"};

    bool succeeded = false;
    switch (modification) {
    case modification_t::insert_k:
        succeeded = yyjson_mut_doc_ptr_addn(document, pointer.data(), pointer.size(), replacement);
        break;
    case modification_t::update_k:
        succeeded = yyjson_mut_doc_ptr_replacen(document, pointer.data(), pointer.size(), replacement) != nullptr;
        break;
    default: succeeded = yyjson_mut_doc_ptr_setn(document, pointer.data(), pointer.size(), replacement); break;
    }
    if (!succeeded) return {status_code_t::not_found_k, "The field path does not resolve inside the document"};
    return {};
}

#pragma endregion Modifications

} // namespace unum::ustore::docs
