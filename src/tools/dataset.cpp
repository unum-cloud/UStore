
#include <fcntl.h>    // `open` files
#include <sys/stat.h> // `stat` to obtain file metadata
#include <sys/mman.h> // `mmap` to read datasets faster
#include <unistd.h>   // `close` files

#include <ctime>
#include <vector>
#include <cstring>
#include <numeric>
#include <fstream>
#include <algorithm>
#include <filesystem>

#include <arrow/api.h>
#include <arrow/array.h>
#include <arrow/table.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/memory_pool.h>
#include <arrow/csv/api.h>
#include <arrow/csv/writer.h>
#include <arrow/io/api.h>
#include <arrow/io/file.h>
#include <arrow/compute/api_aggregate.h>
#include <parquet/arrow/reader.h>
#include <parquet/stream_writer.h>
#include <parquet/exception.h>

#include <simdjson.h>
#include <fmt/format.h>

#include <ustore/ustore.hpp>
#include <ustore/cpp/ranges.hpp>
#include <ustore/cpp/blobs_range.hpp>   // `keys_stream_t`
#include <../helpers/linked_memory.hpp> // `linked_memory_lock_t`

#include "dataset.h"

using namespace unum::ustore;

// Count of symbols to make json ('"', '"', ':', ',')
constexpr ustore_size_t symbols_count_k = 4;
// Json object open brackets for json and parquet
constexpr ustore_str_view_t prefix_k = "{";
// 2 vertices and 1 edge
constexpr ustore_size_t vertices_edge_k = 3;

using tape_t = ptr_range_gt<ustore_char_t>;
using fields_t = strided_iterator_gt<ustore_str_view_t const>;
using lengths_t = strided_iterator_gt<ustore_length_t const>;
using keys_length_t = std::pair<ustore_key_t*, ustore_size_t>;
using val_t = std::pair<ustore_bytes_ptr_t, ustore_size_t>;
using counts_t = ptr_range_gt<ustore_size_t>;
using chunked_array_t = std::shared_ptr<arrow::ChunkedArray>;
using array_t = std::shared_ptr<arrow::Array>;
using int_builder_t = arrow::NumericBuilder<arrow::Int64Type>;
using docs_t = ptr_range_gt<value_view_t>;
using edges_t = ptr_range_gt<edge_t>;

enum ustore_dataset_ext_t {
    parquet_k = 0,
    csv_k,
    ndjson_k,
    unknown_k,
};
using ext_t = ustore_dataset_ext_t;

#pragma region - Helpers

class arrow_visitor_t {
  public:
    arrow_visitor_t(std::string& json) : json(json) {}

    arrow::Status Visit(arrow::NullArray const& arr) {
        fmt::format_to(std::back_inserter(json), "\"\",");
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::BooleanArray const& arr) {
        fmt::format_to(std::back_inserter(json), "true,");
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::Int8Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Int16Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Int32Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Int64Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::UInt8Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::UInt16Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::UInt32Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::UInt64Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::HalfFloatArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::FloatArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::DoubleArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::StringArray const& arr) { return format_bin_str(arr, idx); }
    arrow::Status Visit(arrow::BinaryArray const& arr) { return format_bin_str(arr, idx); }
    arrow::Status Visit(arrow::LargeStringArray const& arr) { return format_bin_str(arr, idx); }
    arrow::Status Visit(arrow::LargeBinaryArray const& arr) { return format_bin_str(arr, idx); }
    arrow::Status Visit(arrow::FixedSizeBinaryArray const& arr) {
        fmt::format_to(std::back_inserter(json), "{},", reinterpret_cast<char const*>(arr.Value(idx)));
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::Date32Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Date64Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Time32Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Time64Array const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::TimestampArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::DayTimeIntervalArray const& arr) {
        auto ds = arr.Value(idx);
        fmt::format_to(std::back_inserter(json), "{{\"days\":{},\"ms-s\":{}}},", ds.days, ds.milliseconds);
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::MonthDayNanoIntervalArray const& arr) {
        auto mdn = arr.Value(idx);
        fmt::format_to(std::back_inserter(json),
                       "{{\"months\":{},\"days\":{},\"us-s\":{}}},",
                       mdn.months,
                       mdn.days,
                       mdn.nanoseconds);
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::MonthIntervalArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::DurationArray const& arr) { return format(arr, idx); }
    arrow::Status Visit(arrow::Decimal128Array const& arr) {
        fmt::format_to(std::back_inserter(json), "{},", reinterpret_cast<char const*>(arr.Value(idx)));
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::Decimal256Array const& arr) {
        fmt::format_to(std::back_inserter(json), "{},", reinterpret_cast<char const*>(arr.Value(idx)));
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::ListArray const& arr) { return arrow::VisitArrayInline(*arr.values().get(), this); }
    arrow::Status Visit(arrow::LargeListArray const& arr) { return arrow::VisitArrayInline(*arr.values().get(), this); }
    arrow::Status Visit(arrow::MapArray const& arr) { return arrow::VisitArrayInline(*arr.values().get(), this); }
    arrow::Status Visit(arrow::FixedSizeListArray const& arr) {
        return arrow::VisitArrayInline(*arr.values().get(), this);
    }
    arrow::Status Visit(arrow::DictionaryArray const& arr) {
        fmt::format_to(std::back_inserter(json), "{},", arr.GetValueIndex(idx));
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::ExtensionArray const& arr) {
        return arrow::VisitArrayInline(*arr.storage().get(), this);
    }
    arrow::Status Visit(arrow::StructArray const& arr) {
        return arrow::Status(arrow::StatusCode::TypeError, "Not supported type");
    }
    arrow::Status Visit(arrow::SparseUnionArray const& arr) {
        return arrow::Status(arrow::StatusCode::TypeError, "Not supported type");
    }
    arrow::Status Visit(arrow::DenseUnionArray const& arr) {
        return arrow::Status(arrow::StatusCode::TypeError, "Not supported type");
    }

    std::string& json;
    ustore_size_t idx = 0;

