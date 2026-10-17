/**
 *  @file include/ustore/docs/json_fold.hpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief The docs fold: a whole document as the base, and deltas that each apply one modification of
 *      @c ustore_docs_write to one field, replayed in commit order and composed without a base where that is sound.
 *
 *  A delta carries its commit stamp, its modification, its field and the JSON text of its value, and a chain of them
 *  is what a @c delta_kind_t::change_k record holds. Beyond the five modifications of @c ustore_docs_write, a delta may
 *  write a whole document as it is, as a whole JSON upsert and @c ustore_blobs_write store it, or remove it.
 *
 *  Each modification replays exactly as `src/docs.cpp` reads, modifies and writes back a document today. A field of a
 *  missing document, and an update or a patch of a whole missing document, fail with @c status_code_t::not_found_k,
 *  and inserting a whole document that exists fails with @c status_code_t::already_exists_k. Then the document and
 *  the value parse, so either one malformed fails with @c status_code_t::malformed_k, and a patch whose test or path
 *  fails answers @c status_code_t::invalid_argument_k. A write, a removal and an upsert of a whole document close the
 *  chain: no base or older delta matters after them, so they also clear a failure older deltas left, as an LSM that
 *  drops what they close would.
 *
 *  Composition keeps the chain minus what merges soundly for every base. After the newest closing delta the document
 *  is known, so the chain folds into one write or removal, unless a delta fails there. Before it, an upsert or an
 *  RFC 7386 merge of a field absorbs the newer upserts and merges of the same field, as long as every delta between
 *  them modifies a disjoint field, keeping its own position in the document. Two merges compose only where one merge
 *  patch expresses both: a member the older patch removes or replaces by a scalar cannot be merged into again. Adjacent
 *  RFC 6902 patches of one field concatenate their operations. Composition keeps the newest stamp of what it merges.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <yyjson.h> // `yyjson_mut_merge_patch`, `yyjson_mut_obj`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/docs/json_patch.hpp"       // `modify`, `modification_t`
#include "ustore/docs/json_values.hpp"      // `json_parse`, `any_parse`, `json_mutable`, `json_dump`
#include "ustore/utilities/folds.hpp"       // `delta_kind_t`, `delta_view_t`, `fold_concept`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`, `arena_allocate`, `growth_status`

namespace unum::ustore::docs {

using utilities::arena_allocator;
using utilities::delta_kind_t;
using utilities::delta_view_t;
using utilities::growth_status;

#pragma region Deltas

/** What one delta does: the modifications of @c ustore_docs_modification_t by their numbers, then writes and
 *  removals of whole documents. */
enum class delta_modification_t : std::uint8_t {
    upsert_k = ustore_docs_modify_upsert_k,
    update_k = ustore_docs_modify_update_k,
    insert_k = ustore_docs_modify_insert_k,
    patch_k = ustore_docs_modify_patch_k,
    merge_k = ustore_docs_modify_merge_k,
    write_k = 5,
    remove_k = 6,
};

/** One modification of one field of a document, as a docs collection logs it. */
struct json_delta_t {

    /** The commit that wrote the delta. */
    std::uint64_t stamp = 0;

    /** What the delta does. */
    delta_modification_t modification = delta_modification_t::upsert_k;

    /** A top-level key or a JSON Pointer, where an empty field addresses the whole document. */
    std::string_view field;

    /** The JSON text of the value, or the stored bytes of a write. */
    std::string_view value;
};

/** Bytes before the field of an encoded delta: the stamp, the modification, and the lengths of the field and value. */
constexpr std::size_t delta_header_bytes_k = sizeof(std::uint64_t) + 1 + 2 * sizeof(std::uint32_t);

/** The bytes @p text spells. */
inline std::span<byte_t const> bytes_of_text(std::string_view text) noexcept {
    return {reinterpret_cast<byte_t const*>(text.data()), text.size()};
}

/** The text @p bytes spell. */
inline std::string_view text_of_bytes(std::span<byte_t const> bytes) noexcept {
    return {reinterpret_cast<char const*>(bytes.data()), bytes.size()};
}

