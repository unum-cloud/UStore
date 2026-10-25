/**
 *  @file src/docs.cpp
 *  @author Ash Vardanian
 *  @date December 26, 2022
 *  @brief Documents modality on yyjson and StringZilla, over the whole-value verbs of the one engine serving it.
 *
 *  Documents are stored as JSON text.
 *  Whole-document reads and upserts pass straight through to the engine's own unchecked read and write.
 *  Field reads and gathers locate their fields with one StringZilla subdocuments pass over the documents read.
 *  They parse with yyjson only a batch that pass cannot read as yyjson would.
 *  Gists and modifications parse every document with yyjson.
 *  All of it lives in the scratch of the call, so the arena of the request holds only its outputs.
 *  The paths, parsing, conversions and modifications live in `ustore/docs/json_paths.hpp`,
 *  `ustore/docs/json_values.hpp` and `ustore/docs/json_patch.hpp`, and this file implements `c/ustore/docs.h`.
 */

#include "ustore/docs.h" // `ustore_docs_field_type_t`, `ustore_docs_modification_t`

#include "blobs_layers/docs.hpp" // `docs_write_over_blobs`, `docs_read_over_blobs`, `docs_gather_over_blobs`
#include "engine.hpp"            // `engine_t`, `docs_verbs_t`
#include "lib.hpp"               // `route`, `enumerator_spelling`, `name_in`, `value_in`

namespace unum::ustore {

#pragma region Names

/** The canonical spelling of every document field type. */
constexpr enumerator_spelling<ustore_docs_field_type_t> docs_field_type_names_k[] {
    {ustore_docs_field_type_json_k, "json"}, {ustore_docs_field_type_null_k, "null"},
    {ustore_docs_field_type_bool_k, "bool"}, {ustore_docs_field_type_uuid_k, "uuid"},
    {ustore_docs_field_type_f64_k, "f64"},   {ustore_docs_field_type_f32_k, "f32"},
    {ustore_docs_field_type_f16_k, "f16"},   {ustore_docs_field_type_i64_k, "i64"},
    {ustore_docs_field_type_i32_k, "i32"},   {ustore_docs_field_type_i16_k, "i16"},
    {ustore_docs_field_type_i8_k, "i8"},     {ustore_docs_field_type_u64_k, "u64"},
    {ustore_docs_field_type_u32_k, "u32"},   {ustore_docs_field_type_u16_k, "u16"},
    {ustore_docs_field_type_u8_k, "u8"},     {ustore_docs_field_type_bin_k, "bin"},
    {ustore_docs_field_type_str_k, "str"},
};

/** The canonical spelling of every document modification. */
constexpr enumerator_spelling<ustore_docs_modification_t> docs_modification_names_k[] {
    {ustore_docs_modify_upsert_k, "upsert"}, {ustore_docs_modify_update_k, "update"},
    {ustore_docs_modify_insert_k, "insert"}, {ustore_docs_modify_patch_k, "patch"},
    {ustore_docs_modify_merge_k, "merge"},
};

#pragma endregion Names

} // namespace unum::ustore

void ustore_docs_write(ustore_docs_write_t* arguments) { route<&engine_t::docs, &docs_verbs_t::write>(arguments); }
void ustore_docs_read(ustore_docs_read_t* arguments) { route<&engine_t::docs, &docs_verbs_t::read>(arguments); }
void ustore_docs_gist(ustore_docs_gist_t* arguments) { route<&engine_t::docs, &docs_verbs_t::gist>(arguments); }
void ustore_docs_gather(ustore_docs_gather_t* arguments) { route<&engine_t::docs, &docs_verbs_t::gather>(arguments); }
void ustore_docs_remove(ustore_docs_remove_t* arguments) { route<&engine_t::docs, &docs_verbs_t::remove>(arguments); }
void ustore_docs_scan(ustore_docs_scan_t* arguments) { route<&engine_t::docs, &docs_verbs_t::scan>(arguments); }
void ustore_docs_sample(ustore_docs_sample_t* arguments) { route<&engine_t::docs, &docs_verbs_t::sample>(arguments); }

char const* ustore_docs_field_type_name(ustore_docs_field_type_t field_type) {
    return name_in(docs_field_type_names_k, field_type);
}

ustore_docs_field_type_t ustore_docs_field_type_from_name(ustore_bytes_view_t name) {
    return value_in(docs_field_type_names_k, name);
}

char const* ustore_docs_modification_name(ustore_docs_modification_t modification) {
    return name_in(docs_modification_names_k, modification);
}

ustore_docs_modification_t ustore_docs_modification_from_name(ustore_bytes_view_t name) {
    return value_in(docs_modification_names_k, name);
}