  private:
    inline static constexpr ustore_char_t int_to_hex_k[16] = {
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

    inline void char_to_hex(ustore_byte_t const c, ustore_byte_t* hex) noexcept {
        hex[0] = int_to_hex_k[c >> 4];
        hex[1] = int_to_hex_k[c & 0x0F];
    }

    template <typename at>
    arrow::Status format(at const& cont, ustore_size_t idx) {
        fmt::format_to(std::back_inserter(json), "{},", cont.Value(idx));
        return arrow::Status::OK();
    }

    template <typename at>
    arrow::Status format_bin_str(at const& cont, ustore_size_t idx) {
        auto str = cont.Value(idx);
        json.push_back('\"');
        json.reserve(json.size() + str.size());
        for (ustore_size_t ch_idx = 0; ch_idx != str.size(); ++ch_idx) {
            ustore_byte_t ch = str[ch_idx];
            switch (ch) {
            case 34: json += "\\\""; break;
            case 92: json += "\\\\"; break;
            case 8: json += "\\b"; break;
            case 9: json += "\\t"; break;
            case 10: json += "\\n"; break;
            case 12: json += "\\f"; break;
            case 13: json += "\\r"; break;
            case 0: [[fallthrough]];
            case 1: [[fallthrough]];
            case 2: [[fallthrough]];
            case 3: [[fallthrough]];
            case 4: [[fallthrough]];
            case 5: [[fallthrough]];
            case 6: [[fallthrough]];
            case 7: [[fallthrough]];
            case 11: [[fallthrough]];
            case 14: [[fallthrough]];
            case 15: [[fallthrough]];
            case 16: [[fallthrough]];
            case 17: [[fallthrough]];
            case 18: [[fallthrough]];
            case 19: [[fallthrough]];
            case 20: [[fallthrough]];
            case 21: [[fallthrough]];
            case 22: [[fallthrough]];
            case 23: [[fallthrough]];
            case 24: [[fallthrough]];
            case 25: [[fallthrough]];
            case 26: [[fallthrough]];
            case 27: [[fallthrough]];
            case 28: [[fallthrough]];
            case 29: [[fallthrough]];
            case 30: [[fallthrough]];
            case 31: {
                json += "\\u0000";
                auto target_ptr = reinterpret_cast<ustore_byte_t*>(json.data() + json.size() - 2);
                char_to_hex(ch, target_ptr);
                break;
            }
            default: json += ch; [[fallthrough]];
            }
        }
        if (json.back() == '\n')
            json.pop_back();
        json += "\",";
        return arrow::Status::OK();
    }
};

bool strcmp_(ustore_char_t const* lhs, ustore_char_t const* rhs) {
    return std::strcmp(lhs, rhs) == 0;
}

bool strncmp_(ustore_char_t const* lhs, ustore_char_t const* rhs, ustore_size_t sz) {
    return std::strncmp(lhs, rhs, sz) == 0;
}

bool chrcmp_(const ustore_char_t lhs, const ustore_char_t rhs) {
    return lhs == rhs;
}

bool is_json_ptr(ustore_str_view_t field) {
    return chrcmp_(field[0], '/');
}

auto get_time_since_epoch() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

std::string generate_file_name() {
    static std::mutex mtx;
    time_t now = time(0);

    mtx.lock();
    ustore_char_t* out = std::ctime(&now);
    mtx.unlock();

    for (ustore_size_t idx = 0; idx < std::strlen(out); ++idx) {
        if ((out[idx] == ' ') | (out[idx] == ':'))
            out[idx] = '_';
    }
    out[std::strlen(out) - 1] = '\0';

    return fmt::format("{}_{}", out, get_time_since_epoch());
}

template <typename task_at>
void validate_docs_fields(task_at& task) {
    if (!task.fields_count && !task.fields)
        return;
    return_error_if_m(!(!task.fields_count && task.fields),
                      task.error,
                      uninitialized_state_k,
                      "Fields count must be initialized");
    return_error_if_m(!(task.fields_count && !task.fields),
                      task.error,
                      uninitialized_state_k,
                      "Fields must be initialized");
    return_error_if_m(!(task.fields_count && task.fields && !task.fields_stride),
                      task.error,
                      uninitialized_state_k,
                      "Fields stride must be initialized");

    fields_t fields {task.fields, task.fields_stride};
    for (ustore_size_t idx = 0; idx < task.fields_count; ++idx) {
        return_error_if_m(fields[idx] != nullptr, task.error, 0, "Invalid field!");
    }
}

template <typename task_at>
bool validate_graph_fields(task_at& imp_exp, bool is_exp = false) {
    if (!imp_exp.source_id_field) {
        *imp_exp.error = "Invalid source id field";
        return false;
    }
    if (!imp_exp.target_id_field) {
        *imp_exp.error = "Invalid target id field";
        return false;
    }

    if (is_exp) {
        auto check_first_char = [](ustore_char_t ch) {
            return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
        };

        auto check_char = [](ustore_char_t ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || //
                   ch == ' ' || ch == '-' || ch == '_';
        };

        auto validate_field = [=](ustore_str_view_t field) {
            for (ustore_size_t idx = 1; idx < std::strlen(field); ++idx) {
                if (check_char(field[idx]))
                    continue;

                return false;
            }
            return true;
        };

        if (!check_first_char(imp_exp.source_id_field[0])) {
            *imp_exp.error = "(source) field must start with A-Z, a-z, '_'";
            return false;
        }

        if (!check_first_char(imp_exp.target_id_field[0])) {
            *imp_exp.error = "(target) field must start with A-Z, a-z, '_'";
            return false;
        }

        if (imp_exp.edge_id_field) {
            if (!check_first_char(imp_exp.edge_id_field[0])) {
                *imp_exp.error = "(edge) field must start with A-Z, a-z, '_'";
                return false;
            }
        }

        if (!validate_field(imp_exp.source_id_field)) {
            *imp_exp.error = "(source) field can contain A-Z, a-z, 0-9, ' ', '-', '_'";
            return false;
        }

        if (!validate_field(imp_exp.target_id_field)) {
            *imp_exp.error = "(target) field can contain A-Z, a-z, 0-9, ' ', '-', '_'";
            return false;
        }

        if (imp_exp.edge_id_field) {
            if (!validate_field(imp_exp.edge_id_field)) {
                *imp_exp.error = "(edge) field can contain A-Z, a-z, 0-9, ' ', '-', '_'";
                return false;
            }
        }
    }

    return true;
}

void check_for_id_field(ustore_docs_import_t& imp) {
    if (imp.fields == nullptr)
        return;

    bool state = false;
    fields_t fields {imp.fields, imp.fields_stride};
    for (ustore_size_t idx = 0; idx < imp.fields_count; ++idx) {
        if (strcmp_(fields[idx], imp.id_field))
            state = true;
    }
    return_error_if_m(state, imp.error, 0, "Fields must contain id_field");
}

template <typename ustore_docs_task_at>
void prepare_fields(ustore_docs_task_at& c, linked_memory_lock_t& arena, fields_t& fields_output) {

    if (c.fields_count == 1) {
        fields_output = {c.fields, c.fields_stride};
        return;
    }
    fields_t fields {c.fields, c.fields_stride};
    ustore_size_t next_idx = 0;

    auto bitmask = arena.alloc<ustore_octet_t>(c.fields_count, c.error);
    return_if_error_m(c.error);

    for (auto& bit : bitmask)
        bit = 1;

    for (ustore_size_t idx = 0; idx < c.fields_count;) {
        while (!bitmask[idx])
            ++idx;

        next_idx = idx + 1;
        if (chrcmp_(fields[idx][0], '/')) {
            while (next_idx < c.fields_count && strncmp_(fields[idx], fields[next_idx], std::strlen(fields[idx])) &&
                   chrcmp_(fields[next_idx][std::strlen(fields[idx])], '/') && bitmask[next_idx]) {
                bitmask[next_idx] = 0;
                ++next_idx;
            }
        }
        else {
            while (next_idx < c.fields_count && !chrcmp_(fields[next_idx][0], '/') &&
                   strcmp_(fields[idx], fields[next_idx])) {
                bitmask[next_idx] = 0;
                ++next_idx;
            }
            ustore_size_t ptr_idx = next_idx;
            while (ptr_idx < c.fields_count && !chrcmp_(fields[ptr_idx][0], '/'))
                ++ptr_idx;

            while (ptr_idx < c.fields_count) {
                if (strncmp_(fields[idx], &fields[ptr_idx][1], std::strlen(fields[idx])) &&
                    chrcmp_(fields[ptr_idx][std::strlen(fields[idx]) + 1], '/'))
                    bitmask[ptr_idx] = 0;
                ++ptr_idx;
            }
        }
        idx = next_idx;
    }

    size_t count = 0;
    for (auto bit : bitmask) {
        if (bit)
            ++count;
    }
    auto prepared_fields = arena.alloc<ustore_str_view_t>(count, c.error);
    return_if_error_m(c.error);

    for (ustore_size_t idx = 0, pos = 0; idx < c.fields_count; ++idx) {
        if (!bitmask[idx])
            continue;

        ustore_size_t len = std::strlen(fields[idx]);
        auto field = arena.alloc<ustore_char_t>(len + 1, c.error);
        return_if_error_m(c.error);
        std::memcpy(field.begin(), fields[idx], len + 1);
        prepared_fields[pos] = field.begin();
        ++pos;
    }
    c.fields_count = count;
    fields_output = {prepared_fields.begin(), sizeof(ustore_str_view_t)};
}

simdjson::ondemand::object& rewinded(simdjson::ondemand::object& doc) noexcept {
    doc.reset();
    return doc;
}

void get_value(simdjson::ondemand::object& data,
               ustore_str_view_t json_field,
               ustore_str_view_t field,
               std::string& json) {

    bool state = is_json_ptr(field);

    auto get_result = [&]() {
        return state ? rewinded(data).at_pointer(field) : rewinded(data)[field];
    };

    switch (get_result().type()) {
    case simdjson::ondemand::json_type::array:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            get_result().get_array().value().raw_json().value());
        break;
    case simdjson::ondemand::json_type::object:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            get_result().get_object().value().raw_json().value());
        break;
    case simdjson::ondemand::json_type::number:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            std::string_view(get_result().raw_json_token().value()));
        break;
    case simdjson::ondemand::json_type::string:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            std::string_view(get_result().raw_json_token().value()));
        break;
    case simdjson::ondemand::json_type::boolean:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            bool(get_result().value()) ? "true" : "false");
        break;
    case simdjson::ondemand::json_type::null:
        fmt::format_to( //
            std::back_inserter(json),
            "{}{},",
            json_field,
            "null");
        break;
    default: break;
    }
}