/** Whether @p delta makes the base and every older delta irrelevant: a write, a removal or a whole upsert. */
inline bool closes(json_delta_t const& delta) noexcept {
    return delta.modification == delta_modification_t::write_k ||
           delta.modification == delta_modification_t::remove_k ||
           (delta.modification == delta_modification_t::upsert_k && delta.field.empty());
}

/** Bytes @p delta takes encoded. */
inline std::size_t encoded_size(json_delta_t const& delta) noexcept {
    return delta_header_bytes_k + delta.field.size() + delta.value.size();
}

/** Encodes @p delta at @p output in host byte order, answering where it ends. */
inline byte_t* encode_into(json_delta_t const& delta, byte_t* output) noexcept {
    auto const field_length = static_cast<std::uint32_t>(delta.field.size());
    auto const value_length = static_cast<std::uint32_t>(delta.value.size());
    auto const modification = static_cast<std::uint8_t>(delta.modification);
    std::memcpy(output, &delta.stamp, sizeof(delta.stamp)), output += sizeof(delta.stamp);
    std::memcpy(output, &modification, 1), output += 1;
    std::memcpy(output, &field_length, sizeof(field_length)), output += sizeof(field_length);
    std::memcpy(output, &value_length, sizeof(value_length)), output += sizeof(value_length);
    if (field_length) std::memcpy(output, delta.field.data(), field_length), output += field_length;
    if (value_length) std::memcpy(output, delta.value.data(), value_length), output += value_length;
    return output;
}

/** Encodes @p deltas, oldest first, into one chain in @p arena. */
inline expected<std::span<byte_t const>> encode_deltas(std::span<json_delta_t const> deltas,
                                                       ustore_arena_t arena) noexcept {
    std::size_t total = 0;
    for (json_delta_t const& delta : deltas) {
        if (delta.field.size() > UINT32_MAX || delta.value.size() > UINT32_MAX)
            return status_t {status_code_t::invalid_argument_k, "A delta's field or value passes 4 GiB"};
        total += encoded_size(delta);
    }
    auto [chain, status] = utilities::arena_allocate<byte_t>(arena, total);
    if (status.failed()) return status;
    byte_t* output = chain.data();
    for (json_delta_t const& delta : deltas) output = encode_into(delta, output);
    return std::span<byte_t const>(chain.data(), total);
}

/** Decodes the delta that starts @p bytes into @p delta, viewing its field and value in place, answering its size. */
inline expected<std::size_t> decode_delta(std::span<byte_t const> bytes, json_delta_t& delta) noexcept {
    if (bytes.size() < delta_header_bytes_k) return status_t {status_code_t::malformed_k, "A delta is truncated"};
    std::uint8_t modification = 0;
    std::uint32_t field_length = 0, value_length = 0;
    byte_t const* input = bytes.data();
    std::memcpy(&delta.stamp, input, sizeof(delta.stamp)), input += sizeof(delta.stamp);
    std::memcpy(&modification, input, 1), input += 1;
    std::memcpy(&field_length, input, sizeof(field_length)), input += sizeof(field_length);
    std::memcpy(&value_length, input, sizeof(value_length)), input += sizeof(value_length);
    if (modification > static_cast<std::uint8_t>(delta_modification_t::remove_k))
        return status_t {status_code_t::malformed_k, "A delta has an unknown modification"};
    std::size_t const size = delta_header_bytes_k + std::size_t(field_length) + std::size_t(value_length);
    if (bytes.size() < size) return status_t {status_code_t::malformed_k, "A delta is truncated"};
    delta.modification = static_cast<delta_modification_t>(modification);
    delta.field = {reinterpret_cast<char const*>(input), field_length};
    delta.value = {reinterpret_cast<char const*>(input) + field_length, value_length};
    return size;
}

/** Calls @p callback with every delta of the encoded @p chain, oldest first, stopping at its first failure. */
template <typename callback_type_>
status_t for_each_delta(std::span<byte_t const> chain, callback_type_&& callback) noexcept {
    while (!chain.empty()) {
        json_delta_t delta;
        auto [size, status] = decode_delta(chain, delta);
        if (status.failed()) return status;
        if (status_t const called = callback(delta); called.failed()) return called;
        chain = chain.subspan(size);
    }
    return {};
}

