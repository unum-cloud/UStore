/**
 *  @file bench/docs_paths.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Benchmarks resolving document fields with yyjson's parse and lookup against one StringZilla subdocuments
 *      pass, through the same conversions a docs gather and a docs read make.
 *
 *  Generated tweet-shaped documents sit back to back in one tape, as a read answers them. Each call takes the next
 *  batch of them and either parses every document with yyjson and looks each field up, or resolves every field with
 *  `extract_fields` from `ustore/docs/json_paths.hpp`; both then fill the same columns, so the gap is the cost of
 *  locating the fields. No engine is opened, so storage reads stay out of the timings.
 *
 *  Beside the flags of @c bench/harness.hpp, whose @c --seed also seeds the documents, it takes:
 *
 *  Flag            Default   Meaning
 *  `--documents`   `100000`  Documents generated into the tape
 *  `--batch-size`  `1024`    Documents each call resolves
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `stderr`

#include <array>       // `std::array`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <fmt/format.h>
#include <yyjson.h>

#include <smashtable/basic_vector.hpp>
#include <stringzilla/capabilities.h> // `sz_capabilities_compiled_cpu_mask`, `sz_capabilities_enabled_cpu`

#include "ustore/docs/json_paths.hpp"       // `extract_fields`, `json_documents_t`
#include "ustore/docs/json_values.hpp"      // `json_parse`, `json_lookup`, `gather_scalar`, `json_to_string`
#include "ustore/utilities/ram_buffers.hpp" // `arena_rewind`, `arena_free`, `growing_strings`, `strings_ladder`

#include "harness.hpp"

namespace unum::ustore::bench {

using namespace unum::ustore::docs;

/** The widest StringZilla kernels this build compiled. */
using strings_kit_t = utilities::strings_ladder<sz_capabilities_compiled_cpu_mask()>::strings_kit_t;

/** The StringZilla capabilities this machine runs among those the build compiled, which the engines
 *  compile for, while @c strings_kit_t keeps to a tier the build's own flags guarantee. */
sz_capability_t strings_capabilities() noexcept {
    sz_capability_t capabilities = sz_cap_serial_k;
    if (sz_capabilities_enabled_cpu(&capabilities) != sz_success_k) capabilities = sz_cap_serial_k;
    return capabilities;
}

#pragma region Settings

/** Command-line options of the benchmark, each field set by the flag of the same name. */
struct settings_t : shared_settings_t {

    /** Documents generated into the tape. */
    count_t documents {.value = 100'000, .minimum = 1};

    /** Documents each call resolves. */
    count_t batch_size {.value = 1024, .minimum = 1};
};

#pragma endregion Settings

#pragma region Documents

/** Every generated document back to back, behind 64-bit offsets, as a read answers them. */
struct corpus_t {
    st::vector<char> bytes;
    st::vector<std::uint64_t> offsets;

    /** The documents @b [first,first+count) as a read would answer them, all present. */
    json_documents_t batch(std::size_t first, std::size_t count) const noexcept {
        json_documents_t documents;
        documents.tape = u64_tape(bytes.data(), offsets.data() + first);
        documents.count = count;
        return documents;
    }