void simdjson_object_parser( //
    simdjson::ondemand::object& object,
    counts_t counts,
    fields_t const& fields,
    ustore_size_t fields_count,
    tape_t const& tape,
    std::string& json) {

    strings_tape_iterator_t iter(fields_count * fields_count, tape.begin());
    auto counts_iter = counts.begin();

    auto try_close = [&]() {
        if (!strcmp_(*iter, "}"))
            return;

        do {
            json.insert(json.size() - 1, *iter);
            ++iter;
        } while (strcmp_(*iter, "}"));
    };

    for (ustore_size_t idx = 0; idx < fields_count;) {
        try_close();
        if (is_json_ptr(fields[idx])) {
            if (strchr(fields[idx] + 1, '/') != nullptr) {
                while ((*iter)[std::strlen(*iter) - 1] == '{') {
                    fmt::format_to(std::back_inserter(json), "{}", *iter);
                    ++iter;
                }
                for (ustore_size_t pos = idx; idx < pos + (*counts_iter); ++idx, ++iter)
                    get_value(object, *iter, fields[idx], json);
                ++counts_iter;
                continue;
            }
        }
        get_value(object, *iter, fields[idx], json);
        ++iter;
        ++idx;
    }
    try_close();
    json.back() = '}';
}

void fields_parser( //
    ustore_error_t* error,
    linked_memory_lock_t& arena,
    ustore_size_t fields_count,
    fields_t const& fields,
    counts_t& counts,
    tape_t& tape) {

    ustore_size_t counts_idx = ULLONG_MAX;
    ustore_size_t back_idx = 0;
    ustore_size_t pre_idx = 0;
    ustore_size_t offset = 0;
    ustore_size_t size = 0;
    ustore_size_t pos = 0;

    for (ustore_size_t idx = 0; idx < fields_count; ++idx) {
        pos = 1;
        if (is_json_ptr(fields[idx])) {
            pos = strchr(fields[idx] + pos, '/') - fields[idx];
            while (pos <= std::strlen(fields[idx])) {
                ++size;
                pos = strchr(fields[idx] + pos + 1, '/') - fields[idx];
            }
        }
    }

    auto prefixes = arena.alloc<ustore_char_t*>(size, error);
    return_if_error_m(error);

    auto close_bracket = [&]() {
        --back_idx;
        tape[offset] = '}';
        tape[offset + 1] = '\0';
        offset += 2;
    };

    auto fill_prefixes = [&](ustore_str_view_t field) {
        while (pos <= std::strlen(field)) {
            prefixes[back_idx] = arena.alloc<ustore_char_t>(pos + 2, error).begin();
            return_if_error_m(error);
            ++back_idx;
            std::memcpy(prefixes[back_idx - 1], field, pos + 1);
            prefixes[back_idx - 1][pos + 1] = '\0';
            auto substr = arena.alloc<ustore_char_t>(pos - pre_idx + 2, error);
            return_if_error_m(error);
            std::memcpy(substr.begin(), field + pre_idx, pos - pre_idx);
            substr[substr.size() - 2] = '\0';
            auto str = fmt::format("\"{}\":{{{}", substr.begin(), '\0');
            std::strncpy(tape.begin() + offset, str.data(), str.size());
            offset += str.size();
            pre_idx = pos + 1;
            pos = strchr(field + pre_idx, '/') - field;
        }
    };

    for (ustore_size_t idx = 0; idx < fields_count;) {
        if (is_json_ptr(fields[idx])) {
            pre_idx = 1;

            pos = strchr(fields[idx] + pre_idx, '/') - fields[idx];
            if (pos <= std::strlen(fields[idx])) {
                fill_prefixes(fields[idx]);
                while (back_idx) {
                    ++counts_idx;
                    counts[counts_idx] = 0;
                    while (strncmp_(prefixes[back_idx - 1], fields[idx], std::strlen(prefixes[back_idx - 1]))) {
                        pre_idx = std::strlen(prefixes[back_idx - 1]) + 1;
                        ustore_size_t length = std::strlen(fields[idx]) - pre_idx + 2;
                        auto substr = arena.alloc<ustore_char_t>(length, error);
                        return_if_error_m(error);
                        std::memcpy(substr.begin(), fields[idx] + pre_idx - 1, length);
                        substr[substr.size() - 1] = '\0';
                        auto str = fmt::format("\"{}\":{}", substr.begin(), '\0');
                        std::strncpy(tape.begin() + offset, str.data(), str.size());
                        offset += str.size();
                        ++counts[counts_idx];
                        ++idx;
                        if (idx == fields_count) {
                            while (back_idx)
                                close_bracket();
                            return;
                        }
                        ustore_size_t sz = back_idx;
                        while (back_idx && !strncmp_( //
                                               prefixes[back_idx - 1],
                                               fields[idx],
                                               std::strlen(prefixes[back_idx - 1]))) {
                            close_bracket();
                        }

                        if (back_idx == 0)
                            break;
                        else if (sz > back_idx) {
                            pre_idx = std::strlen(prefixes[back_idx - 1]) + 1;
                            ++counts_idx;
                            counts[counts_idx] = 0;
                        }

                        pos = strchr(fields[idx] + pre_idx, '/') - fields[idx];
                        if (pos <= std::strlen(fields[idx])) {
                            --pre_idx;
                            fill_prefixes(fields[idx]);
                            ++counts_idx;
                            counts[counts_idx] = 0;
                        }
                    }
                }
            }
            else {
                auto str = fmt::format("\"{}\":{}", fields[idx] + 1, '\0');
                std::strncpy(tape.begin() + offset, str.data(), str.size());
                offset += str.size();
                ++idx;
            }
        }
        else {
            auto str = fmt::format("\"{}\":{}", fields[idx], '\0');
            std::strncpy(tape.begin() + offset, str.data(), str.size());
            offset += str.size();
            ++idx;
        }
    }
}

#pragma endregion - Helpers

#pragma region - Upserting

void upsert_docs(ustore_docs_import_t& c, docs_t& docs, ustore_size_t task_count) {

    ustore_docs_write_t docs_write {
        .db = c.db,
        .error = c.error,
        .arena = c.arena,
        .options = ustore_option_dont_discard_memory_k,
        .tasks_count = task_count,
        .type = ustore_doc_field_json_k,
        .modification = ustore_doc_modify_upsert_k,
        .collections = &c.collection,
        .lengths = docs[0].member_length(),
        .lengths_stride = sizeof(value_view_t),
        .values = docs[0].member_ptr(),
        .values_stride = sizeof(value_view_t),
        .id_field = c.id_field,
    };

    ustore_docs_write(&docs_write);
}

void upsert_graph(ustore_graph_import_t& c, edges_t const& edges_src, ustore_size_t task_count) {

    auto strided = edges(edges_src);
    ustore_graph_upsert_edges_t graph_upsert_edges {
        .db = c.db,
        .error = c.error,
        .arena = c.arena,
        .options = ustore_option_dont_discard_memory_k,
        .tasks_count = task_count,
        .collections = &c.collection,
        .edges_ids = strided.edge_ids.begin().get(),
        .edges_stride = strided.edge_ids.stride(),
        .sources_ids = strided.source_ids.begin().get(),
        .sources_stride = strided.source_ids.stride(),
        .targets_ids = strided.target_ids.begin().get(),
        .targets_stride = strided.target_ids.stride(),
    };

    ustore_graph_upsert_edges(&graph_upsert_edges);
}

#pragma endregion - Upserting

#pragma region - Files Importing
template <typename import_at>
void import_parquet(import_at& c, std::shared_ptr<arrow::Table>& table) {

    arrow::Status status;
    arrow::MemoryPool* pool = arrow::default_memory_pool();

    // Open File
    auto maybe_input = arrow::io::ReadableFile::Open(c.paths_pattern);
    return_error_if_m(maybe_input.ok(), c.error, 0, "Can't open file");
    auto input = *maybe_input;

    std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
    status = parquet::arrow::OpenFile(input, pool, &arrow_reader);
    return_error_if_m(status.ok(), c.error, 0, "Can't instantiate reader");

    // Read File into table
    status = arrow_reader->ReadTable(&table);
    return_error_if_m(status.ok(), c.error, 0, "Can't read file");
}