/** The JSON text a delta carries for @p value of @p type: JSON as written, and any other type printed as JSON. */
inline expected<std::string_view> modifier_text(value_view_t value, ustore_docs_field_type_t type,
                                                ustore_arena_t arena) noexcept {
    if (type == ustore_docs_field_type_json_k) return std::string_view(value);
    auto [modifier, status] = any_parse(value, type, arena);
    if (status.failed()) return status;
    auto [text, dump_status] = json_dump({nullptr, yyjson_mut_doc_get_root(modifier.mutable_handle)}, arena);
    if (dump_status.failed()) return dump_status;
    return std::string_view(text);
}

#pragma endregion Deltas

#pragma region Replay

/** A document being folded, where null bytes stand for a missing document and empty ones for an empty document. */
struct document_state_t {
    std::span<byte_t const> bytes;

    bool exists() const noexcept { return bytes.data() != nullptr; }
};

/** The bytes an existing empty document views, so they never read as missing. */
inline constexpr byte_t empty_document_k[1] = {};

/** Applies @p delta to @p document as `src/docs.cpp` reads, modifies and writes it back, in @p arena. */
inline status_t apply_delta(document_state_t& document, json_delta_t const& delta, ustore_arena_t arena) noexcept {
    switch (delta.modification) {
    case delta_modification_t::write_k:
        document.bytes = delta.value.empty() ? std::span<byte_t const>(empty_document_k, 0)
                                             : bytes_of_text(delta.value);
        return {};
    case delta_modification_t::remove_k: document.bytes = {}; return {};
    default: break;
    }

    auto const modification = static_cast<modification_t>(delta.modification);
    bool const whole = delta.field.empty();
    if (!document.exists() && !whole)
        return {status_code_t::not_found_k, "Modified field belongs to a missing document"};
    if (!document.exists() && (modification == modification_t::update_k || modification == modification_t::patch_k))
        return {status_code_t::not_found_k, "Updated or patched document is missing"};
    if (document.exists() && whole && modification == modification_t::insert_k)
        return {status_code_t::already_exists_k, "Inserted document already exists"};

    value_view_t const base = document.exists() ? value_view_t(text_of_bytes(document.bytes)) : value_view_t();
    auto [parsed, status] = json_parse(base, arena);
    if (status.failed()) return status;
    auto [modifier, modifier_status] = any_parse(value_view_t(delta.value), ustore_docs_field_type_json_k, arena);
    if (modifier_status.failed()) return modifier_status;
    if (!modifier) return {status_code_t::malformed_k, "An empty value is no JSON document"};
    auto [modifier_document, mutable_status] = json_mutable(modifier, arena);
    if (mutable_status.failed()) return mutable_status;
    if (status_t const modified = modify(parsed, yyjson_mut_doc_get_root(modifier_document), delta.field, modification,
                                         arena);
        modified.failed())
        return modified;
    auto [text, dump_status] = json_dump({nullptr, yyjson_mut_doc_get_root(parsed.mutable_handle)}, arena);
    if (dump_status.failed()) return dump_status;
    document.bytes = text.empty() ? std::span<byte_t const>(empty_document_k, 0) : bytes_of_text(text);
    return {};
}

/** Deltas replayed in commit order: the document so far and the failure that stopped it, which a closing delta
 *  clears. */
struct replay_t {
    document_state_t document;
    status_t failure;

    void apply(json_delta_t const& delta, ustore_arena_t arena) noexcept {
        if (closes(delta)) document = {}, failure = {};
        else if (failure.failed()) return;
        failure = apply_delta(document, delta, arena);
    }
};

/** Calls @p callback with every delta @p deltas hold, oldest first, a write or a removal as a delta of its own. */
template <typename callback_type_>
status_t for_each_delta(std::span<delta_view_t const> deltas, callback_type_&& callback) noexcept {
    for (delta_view_t const& delta : deltas) {
        status_t status;
        switch (delta.kind) {
        case delta_kind_t::write_k:
            status = callback(json_delta_t {0, delta_modification_t::write_k, {}, text_of_bytes(delta.bytes)});
            break;
        case delta_kind_t::remove_k: status = callback(json_delta_t {0, delta_modification_t::remove_k, {}, {}}); break;
        case delta_kind_t::change_k: status = for_each_delta(delta.bytes, callback); break;
        default: status = {status_code_t::malformed_k, "A delta has an unknown kind"}; break;
        }
        if (status.failed()) return status;
    }
    return {};
}