    /** Bytes of the documents @b [first,first+count). */
    std::size_t bytes_in(std::size_t first, std::size_t count) const noexcept {
        return offsets[first + count] - offsets[first];
    }
};

/** Appends @p text to @p bytes, answering whether it fit. */
bool append(st::vector<char>& bytes, std::string_view text) noexcept {
    std::size_t const offset = bytes.size();
    if (st::failed(bytes.resize(offset + text.size()))) return false;
    std::memcpy(bytes.data() + offset, text.data(), text.size());
    return true;
}

/** Writes one tweet's members without its closing brace into @p buffer, answering their length, or zero when they do
 *  not fit: identifiers, a text of words with an escape in one of eight, counters, a score, a user with a profile,
 *  hashtags and URLs. */
std::size_t write_tweet(splitmix64_t& random, std::uint64_t id, char* buffer, std::size_t capacity) noexcept {
    constexpr std::string_view words_k[] = {"the",   "ustore", "json",  "fast",    "graph", "query", "vector",
                                            "batch", "\\n",    "\\\"q", "\\u00e9", "disk",  "arena", "fold"};
    constexpr std::string_view languages_k[] = {"en", "fr", "de", "ja", "es"};
    std::uint64_t const draw = random.next();
    char text[512];
    std::size_t text_length = 0;
    std::size_t const words = 12 + draw % 24;
    for (std::size_t word = 0; word != words; ++word) {
        std::uint64_t const pick = random.next();
        // One document in eight carries escapes in its text
        std::size_t const pool = (draw >> 8) % 8 == 0 ? std::size(words_k) : 8;
        std::string_view const chosen = words_k[pick % pool];
        if (text_length + chosen.size() + 1 >= sizeof(text)) break;
        if (word) text[text_length++] = ' ';
        std::memcpy(text + text_length, chosen.data(), chosen.size());
        text_length += chosen.size();
    }
    std::uint64_t const user = random.next() % 1'000'000;
    auto const
        written =
            fmt::format_to_n(
                buffer, capacity,
                R"({{"id":{0},"id_str":"{0}","created_at":"Fri Oct 16 12:{1:02}:{2:02} +0000 2026","text":"{3}",)" R"("source":"<a href=\"https://ustore.example\" rel=\"nofollow\">UStore</a>","truncated":false,)" R"("lang":"{4}","retweet_count":{5},"favorite_count":{6},"possibly_sensitive":{7},"score":{8},)" R"("user":{{"id":{9},"id_str":"{9}","screen_name":"user_{9}","name":"User {9}","followers_count":{10},)" R"("friends_count":{11},"statuses_count":{12},"verified":{7},"location":"London, UK",)" R"("description":"Writes about databases, storage engines and the occasional JSON parser.",)" R"("url":"https://t.co/u{9}","created_at":"Mon Jan 01 00:00:00 +0000 2018","time_zone":null,)" R"("profile_image_url_https":"https://pbs.twimg.com/profile_images/{9}/photo_normal.jpg",)" R"("default_profile":true,"protected":false}},"entities":{{"hashtags":[{{"text":"tag{5}","indices":[3,9]}},)" R"({{"text":"ustore","indices":[12,19]}}],"user_mentions":[{{"screen_name":"user_{11}","id":{11},)" R"("indices":[20,30]}}],"urls":[{{"url":"https://t.co/{13:x}","expanded_url":"https://example.com/{9}/{0}",)" R"("indices":[40,63]}}]}},"coordinates":null,"place":null,"in_reply_to_status_id":null)",
                id, draw % 60, (draw >> 6) % 60, std::string_view {text, text_length},
                languages_k[(draw >> 12) % std::size(languages_k)], (draw >> 16) % 5000, (draw >> 28) % 20000,
                (draw >> 40) % 2 ? "true" : "false", static_cast<double>(draw % 100'000) / 1000.0, user,
                random.next() % 10'000'000, random.next() % 5000, random.next() % 100'000, random.next());
    return written.size < capacity ? written.size : 0;
}

/** Generates @p count tweets of about 2.5 KB each, half of them retweets holding the tweet they retweet. */
bool generate_corpus(std::size_t count, std::uint32_t seed, corpus_t& corpus) noexcept {
    splitmix64_t random {stream_key(seed, "docs_paths", 0)};
    if (st::failed(corpus.offsets.push_back(0))) return false;
    char buffer[8192];
    for (std::size_t index = 0; index != count; ++index) {
        std::size_t length = write_tweet(random, 1'000'000'000'000 + index, buffer, sizeof(buffer));
        if (!length) return false;
        if (random.next() % 2) {
            constexpr std::string_view nested_k = R"(,"retweeted_status":)";
            std::memcpy(buffer + length, nested_k.data(), nested_k.size());
            length += nested_k.size();
            std::size_t const nested = write_tweet(random, index, buffer + length, sizeof(buffer) - length - 2);
            if (!nested) return false;
            length += nested;
            buffer[length++] = '}';
        }
        buffer[length++] = '}';
        if (!append(corpus.bytes, {buffer, length})) return false;
        if (st::failed(corpus.offsets.push_back(corpus.bytes.size()))) return false;
    }
    return true;
}

#pragma endregion Documents

#pragma region Columns

/** One gathered column: its field, its type, and where it lands. */
struct column_t {
    std::string_view field;
    ustore_docs_field_type_t type;
};

/** The columns every gather row fills. */
constexpr std::array<column_t, 4> columns_k {{
    {"/user/followers_count", ustore_docs_field_type_u64_k},
    {"retweet_count", ustore_docs_field_type_u32_k},
    {"score", ustore_docs_field_type_f64_k},
    {"/user/screen_name", ustore_docs_field_type_string_k},
}};

/** Converts @p value into cell @p index of a column of @p type, as the docs gather does. */
conversion_t gather_cell(ustore_docs_field_type_t type, json_cell_t const& value, ustore_byte_t* scalars,
                         std::size_t index, growing_strings<arena_allocator<byte_t>>& strings) noexcept {
    switch (type) {
    case ustore_docs_field_type_u64_k: return gather_scalar<std::uint64_t>(value, scalars, index);
    case ustore_docs_field_type_u32_k: return gather_scalar<std::uint32_t>(value, scalars, index);
    case ustore_docs_field_type_f64_k: return gather_scalar<double>(value, scalars, index);
    default: {
        printed_number_buffer_t buffer;
        std::string_view text;
        conversion_t const conversion = json_to_string(value, buffer, text);
        return strings.push_back(bytes_of(text), string_end_t::null_terminated_k).failed() ? conversion_t::collides_k
                                                                                           : conversion;
    }
    }
}

/** Fills every column of @p columns from @p values, laid out field after field, counting the valid cells. */
std::size_t fill_columns(std::span<column_t const> columns, std::span<json_cell_t const> values, std::size_t count,
                         std::span<ustore_byte_t> scalars, ustore_arena_t scratch) noexcept {
    std::size_t valid = 0;
    for (std::size_t column = 0; column != columns.size(); ++column) {
        growing_strings<arena_allocator<byte_t>> strings {scratch};
        for (std::size_t index = 0; index != count; ++index)
            valid += is_valid(
                gather_cell(columns[column].type, values[column * count + index], scalars.data(), index, strings));
    }
    return valid;
}

#pragma endregion Columns

#pragma region Benchmarks

/** The value yyjson parsed @p value into, as an extraction holds it, missing for @c nullptr. */
json_cell_t cell_of_parsed(yyjson_val* value) noexcept {
    json_cell_t cell;
    if (!value) return cell;
    switch (yyjson_get_type(value)) {
    case YYJSON_TYPE_NULL: cell.token = json_token_t::null_k; break;
    case YYJSON_TYPE_BOOL: cell.token = yyjson_get_bool(value) ? json_token_t::true_k : json_token_t::false_k; break;
    case YYJSON_TYPE_STR:
        cell.token = json_token_t::string_k, cell.bytes = {yyjson_get_str(value), yyjson_get_len(value)};
        break;
    case YYJSON_TYPE_OBJ: cell.token = json_token_t::object_k; break;
    case YYJSON_TYPE_ARR: cell.token = json_token_t::array_k; break;
    case YYJSON_TYPE_NUM:
        cell.token = json_token_t::number_k;
        if (yyjson_is_real(value)) cell.number = yyjson_get_real(value);
        else if (yyjson_is_sint(value)) cell.number = yyjson_get_sint(value);
        else cell.number = yyjson_get_uint(value);
        break;
    default: break;
    }
    return cell;
}

/** Locates every field of @p fields in @p documents by parsing each document with yyjson, field after field. */
bool resolve_with_yyjson(json_documents_t const& documents, std::span<std::string_view const> fields,
                         ustore_arena_t scratch, std::span<json_cell_t> values) noexcept {
    for (std::size_t index = 0; index != documents.count; ++index) {
        std::string_view const document = documents[index];
        auto [parsed, status] = json_parse(document, scratch);
        if (status.failed()) return false;
        yyjson_val* const root = yyjson_doc_get_root(parsed.handle);
        for (std::size_t field = 0; field != fields.size(); ++field)
            values[field * documents.count + index] = cell_of_parsed(json_lookup(root, fields[field]));
    }
    return true;
}

/** Extracts every field of @p fields in @p documents with one subdocuments pass. */
bool resolve_with_subdocuments(json_documents_t const& documents, std::span<std::string_view const> fields,
                               ustore_arena_t scratch, std::span<json_cell_t> values) noexcept {
    std::size_t const count = documents.count;
    auto const cell_of = [count](std::size_t cell) noexcept { return field_cell_t {cell % count, cell / count}; };
    json_cells_t cells;
    if (extract_fields<strings_kit_t>(documents, fields, count * fields.size(), cell_of, strings_capabilities(),
                                      scratch, cells)
            .failed())
        return false;
    for (std::size_t cell = 0; cell != count * fields.size(); ++cell) values[cell] = cells[cell];
    return true;
}

/** What resolves a row's fields: one subdocuments pass over the batch, or yyjson
 *  parsing each document. */
enum class resolver_t { yyjson_k, subdocuments_k };

/** Runs one row: each call resolves the next batch of the corpus with @p resolver, then
 *  fills @p columns. */
row_t run_row(executor_t& executor, settings_t const& settings, corpus_t const& corpus, std::string_view name,
              std::span<column_t const> columns, resolver_t resolver) noexcept {
    std::size_t const batch_size = fu::min_of(settings.batch_size.value, settings.documents.value);
    std::array<std::string_view, columns_k.size()> fields;
    for (std::size_t column = 0; column != columns.size(); ++column) fields[column] = columns[column].field;
    std::span<std::string_view const> const used_fields {fields.data(), columns.size()};

    return run_threads(executor, settings, name, [&](loop_t& loop, std::size_t) noexcept {
        ustore_arena_t const scratch = new (std::nothrow) ustore_arena_handle_t {};
        st::vector<json_cell_t> values;
        st::vector<ustore_byte_t> scalars;
        if (!scratch || st::failed(values.resize(batch_size * columns.size())) ||
            st::failed(scalars.resize(batch_size * sizeof(std::uint64_t))))
            return loop.skip("no memory for the batch");
        std::size_t const batches = settings.documents.value / batch_size;
        for (std::size_t call : loop) {
            std::size_t const first = (call % batches) * batch_size;
            json_documents_t const documents = corpus.batch(first, batch_size);
            arena_rewind(scratch);
            bool const resolved = resolver == resolver_t::subdocuments_k
                                      ? resolve_with_subdocuments(documents, used_fields, scratch, values)
                                      : resolve_with_yyjson(documents, used_fields, scratch, values);
            if (!resolved) {
                loop.fail();
                continue;
            }
            std::size_t const valid = fill_columns(columns, values, batch_size, scalars, scratch);
            if (valid != batch_size * columns.size()) loop.fail();
            loop.count(batch_size, corpus.bytes_in(first, batch_size));
        }
        arena_free(scratch);
    });
}

#pragma endregion Benchmarks

} // namespace unum::ustore::bench

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::bench;