template <typename import_at>
void import_csv(import_at& c, std::shared_ptr<arrow::Table>& table) {

    arrow::io::IOContext io_context = arrow::io::default_io_context();
    auto maybe_input = arrow::io::ReadableFile::Open(c.paths_pattern);
    return_error_if_m(maybe_input.ok(), c.error, 0, "Can't open file");
    std::shared_ptr<arrow::io::InputStream> input = *maybe_input;

    auto read_options = arrow::csv::ReadOptions::Defaults();
    auto parse_options = arrow::csv::ParseOptions::Defaults();
    auto convert_options = arrow::csv::ConvertOptions::Defaults();

    // Instantiate TableReader from input stream and options
    auto maybe_reader = arrow::csv::TableReader::Make(io_context, input, read_options, parse_options, convert_options);
    return_error_if_m(maybe_reader.ok(), c.error, 0, "Can't instantiate reader");
    std::shared_ptr<arrow::csv::TableReader> reader = *maybe_reader;

    // Read table from CSV file
    auto maybe_table = reader->Read();
    return_error_if_m(maybe_table.ok(), c.error, 0, "Can't read file");
    table = *maybe_table;
}
#pragma endregion - Files Importing

#pragma region - Docs

void parse_arrow_table_docs(ustore_docs_import_t& c,
                            std::shared_ptr<arrow::Table> const& table,
                            linked_memory_lock_t& arena) {

    fields_t fields;
    ustore_size_t used_mem = 0;
    ustore_char_t* field = nullptr;
    std::string json = "{";
    ustore_char_t* json_cstr = nullptr;
    arrow_visitor_t visitor(json);

    if (!c.fields) {
        auto clmn_names = table->ColumnNames();
        c.fields_count = clmn_names.size();
        auto names = arena.alloc<ustore_str_view_t>(c.fields_count, c.error);
        return_if_error_m(c.error);

        for (ustore_size_t idx = 0; idx < c.fields_count; ++idx) {
            field = arena.alloc<ustore_char_t>(clmn_names[idx].size() + 1, c.error).begin();
            return_if_error_m(c.error);
            std::memcpy(field, clmn_names[idx].data(), clmn_names[idx].size() + 1);
            names[idx] = field;
        }

        c.fields_stride = sizeof(ustore_str_view_t);
        fields = fields_t {names.begin(), c.fields_stride};
    }
    else {
        fields = fields_t {c.fields, c.fields_stride};
        c.fields_count = c.fields_count;
    }

    auto columns = arena.alloc<chunked_array_t>(c.fields_count, c.error);
    return_if_error_m(c.error);
    auto chunks = arena.alloc<array_t>(c.fields_count, c.error);
    return_if_error_m(c.error);

    auto columns_begin = columns.begin();
    auto chunks_begin = chunks.begin();

    for (ustore_size_t idx = 0; idx < c.fields_count; ++idx, ++columns_begin, ++chunks_begin) {
        new (columns_begin) chunked_array_t();
        new (chunks_begin) array_t();
        chunked_array_t column = table->GetColumnByName(fields[idx]);
        return_error_if_m(column, c.error, 0, fmt::format("{} is not exist", fields[idx]).c_str());
        columns[idx] = column;
    }

    ustore_size_t count = columns[0]->num_chunks();
    auto values = arena.alloc<value_view_t>(table->num_rows(), c.error);
    return_if_error_m(c.error);

    ustore_size_t idx = 0;
    for (ustore_size_t chunk_idx = 0, g_idx = 0; chunk_idx != count; ++chunk_idx, g_idx = 0) {

        for (auto column : columns) {
            chunks[g_idx] = column->chunk(chunk_idx);
            ++g_idx;
        }

        for (ustore_size_t value_idx = 0; value_idx < columns[0]->chunk(chunk_idx)->length(); ++value_idx) {

            g_idx = 0;
            for (auto it = fields; g_idx < c.fields_count; ++g_idx, ++it) {
                fmt::format_to(std::back_inserter(json), "\"{}\":", *it);
                visitor.idx = value_idx;
                auto status = arrow::VisitArrayInline(*chunks[g_idx].get(), &visitor);
                if (!status.ok()) {
                    auto ptr = arena.alloc<char>(status.message().size() + 1, c.error);
                    return_if_error_m(c.error);
                    std::memcpy(ptr.data(), status.message().c_str(), status.message().size() + 1);
                    *c.error = ptr.data();
                }
            }

            json[json.size() - 1] = '}';
            json.push_back('\n');
            json_cstr = arena.alloc<ustore_char_t>(json.size() + 1, c.error).begin();
            return_if_error_m(c.error);
            std::memcpy(json_cstr, json.data(), json.size() + 1);

            values[idx] = json_cstr;
            used_mem += json.size();
            json = "{";
            ++idx;

            if (used_mem >= c.max_batch_size) {
                upsert_docs(c, values, idx);
                return_if_error_m(c.error);
                used_mem = 0;
                idx = 0;
            }
        }
    }
    if (idx != 0)
        upsert_docs(c, values, idx);
}

void import_whole_ndjson(ustore_docs_import_t& c,
                         simdjson::ondemand::document_stream& docs,
                         ustore_size_t rows_count,
                         linked_memory_lock_t& arena) {

    ustore_size_t idx = 0;
    ustore_size_t used_mem = 0;

    auto values = arena.alloc<value_view_t>(rows_count, c.error);
    return_if_error_m(c.error);

    for (auto doc : docs) {
        simdjson::ondemand::object object = doc.get_object().value();
        values[idx] = rewinded(object).raw_json().value();
        used_mem += values[idx].size();
        ++idx;
        if (used_mem >= c.max_batch_size) {
            upsert_docs(c, values, idx);
            return_if_error_m(c.error);
            idx = 0;
        }
    }
    if (idx != 0)
        upsert_docs(c, values, idx);
}

void import_sub_ndjson(ustore_docs_import_t& c,
                       simdjson::ondemand::document_stream& docs,
                       ustore_size_t rows_count,
                       linked_memory_lock_t& arena) {

    fields_t fields;
    prepare_fields(c, arena, fields);
    ustore_size_t max_size = c.fields_count * symbols_count_k;
    for (ustore_size_t idx = 0; idx < c.fields_count; ++idx)
        max_size += std::strlen(fields[idx]);

    auto values = arena.alloc<value_view_t>(rows_count, c.error);
    return_if_error_m(c.error);

    ustore_size_t used_mem = 0;
    std::string json = "{";
    ustore_char_t* json_cstr = nullptr;

    counts_t counts = arena.alloc<ustore_size_t>(c.fields_count, c.error);
    return_if_error_m(c.error);

    auto tape = arena.alloc<ustore_char_t>(max_size, c.error);
    return_if_error_m(c.error);
    fields_parser(c.error, arena, c.fields_count, fields, counts, tape);

    ustore_size_t idx = 0;
    for (auto doc : docs) {
        simdjson::ondemand::object object = doc.get_object().value();
        simdjson_object_parser(object, counts, fields, c.fields_count, tape, json);

        json.push_back('\n');
        json_cstr = arena.alloc<ustore_char_t>(json.size() + 1, c.error).begin();
        return_if_error_m(c.error);
        std::memcpy(json_cstr, json.data(), json.size() + 1);

        values[idx] = json_cstr;
        used_mem += json.size();
        json = "{";
        ++idx;

        if (used_mem >= c.max_batch_size) {
            upsert_docs(c, values, idx);
            return_if_error_m(c.error);
            idx = 0;
        }
    }
    if (idx != 0)
        upsert_docs(c, values, idx);
}

void import_ndjson_docs(ustore_docs_import_t& c, linked_memory_lock_t& arena) {

    auto handle = open(c.paths_pattern, O_RDONLY);
    return_error_if_m(handle != -1, c.error, 0, "Can't open file");

    ustore_size_t file_size = std::filesystem::file_size(std::filesystem::path(c.paths_pattern));
    auto begin = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, handle, 0);
    return_error_if_m(begin != MAP_FAILED, c.error, 0, "Failed to mmap content");
    std::string_view mapped_content = std::string_view(reinterpret_cast<ustore_char_t const*>(begin), file_size);
    auto res = madvise(begin, file_size, MADV_SEQUENTIAL);
    return_error_if_m(res == 0, c.error, 0, "Failed to madvise content");

    std::ifstream file(c.paths_pattern);
    ustore_size_t rows_count =
        std::count(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>(), '\n') + 1;

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs = parser.iterate_many( //
        mapped_content.data(),
        mapped_content.size(),
        1000000ul);

    if (!c.fields)
        import_whole_ndjson(c, docs, rows_count, arena);
    else
        import_sub_ndjson(c, docs, rows_count, arena);
    return_if_error_m(c.error);

    res = munmap((void*)mapped_content.data(), mapped_content.size());
    return_error_if_m(res == 0, c.error, 0, "Failed to munmap content");
    close(handle);
    return_error_if_m(res == 0, c.error, 0, "Failed to close decriptor");
}