#pragma endregion Replay

#pragma region Fields

/** One reference token of a field: a top-level key as written, or a JSON Pointer token with its escapes. */
struct field_token_t {
    std::string_view text;
    bool escaped = false;

    /** Whether an array would read it as an index or as the end, and not as a member. */
    bool reads_as_index() const noexcept {
        if (text == "-") return true;
        if (text.empty()) return false;
        for (char const character : text)
            if (character < '0' || character > '9') return false;
        return true;
    }

    /** Whether it spells the same name as @p other once both are unescaped. */
    bool same_name(field_token_t const& other) const noexcept {
        std::size_t index = 0, other_index = 0;
        auto const next = [](field_token_t const& token, std::size_t& position) noexcept {
            char const character = token.text[position++];
            if (!token.escaped || character != '~') return character;
            return token.text[position++] == '0' ? '~' : '/';
        };
        while (index != text.size() && other_index != other.text.size())
            if (next(*this, index) != next(other, other_index)) return false;
        return index == text.size() && other_index == other.text.size();
    }
};

/** The reference tokens of a field, read in place. */
class field_tokens_t {
    std::string_view rest_;
    bool escaped_ = false;
    bool done_ = true;

  public:
    explicit field_tokens_t(std::string_view field) noexcept
        : rest_(field.starts_with('/') ? field.substr(1) : field), escaped_(field.starts_with('/')),
          done_(field.empty()) {}

    /** Reads the next token into @p token, answering false past the last. */
    bool next(field_token_t& token) noexcept {
        if (done_) return false;
        if (!escaped_) {
            token = {rest_, false}, done_ = true;
            return true;
        }
        std::size_t const end = rest_.find('/');
        token = {rest_.substr(0, end), true};
        if (end == std::string_view::npos) done_ = true;
        else rest_ = rest_.substr(end + 1);
        return true;
    }
};

/** Whether every escape of @p field is one RFC 6901 knows, so its tokens compare by name. */
inline bool well_escaped(std::string_view field) noexcept {
    if (!field.starts_with('/')) return true;
    for (std::size_t index = 0; index != field.size(); ++index)
        if (field[index] == '~' && (index + 1 == field.size() || (field[index + 1] != '0' && field[index + 1] != '1')))
            return false;
    return true;
}

/** How two fields relate: one names the other, they share neither path nor position, or they may interact. */
enum class fields_relation_t : std::uint8_t {
    same_k,
    disjoint_k,
    entangled_k,
};

/**
 *  @brief How modifying @p first and @p second interacts, judged without the document.
 *
 *  Two fields are disjoint when neither holds the other and the first tokens they differ in cannot both index one
 *  array, since an insertion into an array moves every later element. The whole document holds every field.
 */
inline fields_relation_t relate_fields(std::string_view first, std::string_view second) noexcept {
    if (!well_escaped(first) || !well_escaped(second)) return fields_relation_t::entangled_k;
    if (first.empty() || second.empty())
        return first.empty() && second.empty() ? fields_relation_t::same_k : fields_relation_t::entangled_k;
    field_tokens_t first_tokens(first), second_tokens(second);
    field_token_t first_token, second_token;
    while (true) {
        bool const first_has = first_tokens.next(first_token);
        bool const second_has = second_tokens.next(second_token);
        if (!first_has || !second_has)
            return first_has == second_has ? fields_relation_t::same_k : fields_relation_t::entangled_k;
        if (first_token.same_name(second_token)) continue;
        return first_token.reads_as_index() && second_token.reads_as_index() ? fields_relation_t::entangled_k
                                                                             : fields_relation_t::disjoint_k;
    }
}

#pragma endregion Fields

#pragma region Composition