    settings_t settings;
    std::array<flag_t, 2> const flags {{
        {"--documents", "count", "documents generated into the tape", &settings.documents},
        {"--batch-size", "count", "documents each call resolves", &settings.batch_size},
    }};
    read_settings(argc, argv, settings, flags);
    auto maybe_executor = executor_t::make(1, caller_exclusivity_t::caller_inclusive_k);
    if (!maybe_executor) {
        fmt::println(stderr, "Failed to pin the calling thread");
        return 1;
    }
    executor_t& executor = *maybe_executor;
    corpus_t corpus;
    if (!generate_corpus(settings.documents.value, settings.seed.value, corpus)) {
        fmt::println(stderr, "Failed to generate the documents");
        return 1;
    }
    fmt::println("- Corpus: {} documents, {} bytes, {:.0f} bytes each", settings.documents.value, corpus.bytes.size(),
                 static_cast<double>(corpus.bytes.size()) / static_cast<double>(settings.documents.value));

    std::span<column_t const> const gathered {columns_k};
    constexpr std::array<column_t, 1> read_k {{{"text", ustore_docs_field_type_string_k}}};
    std::span<column_t const> const read {read_k};
    struct row_spec_t {
        std::string_view name;
        std::span<column_t const> columns;
        resolver_t resolver;
    };
    row_spec_t const rows[] = {
        {"docs_gather_4_fields_yyjson", gathered, resolver_t::yyjson_k},
        {"docs_gather_4_fields_subdocuments", gathered, resolver_t::subdocuments_k},
        {"docs_read_text_yyjson", read, resolver_t::yyjson_k},
        {"docs_read_text_subdocuments", read, resolver_t::subdocuments_k},
    };
    for (row_spec_t const& row : rows)
        if (settings.selects(row.name)) print(run_row(executor, settings, corpus, row.name, row.columns, row.resolver));
    return 0;
}