void export_whole_docs( //
    ustore_error_t* error,
    linked_memory_lock_t& arena,
    ptr_range_gt<ustore_key_t const>& keys,
    ptr_range_gt<ustore_key_t>* keys_ptr,
    ptr_range_gt<ustore_char_t*>* docs_ptr,
    parquet::StreamWriter* os_ptr,
    val_t const& values,
    int handle,
    ext_t pcn) {

    ptr_range_gt<ustore_char_t*>& docs_vec = *docs_ptr;
    ptr_range_gt<ustore_key_t>& keys_vec = *keys_ptr;
    parquet::StreamWriter& os = *os_ptr;

    ustore_size_t idx = 0;
    auto iter = keys.begin();

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs = parser.iterate_many( //
        values.first,
        values.second,
        1000000ul);

    for (auto doc : docs) {
        simdjson::ondemand::object obj = doc.get_object().value();
        auto value = rewinded(obj).raw_json().value();
        auto json = std::string(value.data(), value.size());
        json.pop_back();

        if (pcn == parquet_k) {
            os << *iter << json.data();
            os << parquet::EndRow;
        }
        else if (pcn == csv_k) {
            keys_vec[idx] = *iter;
            docs_vec[idx] = arena.alloc<ustore_char_t>(json.size() + 1, error).begin();
            return_if_error_m(error);
            std::memcpy(docs_vec[idx], json.data(), json.size() + 1);
            ++idx;
        }
        else {
            auto str = fmt::format("{{\"_id\":{},\"doc\":{}}}\n", *iter, json.data());
            write(handle, str.data(), str.size());
        }
        ++iter;
    }
}

void export_sub_docs( //
    ustore_docs_export_t& c,
    linked_memory_lock_t& arena,
    parquet::StreamWriter* os_ptr,
    ptr_range_gt<ustore_char_t*>* docs_ptr,
    ptr_range_gt<ustore_key_t>* keys_ptr,
    ptr_range_gt<ustore_key_t const>& keys,
    ptr_range_gt<ustore_char_t> const& tape,
    fields_t const& fields,
    counts_t const& counts,
    val_t const& values,
    int handle,
    ext_t pcn) {

    ptr_range_gt<ustore_char_t*>& docs_vec = *docs_ptr;
    ptr_range_gt<ustore_key_t>& keys_vec = *keys_ptr;
    parquet::StreamWriter& os = *os_ptr;

    ustore_size_t idx = 0;
    auto iter = keys.begin();
    std::string json = prefix_k;

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs = parser.iterate_many( //
        values.first,
        values.second,
        1000000ul);

    for (auto doc : docs) {
        simdjson::ondemand::object obj = doc.get_object().value();
        simdjson_object_parser(obj, counts, fields, c.fields_count, tape, json);

        if (pcn == parquet_k) {
            os << *iter << json.data();
            os << parquet::EndRow;
        }
        else if (pcn == csv_k) {
            keys_vec[idx] = *iter;
            docs_vec[idx] = arena.alloc<ustore_char_t>(json.size() + 1, c.error).begin();
            return_if_error_m(c.error);
            std::memcpy(docs_vec[idx], json.data(), json.size() + 1);
            ++idx;
        }
        else {
            auto str = fmt::format("{{\"_id\":{},\"doc\":{}}}\n", *iter, json.data());
            write(handle, str.data(), str.size());
        }
        json = prefix_k;
        ++iter;
    }
}

void make_parquet(ustore_docs_export_t& c, parquet::StreamWriter& os) {

    parquet::schema::NodeVector nodes;
    nodes.push_back(parquet::schema::PrimitiveNode::Make( //
        "_id",
        parquet::Repetition::REQUIRED,
        parquet::Type::INT64,
        parquet::ConvertedType::INT_64));

    nodes.push_back(parquet::schema::PrimitiveNode::Make( //
        "doc",
        parquet::Repetition::REQUIRED,
        parquet::Type::BYTE_ARRAY,
        parquet::ConvertedType::UTF8));

    std::shared_ptr<parquet::schema::GroupNode> schema = std::static_pointer_cast<parquet::schema::GroupNode>(
        parquet::schema::GroupNode::Make("schema", parquet::Repetition::REQUIRED, nodes));

    auto maybe_outfile =
        arrow::io::FileOutputStream::Open(fmt::format("{}{}", generate_file_name(), c.paths_extension));
    return_error_if_m(maybe_outfile.ok(), c.error, 0, "Can't open file");
    auto outfile = *maybe_outfile;

    parquet::WriterProperties::Builder builder;
    builder.memory_pool(arrow::default_memory_pool());
    builder.write_batch_size(c.max_batch_size);

    os = parquet::StreamWriter {parquet::ParquetFileWriter::Open(outfile, schema, builder.build())};
}

int make_ndjson(ustore_docs_export_t& docs) {
    return open(fmt::format("{}{}", generate_file_name(), docs.paths_extension).data(),
                O_CREAT | O_WRONLY,
                S_IRUSR | S_IWUSR);
}

void write_in_parquet( //
    ustore_docs_export_t& c,
    linked_memory_lock_t& arena,
    parquet::StreamWriter& ostream,
    ptr_range_gt<ustore_key_t const>& keys,
    ptr_range_gt<ustore_char_t> const& tape,
    fields_t const& fields,
    counts_t const& counts,
    val_t const& values) {

    if (c.fields)
        export_sub_docs(c, arena, &ostream, nullptr, nullptr, keys, tape, fields, counts, values, 0, parquet_k);
    else
        export_whole_docs(c.error, arena, keys, nullptr, nullptr, &ostream, values, 0, parquet_k);
}

void write_in_csv( //
    ustore_docs_export_t& c,
    linked_memory_lock_t& arena,
    ptr_range_gt<ustore_char_t*>& docs_vec,
    ptr_range_gt<ustore_key_t>& keys_vec,
    ptr_range_gt<ustore_key_t const>& keys,
    int_builder_t& int_builder,
    arrow::StringBuilder& string_builder,
    ptr_range_gt<ustore_char_t> const& tape,
    fields_t const& fields,
    counts_t const& counts,
    val_t const& values,
    ustore_size_t size) {

    arrow::Status status;
    status = int_builder.Resize(int_builder.capacity() + size);
    return_error_if_m(status.ok(), c.error, 0, "Can't resize builder");
    status = string_builder.Resize(string_builder.capacity() + size);
    return_error_if_m(status.ok(), c.error, 0, "Can't resize builder");

    if (c.fields)
        export_sub_docs(c, arena, nullptr, &docs_vec, &keys_vec, keys, tape, fields, counts, values, 0, csv_k);
    else
        export_whole_docs(c.error, arena, keys, &keys_vec, &docs_vec, nullptr, values, 0, csv_k);

    status = int_builder.AppendValues(keys_vec.begin(), size);
    return_error_if_m(status.ok(), c.error, 0, "Can't append keys");
    status = string_builder.AppendValues((ustore_char_t const**)docs_vec.begin(), size);
    return_error_if_m(status.ok(), c.error, 0, "Can't append docs");
}

void end_csv( //
    ustore_docs_export_t& c,
    arrow::StringBuilder& string_builder,
    int_builder_t& int_builder) {

    array_t keys_array;
    array_t docs_array;
    arrow::Status status;

    status = int_builder.Finish(&keys_array);
    return_error_if_m(status.ok(), c.error, 0, "Can't finish array(keys)");
    status = string_builder.Finish(&docs_array);
    return_error_if_m(status.ok(), c.error, 0, "Can't finish array(docs)");

    arrow::FieldVector fields;
    fields.push_back(arrow::field("_id", arrow::int64()));
    fields.push_back(arrow::field("doc", arrow::large_binary()));
    std::shared_ptr<arrow::Schema> schema = std::make_shared<arrow::Schema>(fields);

    std::shared_ptr<arrow::Table> table;
    table = arrow::Table::Make(schema, {keys_array, docs_array});

    auto maybe_outstream =
        arrow::io::FileOutputStream::Open(fmt::format("{}{}", generate_file_name(), c.paths_extension));
    return_error_if_m(maybe_outstream.ok(), c.error, 0, "Can't open file");
    std::shared_ptr<arrow::io::FileOutputStream> outstream = *maybe_outstream;

    status = arrow::csv::WriteCSV(*table, arrow::csv::WriteOptions::Defaults(), outstream.get());
    return_error_if_m(status.ok(), c.error, 0, "Can't write in file");
}