/** The value @p text parses into, or null for a value that does not parse, as a delta's modifier reads it. */
inline yyjson_mut_val* parse_modifier(std::string_view text, yyjson_mut_doc* document, ustore_arena_t arena) noexcept {
    auto [parsed, status] = json_parse(value_view_t(text), arena);
    if (status.failed() || !parsed.handle) return nullptr;
    return yyjson_val_mut_copy(document, yyjson_doc_get_root(parsed.handle));
}

/** Whether the object @p object repeats a key. */
inline bool repeats_keys(yyjson_mut_val* object) noexcept {
    yyjson_mut_obj_iter outer;
    yyjson_mut_obj_iter_init(object, &outer);
    while (yyjson_mut_val* const key = yyjson_mut_obj_iter_next(&outer)) {
        std::size_t matches = 0;
        yyjson_mut_obj_iter inner;
        yyjson_mut_obj_iter_init(object, &inner);
        while (yyjson_mut_val* const other = yyjson_mut_obj_iter_next(&inner)) matches += yyjson_mut_equals(key, other);
        if (matches > 1) return true;
    }
    return false;
}

/**
 *  @brief The RFC 7386 patch that merges like @p older and then @p newer into every value, built in @p document.
 *  @return The patch, null where no single patch exists, or a failure where memory runs out.
 *
 *  yyjson keeps the members a patch leaves alone first, in their order, and appends the patched ones in the patch's
 *  order, so the composed patch lists the members only @p older names, then those of @p newer.
 */
inline expected<yyjson_mut_val*> compose_merges(yyjson_mut_doc* document, yyjson_mut_val* older,
                                                yyjson_mut_val* newer) noexcept {
    status_t const out_of_memory {status_code_t::out_of_memory_k, "Failed to compose merges"};
    if (!yyjson_mut_is_obj(newer)) {
        yyjson_mut_val* const copy = yyjson_mut_val_mut_copy(document, newer);
        if (!copy) return out_of_memory;
        return copy;
    }
    if (!yyjson_mut_is_obj(older) || repeats_keys(older) || repeats_keys(newer)) return nullptr;

    yyjson_mut_val* const composed = yyjson_mut_obj(document);
    if (!composed) return out_of_memory;
    auto const add = [&](yyjson_mut_val* key, yyjson_mut_val* value) noexcept {
        yyjson_mut_val* const key_copy = yyjson_mut_val_mut_copy(document, key);
        return key_copy && value && yyjson_mut_obj_add(composed, key_copy, value);
    };

    yyjson_mut_obj_iter iterator;
    yyjson_mut_obj_iter_init(older, &iterator);
    while (yyjson_mut_val* const key = yyjson_mut_obj_iter_next(&iterator)) {
        if (yyjson_mut_obj_getn(newer, yyjson_mut_get_str(key), yyjson_mut_get_len(key))) continue;
        if (!add(key, yyjson_mut_val_mut_copy(document, yyjson_mut_obj_iter_get_val(key)))) return out_of_memory;
    }
    yyjson_mut_obj_iter_init(newer, &iterator);
    while (yyjson_mut_val* const key = yyjson_mut_obj_iter_next(&iterator)) {
        yyjson_mut_val* const newer_value = yyjson_mut_obj_iter_get_val(key);
        yyjson_mut_val* const older_value = yyjson_mut_obj_getn(older, yyjson_mut_get_str(key),
                                                                yyjson_mut_get_len(key));
        yyjson_mut_val* value = nullptr;
        if (!older_value || yyjson_mut_is_null(newer_value)) value = yyjson_mut_val_mut_copy(document, newer_value);
        else if (yyjson_mut_is_null(older_value)) {
            // The older patch removed the member, and a newer object merges into nothing rather than into the base
            if (yyjson_mut_is_obj(newer_value)) return nullptr;
            value = yyjson_mut_val_mut_copy(document, newer_value);
        }
        else {
            auto [nested, status] = compose_merges(document, older_value, newer_value);
            if (status.failed()) return status;
            if (!nested) return nullptr;
            value = nested;
        }
        if (!add(key, value)) return out_of_memory;
    }
    return composed;
}

/** Prints @p value as the JSON text a delta carries. */
inline expected<std::string_view> print_modifier(yyjson_mut_val* value, ustore_arena_t arena) noexcept {
    auto [text, status] = json_dump({nullptr, value}, arena);
    if (status.failed()) return status;
    return std::string_view(text);
}