int end_ndjson(int fd) {
    return close(fd);
}

void write_in_ndjson( //
    ustore_docs_export_t& c,
    linked_memory_lock_t& arena,
    ptr_range_gt<ustore_key_t const>& keys,
    ptr_range_gt<ustore_char_t> const& tape,
    fields_t const& fields,
    counts_t const& counts,
    val_t const& values,
    int handle) {

    if (c.fields)
        export_sub_docs(c, arena, nullptr, nullptr, nullptr, keys, tape, fields, counts, values, handle, ndjson_k);
    else
        export_whole_docs(c.error, arena, keys, nullptr, nullptr, nullptr, values, handle, ndjson_k);
}

#pragma region - Main Functions(Docs)

void ustore_docs_import(ustore_docs_import_t* c_ptr) {

    ustore_docs_import_t& c = *c_ptr;

    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    validate_docs_fields(c);
    return_if_error_m(c.error);
    return_error_if_m(c.id_field, c.error, uninitialized_state_k, "id_field must be initialized");
    check_for_id_field(c);
    return_if_error_m(c.error);
    return_error_if_m(c.max_batch_size, c.error, uninitialized_state_k, "Max batch size is 0");
    return_error_if_m(c.paths_pattern, c.error, uninitialized_state_k, "Paths pattern is uninitialized");

    if (!c.arena)
        c.arena = arena_t(c.db).member_ptr();

    auto arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    auto handle_exception = [&](char const* ex) {
        auto ptr = arena.alloc<char>(std::strlen(ex) + 1, c.error);
        return_if_error_m(c.error);
        std::memcpy(ptr.data(), ex, std::strlen(ex) + 1);
        *c.error = ptr.data();
    };

    try {
        auto ext = std::filesystem::path(c.paths_pattern).extension();
        if (ext == ".ndjson")
            import_ndjson_docs(c, arena);
        else {
            std::shared_ptr<arrow::Table> table;
            if (ext == ".parquet")
                import_parquet(c, table);
            else if (ext == ".csv")
                import_csv(c, table);
            parse_arrow_table_docs(c, table, arena);
            return_if_error_m(c.error);
        }
    }
    catch (std::exception const& ex) {
        handle_exception(ex.what());
        return_if_error_m(c.error);
    }
}

void ustore_docs_export(ustore_docs_export_t* c_ptr) {

    ustore_docs_export_t& c = *c_ptr;

    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    validate_docs_fields(c);
    return_if_error_m(c.error);
    return_error_if_m(c.paths_extension, c.error, uninitialized_state_k, "Paths extension is uninitialized");
    return_error_if_m(c.max_batch_size, c.error, uninitialized_state_k, "Max batch size is 0");

    auto ext = c.paths_extension;
    ext_t pcn = strcmp_(ext, ".parquet")  ? parquet_k
                : strcmp_(ext, ".csv")    ? csv_k
                : strcmp_(ext, ".ndjson") ? ndjson_k
                                          : unknown_k;
    return_error_if_m(!(pcn == unknown_k), c.error, 0, "Not supported format");

    if (!c.arena)
        c.arena = arena_t(c.db).member_ptr();
    auto arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    auto handle_exception = [&](char const* ex) {
        auto ptr = arena.alloc<char>(std::strlen(ex) + 1, c.error);
        return_if_error_m(c.error);
        std::memcpy(ptr.data(), ex, std::strlen(ex) + 1);
        *c.error = ptr.data();
    };

    try {
        int handle = 0;
        parquet::StreamWriter os;
        ptr_range_gt<ustore_char_t*> docs_vec;
        ptr_range_gt<ustore_key_t> keys_vec;
        arrow::StringBuilder string_builder;
        int_builder_t int_builder;

        ustore_size_t task_count = 1'000'000;
        keys_stream_t stream(c.db, c.collection, task_count);

        fields_t fields;
        ptr_range_gt<ustore_char_t> tape;
        auto counts = arena.alloc<ustore_size_t>(c.fields_count, c.error);
        return_if_error_m(c.error);

        if (pcn == parquet_k)
            make_parquet(c, os);
        else if (pcn == csv_k) {
            keys_vec = arena.alloc<ustore_key_t>(task_count, c.error);
            return_if_error_m(c.error);
            docs_vec = arena.alloc<ustore_char_t*>(task_count, c.error);
            return_if_error_m(c.error);
        }
        else
            handle = make_ndjson(c);

        if (c.fields) {
            prepare_fields(c, arena, fields);
            ustore_size_t max_size = c.fields_count * symbols_count_k;
            for (ustore_size_t idx = 0; idx < c.fields_count; ++idx)
                max_size += std::strlen(fields[idx]);

            tape = arena.alloc<ustore_char_t>(max_size, c.error);
            return_if_error_m(c.error);
            fields_parser(c.error, arena, c.fields_count, fields, counts, tape);
            return_if_error_m(c.error);
        }

        auto status = stream.seek_to_first();
        return_error_if_m(status, c.error, 0, "No batches in stream");

        while (!stream.is_end()) {
            val_t values {nullptr, 0};
            ustore_length_t* offsets = nullptr;
            ustore_length_t* lengths = nullptr;
            auto keys = stream.keys_batch();

            ustore_docs_read_t docs_read {
                .db = c.db,
                .error = c.error,
                .arena = c.arena,
                .options = ustore_option_dont_discard_memory_k,
                .tasks_count = keys.size(),
                .collections = &c.collection,
                .keys = keys.begin(),
                .keys_stride = sizeof(ustore_key_t),
                .lengths = &lengths,
            };
            ustore_docs_read(&docs_read);
            return_if_error_m(c.error);

            for (ustore_size_t idx = 0; idx < keys.size(); ++idx) {
                ustore_size_t pre_idx = idx;
                ustore_size_t size = 0;

                do {
                    size += lengths[idx];
                    ++idx;
                } while (size < c.max_batch_size && idx < keys.size());

                ustore_docs_read_t docs_read {
                    .db = c.db,
                    .error = c.error,
                    .arena = c.arena,
                    .options = ustore_option_dont_discard_memory_k,
                    .tasks_count = idx - pre_idx,
                    .collections = &c.collection,
                    .keys = keys.begin() + pre_idx,
                    .keys_stride = sizeof(ustore_key_t),
                    .offsets = &offsets,
                    .values = &values.first,
                };
                ustore_docs_read(&docs_read);
                return_if_error_m(c.error);
                values.second = offsets[idx - pre_idx - 1] + lengths[idx - 1];

                if (pcn == parquet_k)
                    write_in_parquet(c, arena, os, keys, tape, fields, counts, values);
                else if (pcn == csv_k)
                    write_in_csv(c,
                                 arena,
                                 docs_vec,
                                 keys_vec,
                                 keys,
                                 int_builder,
                                 string_builder,
                                 tape,
                                 fields,
                                 counts,
                                 values,
                                 idx - pre_idx);
                else
                    write_in_ndjson(c, arena, keys, tape, fields, counts, values, handle);
                return_if_error_m(c.error);
            }
            status = stream.seek_to_next_batch();
            return_error_if_m(status, c.error, 0, "Invalid batch");
        }

        if (pcn == csv_k)
            end_csv(c, string_builder, int_builder);
        else if (pcn == ndjson_k)
            return_error_if_m(end_ndjson(handle) == 0, c.error, 0, "Failed to close decriptor");
    }
    catch (std::exception const& ex) {
        handle_exception(ex.what());
        return_if_error_m(c.error);
    }
}

#pragma endregion - Main Functions(Docs)
#pragma endregion - Docs

#pragma region - Graph

void parse_arrow_table_graph(ustore_graph_import_t& c,
                             ustore_size_t task_count,
                             std::shared_ptr<arrow::Table> const& table,
                             linked_memory_lock_t& arena) {

    auto sources = table->GetColumnByName(c.source_id_field);
    return_error_if_m(sources, c.error, 0, "The source field does not exist");
    auto targets = table->GetColumnByName(c.target_id_field);
    return_error_if_m(targets, c.error, 0, "The target field does not exist");
    auto edges = c.edge_id_field ? table->GetColumnByName(c.edge_id_field) : nullptr;
    return_error_if_m(edges || (c.edge_id_field == nullptr), c.error, 0, "The edge field does not exist");
    ustore_size_t count = sources->num_chunks();
    return_error_if_m(count > 0, c.error, 0, "Empty Input");

    auto vertices_edges = arena.alloc<edge_t>(table->num_rows(), c.error);
    return_if_error_m(c.error);
    ustore_size_t idx = 0;

    for (ustore_size_t chunk_idx = 0; chunk_idx != count; ++chunk_idx) {
        auto source_chunk = sources->chunk(chunk_idx);
        auto target_chunk = targets->chunk(chunk_idx);
        auto edge_chunk = edges ? edges->chunk(chunk_idx) : array_t {};
        auto source_array = std::static_pointer_cast<arrow::Int64Array>(source_chunk);
        auto target_array = std::static_pointer_cast<arrow::Int64Array>(target_chunk);
        auto edge_array = std::static_pointer_cast<arrow::Int64Array>(edge_chunk);
        for (ustore_size_t value_idx = 0; value_idx != source_array->length(); ++value_idx) {
            edge_t edge {
                .source_id = source_array->Value(value_idx),
                .target_id = target_array->Value(value_idx),
                .id = edges ? edge_array->Value(value_idx) : ustore_default_edge_id_k,
            };
            vertices_edges[idx] = edge;
            ++idx;
            if (idx == task_count) {
                upsert_graph(c, vertices_edges, idx);
                return_if_error_m(c.error);
                idx = 0;
            }
        }
    }
    if (idx != 0)
        upsert_graph(c, vertices_edges, idx);
}

void import_ndjson_graph(ustore_graph_import_t& c, ustore_size_t task_count, linked_memory_lock_t& arena) {

    auto edges = arena.alloc<edge_t>(task_count, c.error);
    return_if_error_m(c.error);

    auto handle = open(c.paths_pattern, O_RDONLY);
    return_error_if_m(handle != -1, c.error, 0, "Can't open file");

    ustore_size_t file_size = std::filesystem::file_size(std::filesystem::path(c.paths_pattern));
    auto begin = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, handle, 0);
    return_error_if_m(begin != MAP_FAILED, c.error, 0, "Failed to mmap content");
    std::string_view mapped_content = std::string_view(reinterpret_cast<ustore_char_t const*>(begin), file_size);
    auto res = madvise(begin, file_size, MADV_SEQUENTIAL);
    return_error_if_m(res == 0, c.error, 0, "Failed to madvise content");

    auto get_data = [&](simdjson::ondemand::object& data, ustore_str_view_t field) {
        return chrcmp_(field[0], '/') ? rewinded(data).at_pointer(field) : rewinded(data)[field];
    };

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs = parser.iterate_many( //
        mapped_content.data(),
        mapped_content.size(),
        1000000ul);

    ustore_key_t edge = ustore_default_edge_id_k;
    ustore_size_t idx = 0;
    for (auto doc : docs) {
        simdjson::ondemand::object data = doc.get_object().value();
        if (c.edge_id_field)
            edge = get_data(data, c.edge_id_field);
        edges[idx] = edge_t {get_data(data, c.source_id_field), get_data(data, c.target_id_field), edge};
        ++idx;
        if (idx == task_count) {
            upsert_graph(c, edges, idx);
            return_if_error_m(c.error);
            idx = 0;
        }
    }
    if (idx != 0)
        upsert_graph(c, edges, idx);
    return_if_error_m(c.error);

    res = munmap((void*)mapped_content.data(), mapped_content.size());
    return_error_if_m(res == 0, c.error, 0, "Failed to munmap content");
    close(handle);
    return_error_if_m(res == 0, c.error, 0, "Failed to close decriptor");
}

void make_parquet_graph(ustore_graph_export_t& c, parquet::StreamWriter& os) {
    parquet::schema::NodeVector fields;
    fields.push_back(parquet::schema::PrimitiveNode::Make( //
        c.source_id_field,
        parquet::Repetition::REQUIRED,
        parquet::Type::INT64,
        parquet::ConvertedType::INT_64));

    fields.push_back(parquet::schema::PrimitiveNode::Make( //
        c.target_id_field,
        parquet::Repetition::REQUIRED,
        parquet::Type::INT64,
        parquet::ConvertedType::INT_64));

    if (c.edge_id_field)
        fields.push_back(parquet::schema::PrimitiveNode::Make( //
            c.edge_id_field,
            parquet::Repetition::REQUIRED,
            parquet::Type::INT64,
            parquet::ConvertedType::INT_64));

    std::shared_ptr<parquet::schema::GroupNode> schema = std::static_pointer_cast<parquet::schema::GroupNode>(
        parquet::schema::GroupNode::Make("schema", parquet::Repetition::REQUIRED, fields));

    auto maybe_outfile =
        arrow::io::FileOutputStream::Open(fmt::format("{}{}", generate_file_name(), c.paths_extension));
    return_error_if_m(maybe_outfile.ok(), c.error, 0, "Can't open file");
    auto outfile = *maybe_outfile;

    parquet::WriterProperties::Builder builder;
    builder.memory_pool(arrow::default_memory_pool());
    builder.write_batch_size(c.max_batch_size / vertices_edge_k);

    os = parquet::StreamWriter {parquet::ParquetFileWriter::Open(outfile, schema, builder.build())};
}

template <typename docs_graph_at>
int make_ndjson(docs_graph_at& docs_graph) {
    return open(fmt::format("{}{}", generate_file_name(), docs_graph.paths_extension).data(),
                O_CREAT | O_WRONLY,
                S_IRUSR | S_IWUSR);
}

void write_in_file_graph( //
    ustore_graph_export_t& c,
    keys_length_t const& ids,
    int_builder_t& sources_builder,
    int_builder_t& targets_builder,
    int_builder_t& edges_builder,
    ptr_range_gt<ustore_key_t>& sources,
    ptr_range_gt<ustore_key_t>& targets,
    ptr_range_gt<ustore_key_t>& edges,
    parquet::StreamWriter& os,
    int handle,
    ext_t pcn) {

    ustore_size_t csv_idx = 0;
    ustore_key_t* data = nullptr;

    arrow::Status status;
    status = sources_builder.Resize(sources_builder.capacity() + (ids.second / vertices_edge_k));
    return_error_if_m(status.ok(), c.error, 0, "Can't resize builder");
    status = targets_builder.Resize(targets_builder.capacity() + (ids.second / vertices_edge_k));
    return_error_if_m(status.ok(), c.error, 0, "Can't resize builder");
    status = edges_builder.Resize(edges_builder.capacity() + (ids.second / vertices_edge_k));
    return_error_if_m(status.ok(), c.error, 0, "Can't resize builder");

    if (!c.edge_id_field) {
        data = ids.first;
        for (ustore_size_t idx = 0; idx < ids.second; idx += 3, ++csv_idx) {
            if (pcn == parquet_k)
                os << data[idx] << data[idx + 1] << parquet::EndRow;
            else if (pcn == csv_k) {
                sources[csv_idx] = data[idx];
                targets[csv_idx] = data[idx + 1];
            }
            else {
                auto str = fmt::format( //
                    "{{\"{}\":{},\"{}\":{}}}\n",
                    c.source_id_field,
                    data[idx],
                    c.target_id_field,
                    data[idx + 1]);
                write(handle, str.data(), str.size());
            }
        }
    }
    else {
        data = ids.first;
        for (ustore_size_t idx = 0; idx < ids.second; idx += 3, ++csv_idx) {
            if (pcn == parquet_k)
                os << data[idx] << data[idx + 1] << data[idx + 2] << parquet::EndRow;
            else if (pcn == csv_k) {
                sources[csv_idx] = data[idx];
                targets[csv_idx] = data[idx + 1];
                edges[csv_idx] = data[idx + 2];
            }
            else {
                auto str = fmt::format( //
                    "{{\"{}\":{},\"{}\":{},\"{}\":{}}}\n",
                    c.source_id_field,
                    data[idx],
                    c.target_id_field,
                    data[idx + 1],
                    c.edge_id_field,
                    data[idx + 2]);
                write(handle, str.data(), str.size());
            }
        }
    }
    status = sources_builder.AppendValues(sources.begin(), csv_idx);
    return_error_if_m(status.ok(), c.error, 0, "Can't append sources");
    status = targets_builder.AppendValues(targets.begin(), csv_idx);
    return_error_if_m(status.ok(), c.error, 0, "Can't append targets");
    if (c.edge_id_field) {
        status = edges_builder.AppendValues(edges.begin(), csv_idx);
        return_error_if_m(status.ok(), c.error, 0, "Can't append edges");
    }
}