/** Merges the deltas of @p deltas that @p keep marks into each other wherever that holds for every base, unmarking
 *  the absorbed ones, for a chain that holds no closing delta. */
template <typename deltas_type_, typename keep_type_>
status_t merge_open_chain(deltas_type_& deltas, keep_type_& keep, ustore_arena_t arena) noexcept {
    yyjson_alc const allocator = json_allocator(arena);
    yyjson_mut_doc* const document = yyjson_mut_doc_new(&allocator);
    if (!document) return {status_code_t::out_of_memory_k, "Failed to compose deltas"};
    std::size_t const count = deltas.size();
    auto const sets_field = [](delta_modification_t modification) noexcept {
        return modification == delta_modification_t::upsert_k || modification == delta_modification_t::merge_k;
    };

    // An upsert or a merge of a field absorbs the newer upserts and merges of it past deltas of disjoint fields
    for (std::size_t older = 0; older != count; ++older) {
        if (!keep[older] || !sets_field(deltas[older].modification)) continue;
        for (std::size_t newer = older + 1; newer != count; ++newer) {
            if (!keep[newer]) continue;
            json_delta_t const candidate = deltas[newer];
            fields_relation_t const relation = relate_fields(deltas[older].field, candidate.field);
            if (relation == fields_relation_t::disjoint_k) continue;
            if (relation != fields_relation_t::same_k || !sets_field(candidate.modification)) break;
            yyjson_mut_val* const older_value = parse_modifier(deltas[older].value, document, arena);
            yyjson_mut_val* const newer_value = parse_modifier(candidate.value, document, arena);
            if (!older_value || !newer_value) break;

            json_delta_t merged = deltas[older];
            merged.stamp = candidate.stamp;
            // A merge of a field with anything but an object sets the field, as an upsert does
            bool const older_sets = merged.modification == delta_modification_t::upsert_k ||
                                    (!merged.field.empty() && !yyjson_mut_is_obj(older_value));
            if (candidate.modification == delta_modification_t::upsert_k) {
                merged.modification = delta_modification_t::upsert_k;
                merged.value = candidate.value;
            }
            else if (older_sets) {
                yyjson_mut_val* const result = yyjson_mut_merge_patch(document, older_value, newer_value);
                if (!result) return {status_code_t::out_of_memory_k, "Failed to compose merges"};
                auto [text, status] = print_modifier(result, arena);
                if (status.failed()) return status;
                merged.modification = delta_modification_t::upsert_k;
                merged.value = text;
            }
            else {
                auto [composed, status] = compose_merges(document, older_value, newer_value);
                if (status.failed()) return status;
                if (!composed) break;
                auto [text, print_status] = print_modifier(composed, arena);
                if (print_status.failed()) return print_status;
                merged.value = text;
            }
            deltas[older] = merged;
            keep[newer] = 0;
        }
    }

    // Adjacent patches of one field run as one patch listing the operations of both
    for (std::size_t older = 0; older != count; ++older) {
        if (!keep[older] || deltas[older].modification != delta_modification_t::patch_k) continue;
        for (std::size_t newer = older + 1; newer != count; ++newer) {
            if (!keep[newer]) continue;
            json_delta_t const candidate = deltas[newer];
            if (candidate.modification != delta_modification_t::patch_k ||
                relate_fields(deltas[older].field, candidate.field) != fields_relation_t::same_k)
                break;
            yyjson_mut_val* const older_value = parse_modifier(deltas[older].value, document, arena);
            yyjson_mut_val* const newer_value = parse_modifier(candidate.value, document, arena);
            if (!yyjson_mut_is_arr(older_value) || !yyjson_mut_is_arr(newer_value)) break;
            yyjson_mut_arr_iter iterator;
            yyjson_mut_arr_iter_init(newer_value, &iterator);
            while (yyjson_mut_val* const operation = yyjson_mut_arr_iter_next(&iterator))
                if (!yyjson_mut_arr_append(older_value, yyjson_mut_val_mut_copy(document, operation)))
                    return {status_code_t::out_of_memory_k, "Failed to concatenate patches"};
            auto [text, status] = print_modifier(older_value, arena);
            if (status.failed()) return status;
            deltas[older].value = text, deltas[older].stamp = candidate.stamp;
            keep[newer] = 0;
        }
    }
    return {};
}