void end_csv_graph( //
    ustore_graph_export_t& c,
    int_builder_t& sources_builder,
    int_builder_t& targets_builder,
    int_builder_t& edges_builder) {

    array_t sources_array;
    array_t targets_array;
    array_t edges_array;

    auto status = sources_builder.Finish(&sources_array);
    return_error_if_m(status.ok(), c.error, 0, "Can't finish array(sources)");
    status = targets_builder.Finish(&targets_array);
    return_error_if_m(status.ok(), c.error, 0, "Can't finish array(targets)");
    if (c.edge_id_field) {
        status = edges_builder.Finish(&edges_array);
        return_error_if_m(status.ok(), c.error, 0, "Can't finish array(edges)");
    }

    arrow::FieldVector fields;
    fields.push_back(arrow::field(c.source_id_field, arrow::int64()));
    fields.push_back(arrow::field(c.target_id_field, arrow::int64()));
    if (c.edge_id_field)
        fields.push_back(arrow::field(c.edge_id_field, arrow::int64()));

    std::shared_ptr<arrow::Schema> schema = std::make_shared<arrow::Schema>(fields);
    std::shared_ptr<arrow::Table> table = nullptr;

    if (c.edge_id_field)
        table = arrow::Table::Make(schema, {sources_array, targets_array, edges_array});
    else
        table = arrow::Table::Make(schema, {sources_array, targets_array});
    return_error_if_m(table, c.error, 0, "Can't make schema");

    auto maybe_outstream =
        arrow::io::FileOutputStream::Open(fmt::format("{}{}", generate_file_name(), c.paths_extension));
    return_error_if_m(maybe_outstream.ok(), c.error, 0, "Can't open file");
    std::shared_ptr<arrow::io::FileOutputStream> outstream = *maybe_outstream;

    status = arrow::csv::WriteCSV(*table, arrow::csv::WriteOptions::Defaults(), outstream.get());
    return_error_if_m(status.ok(), c.error, 0, "Can't write in file");
}

#pragma region - Main Functions(Graph)

void ustore_graph_import(ustore_graph_import_t* c_ptr) {

    ustore_graph_import_t& c = *c_ptr;

    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    return_error_if_m(c.paths_pattern, c.error, uninitialized_state_k, "Paths pattern is uninitialized");
    return_error_if_m(c.max_batch_size, c.error, uninitialized_state_k, "Max batch size is 0");
    if (!validate_graph_fields(c))
        return_if_error_m(c.error);

    if (!c.arena)
        c.arena = arena_t(c.db).member_ptr();

    auto arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    auto handle_exception = [&](char const* ex) {
        auto ptr = arena.alloc<char>(std::strlen(ex) + 1, c.error);
        return_if_error_m(c.error);
        std::memcpy(ptr.data(), ex, std::strlen(ex) + 1);
        *c.error = ptr.data();
    };

    try {
        auto ext = std::filesystem::path(c.paths_pattern).extension();
        ustore_size_t task_count = c.max_batch_size / sizeof(edge_t);
        if (ext == ".ndjson")
            import_ndjson_graph(c, task_count, arena);
        else {
            std::shared_ptr<arrow::Table> table;
            if (ext == ".parquet")
                import_parquet(c, table);
            else if (ext == ".csv")
                import_csv(c, table);
            parse_arrow_table_graph(c, task_count, table, arena);
            return_if_error_m(c.error);
        }
    }
    catch (std::exception const& ex) {
        handle_exception(ex.what());
        return_if_error_m(c.error);
    }
}

void ustore_graph_export(ustore_graph_export_t* c_ptr) {

    ustore_graph_export_t& c = *c_ptr;

    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    if (!validate_graph_fields(c, true))
        return_if_error_m(c.error);
    return_error_if_m(c.paths_extension, c.error, uninitialized_state_k, "Paths extension is uninitialized");
    return_error_if_m(c.max_batch_size, c.error, uninitialized_state_k, "Max batch size is 0");

    auto ext = c.paths_extension;
    ext_t pcn = strcmp_(ext, ".parquet")  ? parquet_k
                : strcmp_(ext, ".csv")    ? csv_k
                : strcmp_(ext, ".ndjson") ? ndjson_k
                                          : unknown_k;
    return_error_if_m(!(pcn == unknown_k), c.error, 0, "Not supported format");

    if (!c.arena)
        c.arena = arena_t(c.db).member_ptr();
    auto arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    auto handle_exception = [&](char const* ex) {
        auto ptr = arena.alloc<char>(std::strlen(ex) + 1, c.error);
        return_if_error_m(c.error);
        std::memcpy(ptr.data(), ex, std::strlen(ex) + 1);
        *c.error = ptr.data();
    };

    try {
        parquet::StreamWriter os;
        int handle = 0;

        if (pcn == parquet_k)
            make_parquet_graph(c, os);
        else if (pcn == ndjson_k)
            handle = make_ndjson(c);

        ustore_vertex_degree_t* degrees = nullptr;
        ustore_vertex_role_t const role = ustore_vertex_source_k;

        ustore_size_t count = 0;
        ustore_size_t batch_ids = 0;
        ustore_size_t total_ids = 0;
        ustore_size_t task_count = c.max_batch_size / sizeof(edge_t);

        int_builder_t sources_builder;
        int_builder_t targets_builder;
        int_builder_t edges_builder;
        auto sources = arena.alloc<ustore_key_t>(task_count, c.error);
        return_if_error_m(c.error);
        auto targets = arena.alloc<ustore_key_t>(task_count, c.error);
        return_if_error_m(c.error);
        ptr_range_gt<ustore_key_t> edges;
        if (c.edge_id_field)
            edges = arena.alloc<ustore_key_t>(task_count, c.error);
        return_if_error_m(c.error);

        keys_stream_t stream(c.db, c.collection, task_count, nullptr);
        auto status = stream.seek_to_first();
        return_error_if_m(status, c.error, 0, "No batches in stream");

        while (!stream.is_end()) {
            keys_length_t ids_in_edges {nullptr, 0};
            count = stream.keys_batch().size();

            ustore_graph_find_edges_t graph_find {
                .db = c.db,
                .error = c.error,
                .arena = c.arena,
                .options = ustore_option_dont_discard_memory_k,
                .tasks_count = count,
                .collections = &c.collection,
                .vertices = stream.keys_batch().begin(),
                .vertices_stride = sizeof(ustore_key_t),
                .roles = &role,
                .degrees_per_vertex = &degrees,
                .edges_per_vertex = &ids_in_edges.first,
            };
            ustore_graph_find_edges(&graph_find);
            return_if_error_m(c.error);

            batch_ids =
                std::transform_reduce(degrees, degrees + count, 0ul, std::plus {}, [](ustore_vertex_degree_t d) {
                    return d != ustore_vertex_degree_missing_k ? d : 0;
                });
            batch_ids *= vertices_edge_k;
            total_ids += batch_ids;
            ids_in_edges.second = batch_ids;

            write_in_file_graph( //
                c,
                ids_in_edges,
                sources_builder,
                targets_builder,
                edges_builder,
                sources,
                targets,
                edges,
                os,
                handle,
                pcn);
            status = stream.seek_to_next_batch();
            return_error_if_m(status, c.error, 0, "Invalid batch");
        }
        if (pcn == csv_k)
            end_csv_graph(c, sources_builder, targets_builder, edges_builder);
        else if (pcn == ndjson_k)
            return_error_if_m(end_ndjson(handle) == 0, c.error, 0, "Failed to close decriptor");
    }
    catch (std::exception const& ex) {
        handle_exception(ex.what());
        return_if_error_m(c.error);
    }
}

#pragma endregion - Main Functions(Graph)
#pragma endregion - Graph