#pragma endregion Composition

#pragma region Fold

/** The fold of docs, fitting the fold concept of `ustore/utilities/folds.hpp`: apply, compose and closes. */
struct json_fold {

    /**
     *  @brief The document @p base becomes once every delta of @p deltas applies, oldest first.
     *  @return @p base itself without deltas, the document in @p arena, @c status_code_t::not_found_k for a missing
     *      document, or the failure of the first delta that fails after the newest closing one.
     */
    expected<std::span<byte_t const>> apply(std::span<byte_t const> base, std::span<delta_view_t const> deltas,
                                            arena_allocator<byte_t>& allocator) const noexcept {
        replay_t replay;
        replay.document.bytes = base;
        status_t const status = for_each_delta(deltas, [&](json_delta_t const& delta) noexcept -> status_t {
            replay.apply(delta, allocator.arena);
            return {};
        });
        if (status.failed()) return status;
        if (replay.failure.failed()) return replay.failure;
        if (!replay.document.exists()) return status_t {status_code_t::not_found_k, "The document does not exist"};
        return replay.document.bytes;
    }

    /** Whether @p delta makes every older record irrelevant: a write, a removal, or a chain holding a whole upsert,
     *  a write or a removal. */
    bool closes(delta_view_t const& delta) const noexcept {
        if (delta.kind != delta_kind_t::change_k) return true;
        bool closing = false;
        status_t const status = for_each_delta(delta.bytes, [&](json_delta_t const& record) noexcept -> status_t {
            closing = closing || docs::closes(record);
            return {};
        });
        return status.succeeded() && closing;
    }

    /**
     *  @brief One chain of deltas equivalent to @p deltas on every base, as compaction stores it.
     *  @return The chain in the arena of @p allocator.
     */
    expected<std::span<byte_t const>> compose(std::span<delta_view_t const> deltas,
                                              arena_allocator<byte_t>& allocator) const noexcept {
        ustore_arena_t const arena = allocator.arena;
        st::basic_vector<json_delta_t, arena_allocator<json_delta_t>> chain {arena_allocator<json_delta_t>(arena)};
        st::basic_vector<std::uint8_t, arena_allocator<std::uint8_t>> keep {arena_allocator<std::uint8_t>(arena)};
        status_t status = for_each_delta(deltas, [&](json_delta_t const& delta) noexcept -> status_t {
            return growth_status(chain.push_back(json_delta_t(delta)));
        });
        if (status.failed()) return status;
        if (chain.empty()) return status_t {status_code_t::invalid_argument_k, "No deltas to compose"};

        std::size_t closing = chain.size();
        for (std::size_t index = chain.size(); index-- && closing == chain.size();)
            if (docs::closes(chain[index])) closing = index;

        // Past a closing delta the document is known, so the deltas fold into it, unless one of them fails
        if (closing != chain.size()) {
            replay_t replay;
            for (std::size_t index = closing; index != chain.size(); ++index) replay.apply(chain[index], arena);
            std::span<json_delta_t const> const rest(chain.data() + closing, chain.size() - closing);
            if (replay.failure.failed()) return encode_deltas(rest, arena);
            json_delta_t folded {chain[chain.size() - 1].stamp, delta_modification_t::remove_k, {}, {}};
            if (replay.document.exists())
                folded.modification = delta_modification_t::write_k,
                folded.value = text_of_bytes(replay.document.bytes);
            return encode_deltas(std::span<json_delta_t const>(&folded, 1), arena);
        }

        if (status = growth_status(keep.resize(chain.size(), 1)); status.failed()) return status;
        if (status = merge_open_chain(chain, keep, arena); status.failed()) return status;
        std::size_t kept = 0;
        for (std::size_t index = 0; index != chain.size(); ++index)
            if (keep[index]) chain[kept++] = chain[index];
        return encode_deltas(std::span<json_delta_t const>(chain.data(), kept), arena);
    }
};

static_assert(utilities::fold_concept<json_fold>);

#pragma endregion Fold

} // namespace unum::ustore::docs
