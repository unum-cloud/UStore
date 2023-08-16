#include <fcntl.h>    // `open` files
#include <sys/stat.h> // `stat` to obtain file metadata
#include <sys/mman.h> // `mmap` to read datasets faster
#include <unistd.h>

#include <string>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <unordered_map>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#include <arrow/api.h>
#include <arrow/array.h>
#include <arrow/table.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/io/api.h>
#include <arrow/csv/api.h>
#include <arrow/io/file.h>
#include <arrow/csv/writer.h>
#include <arrow/memory_pool.h>
#include <parquet/arrow/reader.h>
#include <parquet/stream_writer.h>
#include <arrow/compute/api_aggregate.h>
#pragma GCC diagnostic pop

#include <simdjson.h>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <ustore/ustore.hpp>
#include "dataset.h"

using namespace unum::ustore;
using graph_t = std::vector<edge_t>;
using docs_t = std::unordered_map<ustore_key_t, std::string>;

namespace fs = std::filesystem;

constexpr size_t max_batch_size_k = 1024 * 1024 * 1024;

constexpr ustore_str_view_t dataset_path_k = "~/Datasets/tweets32K.ndjson";
constexpr ustore_str_view_t parquet_path_k = "~/Datasets/tweets32K-clean.parquet";
constexpr ustore_str_view_t ndjson_path_k = "~/Datasets/tweets32K-clean.ndjson";
constexpr ustore_str_view_t csv_path_k = "~/Datasets/tweets32K-clean.csv";
constexpr ustore_str_view_t sample_path_k = "sample_docs.ndjson";
constexpr ustore_str_view_t path_k = "./";
constexpr size_t rows_count_k = 1000;

static constexpr ustore_str_view_t ext_parquet_k = ".parquet";
static constexpr ustore_str_view_t ext_ndjson_k = ".ndjson";
static constexpr ustore_str_view_t ext_csv_k = ".csv";

constexpr size_t prefixes_count_k = 4;
constexpr ustore_str_view_t prefixes_ak[prefixes_count_k] = {
    "id",
    "id_str",
    "user",
    "quoted_status",
};

constexpr size_t fields_paths_count_k = 13;
static constexpr ustore_str_view_t fields_paths_ak[fields_paths_count_k] = {
    "id",
    "id_str",
    "/user/id",
    "/user/followers_count",
    "/quoted_status/id",
    "/quoted_status/user",
    "/quoted_status/entities/hashtags",
    "/quoted_status/entities/media/0/id",
    "/quoted_status/entities/media/0/sizes/small",
    "/quoted_status/entities/media/0/sizes/large",
    "/quoted_status/extended_entities/media/0/video_info/variants/0",
    "/quoted_status/extended_entities/media/0/video_info/variants/1",
    "/quoted_status/extended_entities/media/0/sizes",
};

constexpr size_t fields_columns_count_k = 7;
static constexpr ustore_str_view_t fields_columns_ak[fields_columns_count_k] = {
    "id",
    "id_str",
    "user_id",
    "user_followers_count",
    "geo_type",
    "favorited",
    "retweeted",
};

static constexpr ustore_str_view_t source_field_k = "id";
static constexpr ustore_str_view_t target_field_k = "user_id";
static constexpr ustore_str_view_t edge_field_k = "user_followers_count";
static constexpr ustore_str_view_t doc_k = "doc";
static constexpr ustore_str_view_t id_k = "_id";

static char const* path() {
    char* path = std::getenv("USTORE_TEST_PATH");
    if (path)
        return std::strlen(path) ? path : nullptr;

#if defined(USTORE_CLI)
    return nullptr;
#elif defined(USTORE_TEST_PATH)
    return USTORE_TEST_PATH;
#else
    return nullptr;
#endif // USTORE_CLI
}

static std::string config() {
    auto dir = path();
    if (!dir)
        return {};
    return fmt::format(R"({{"version": "1.0", "directory": "{}"}})", dir);
}

#if defined(USTORE_CLI)
static pid_t srv_id = -1;
static std::string srv_path;
static std::string cli_path;
#endif // USTORE_CLI

void clear_environment() {
#if defined(USTORE_CLI)
    if (srv_id > 0) {
        kill(srv_id, SIGKILL);
        waitpid(srv_id, nullptr, 0);
    }

    srv_id = fork();
    if (srv_id == 0) {
        usleep(1); // TODO Any statement is requiered to be run for successful `execl` run...
        execl(srv_path.c_str(), srv_path.c_str(), "--quiet", (char*)(NULL));
        exit(0);
    }
    usleep(100000); // 0.1 sec
#endif // USTORE_CLI

    namespace stdfs = std::filesystem;
    auto directory_str = path() ? std::string_view(path()) : "";
    if (!directory_str.empty()) {
        stdfs::remove_all(directory_str);
        stdfs::create_directories(stdfs::path(directory_str));
    }
}

std::filesystem::path home_path(std::getenv("HOME"));
std::vector<std::string> paths;
graph_t expected_edges;
docs_t docs_w_keys;

simdjson::ondemand::object& rewind(simdjson::ondemand::object& doc) noexcept {
    doc.reset();
    return doc;
}

class arrow_visitor_at {
  public:
    arrow_visitor_at(std::string& json) : json(json) {}

    arrow::Status Visit(arrow::NullArray const& arr) {
        fmt::format_to(std::back_inserter(json), "\"\",");
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::BooleanArray const& arr) {
        fmt::format_to(std::back_inserter(json), "true,");
        return arrow::Status::OK();
    }
    arrow::Status Visit(arrow::Int8Array const& arr) {
        if (id_field)
            key = arr.Value(idx), id_field = false;
        return format(arr, idx);
    }
    arrow::Status Visit(arrow::Int16Array const& arr) {
        if (id_field)
            key = arr.Value(idx), id_field = false;
        return format(arr, idx);
    }
    arrow::Status Visit(arrow::Int32Array const& arr) {
        if (id_field)
            key = arr.Value(idx), id_field = false;
        return format(arr, idx);
    }
    arrow::Status Visit(arrow::Int64Array const& arr) {
        if (id_field)
            key = arr.Value(idx), id_field = false;
        return format(arr, idx);
    }
    arrow::Status Visit(arrow::UInt8Array const& arr) {
        if (id_field)
            key = arr.Value(idx), id_field = false;
        return format(arr, idx);
    }
    arrow::Status Visit(arrow::UInt16Array const& arr) {
        if (id_field)
            key = arr.Value(idx), id_field = false;
        return format(arr, idx);
    }
    arrow::Status Visit(arrow::UInt32Array const& arr) {
        if (id_field)
            key = arr.Value(idx), id_field = false;
        return format(arr, idx);
    }
    arrow::Status Visit(arrow::UInt64Array const& arr) {
        if (id_field)
            key = arr.Value(idx), id_field = false;
        return format(arr, idx);
    }
    arrow::Status Visit(arrow::HalfFloatArray const& arr) {
        if (id_field)
            key = arr.Value(idx), id_field = false;
        return format(arr, idx);
    }
    arrow::Status Visit(arrow::FloatArray const& arr) {
        if (id_field)
            key = arr.Value(idx), id_field = false;
        return format(arr, idx);
    }
    arrow::Status Visit(arrow::DoubleArray const& arr) {
        if (id_field)
            key = arr.Value(idx), id_field = false;
        return format(arr, idx);
    }
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
    bool id_field = false;
    ustore_key_t key;
    size_t idx = 0;

  private:
    inline static constexpr char int_to_hex_k[16] = {
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

    inline void char_to_hex(uint8_t const c, uint8_t* hex) noexcept {
        hex[0] = int_to_hex_k[c >> 4];
        hex[1] = int_to_hex_k[c & 0x0F];
    }

    template <typename at>
    arrow::Status format(at const& cont, size_t idx) {
        fmt::format_to(std::back_inserter(json), "{},", cont.Value(idx));
        return arrow::Status::OK();
    }

    template <typename at>
    arrow::Status format_bin_str(at const& cont, size_t idx) {
        auto str = cont.Value(idx);
        std::string output;
        output.reserve(str.size());
        for (std::size_t ch_idx = 0; ch_idx != str.size(); ++ch_idx) {
            uint8_t c = str[ch_idx];
            switch (c) {
            case 34: output += "\\\""; break;
            case 92: output += "\\\\"; break;
            case 8: output += "\\b"; break;
            case 9: output += "\\t"; break;
            case 10: output += "\\n"; break;
            case 12: output += "\\f"; break;
            case 13: output += "\\r"; break;
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
            case 7:
            case 11:
            case 14:
            case 15:
            case 16:
            case 17:
            case 18:
            case 19:
            case 20:
            case 21:
            case 22:
            case 23:
            case 24:
            case 25:
            case 26:
            case 27:
            case 28:
            case 29:
            case 30:
            case 31: {
                output += "\\u0000";
                auto target_ptr = reinterpret_cast<uint8_t*>(output.data() + output.size() - 2);
                char_to_hex(c, target_ptr);
                break;
            }
            default: output += c;
            }
        }
        if (output.back() == '\n')
            output.pop_back();
        fmt::format_to(std::back_inserter(json), "\"{}\",", output.data());
        return arrow::Status::OK();
    }
};

void make_ndjson_docs() {
    std::string dataset_path = dataset_path_k;
    dataset_path = home_path / dataset_path.substr(2);

    std::filesystem::path pt {dataset_path};
    size_t size = std::filesystem::file_size(pt);

    int fd = open(sample_path_k, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
    auto handle = open(dataset_path.c_str(), O_RDONLY);
    auto begin = mmap(NULL, size, PROT_READ, MAP_PRIVATE, handle, 0);
    std::string_view mapped_content = std::string_view(reinterpret_cast<char const*>(begin), size);
    madvise(begin, size, MADV_SEQUENTIAL);

    auto get_value = [](simdjson::ondemand::object& obj, ustore_str_view_t field) {
        return (field[0] == '/') ? rewind(obj).at_pointer(field) : rewind(obj)[field];
    };

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs = parser.iterate_many( //
        mapped_content.data(),
        mapped_content.size(),
        1000000ul);

    std::string json;
    size_t row = 0;
    bool state = false;
    for (auto doc : docs) {
        simdjson::ondemand::object obj = doc.get_object().value();
        state = false;
        json = "{";
        for (size_t idx = 0; idx < fields_paths_count_k; ++idx) {
            auto result = get_value(obj, fields_paths_ak[idx]);
            if (result.error() != simdjson::SUCCESS) {
                state = true;
                break;
            }
        }
        if (state)
            continue;
        for (size_t idx = 0; idx < prefixes_count_k; ++idx) {
            switch (obj[prefixes_ak[idx]].type()) {
            case simdjson::ondemand::json_type::array:
                fmt::format_to(std::back_inserter(json),
                               "\"{}\":{},",
                               prefixes_ak[idx],
                               obj[prefixes_ak[idx]].get_array().value().raw_json().value());
                break;
            case simdjson::ondemand::json_type::object:
                fmt::format_to(std::back_inserter(json),
                               "\"{}\":{},",
                               prefixes_ak[idx],
                               obj[prefixes_ak[idx]].get_object().value().raw_json().value());
                break;
            case simdjson::ondemand::json_type::number:
                fmt::format_to(std::back_inserter(json),
                               "\"{}\":{},",
                               prefixes_ak[idx],
                               std::string_view(obj[prefixes_ak[idx]].raw_json_token().value()));
                break;
            case simdjson::ondemand::json_type::string:
                fmt::format_to(std::back_inserter(json),
                               "\"{}\":{},",
                               prefixes_ak[idx],
                               std::string_view(obj[prefixes_ak[idx]].raw_json_token().value()));
                break;
            case simdjson::ondemand::json_type::boolean:
                fmt::format_to(std::back_inserter(json),
                               "\"{}\":{},",
                               prefixes_ak[idx],
                               std::string_view(obj[prefixes_ak[idx]].value()));
                break;
            default: break;
            }
        }
        json.back() = '}';
        json.push_back('\n');

        write(fd, json.data(), json.size());
        ++row;
        if (row == rows_count_k)
            break;
    }
    close(fd);
    close(handle);
}

void delete_test_file() {
    std::remove(sample_path_k);
}

void fill_expected() {
    std::string dataset_path = ndjson_path_k;
    dataset_path = home_path / dataset_path.substr(2);

    std::filesystem::path pt {dataset_path.c_str()};
    size_t size = std::filesystem::file_size(pt);

    auto handle = open(dataset_path.c_str(), O_RDONLY);
    auto begin = mmap(NULL, size, PROT_READ, MAP_PRIVATE, handle, 0);
    std::string_view mapped_content = std::string_view(reinterpret_cast<char const*>(begin), size);
    madvise(begin, size, MADV_SEQUENTIAL);

    auto get_value = [](simdjson::ondemand::object& obj, ustore_str_view_t field) {
        return (field[0] == '/') ? rewind(obj).at_pointer(field) : rewind(obj)[field];
    };

    std::ifstream file(dataset_path.c_str());
    size_t rows_count = std::count(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>(), '\n');

    expected_edges.reserve(rows_count);
    edge_t edge;

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs = parser.iterate_many( //
        mapped_content.data(),
        mapped_content.size(),
        1000000ul);

    for (auto doc : docs) {
        simdjson::ondemand::object obj = doc.get_object().value();
        try {
            edge = edge_t {
                get_value(obj, source_field_k),
                get_value(obj, target_field_k),
                get_value(obj, edge_field_k),
            };
        }
        catch (simdjson::simdjson_error const& ex) {
            continue;
        }
        expected_edges.push_back(edge);
    }
    std::sort(expected_edges.begin(), expected_edges.end(), [](auto const& lhs, auto const& rhs) {
        return lhs.source_id < rhs.source_id;
    });
    close(handle);
}

void fill_array_from_ndjson(graph_t& array, std::string_view const& mapped_content) {

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs = parser.iterate_many( //
        mapped_content.data(),
        mapped_content.size(),
        1000000ul);

    auto get_value = [](simdjson::ondemand::object& data, ustore_str_view_t field) {
        return (field[0] == '/') ? rewind(data).at_pointer(field) : rewind(data)[field];
    };

    for (auto doc : docs) {
        simdjson::ondemand::object data = doc.get_object().value();
        array.push_back(edge_t {
            get_value(data, source_field_k),
            get_value(data, target_field_k),
            get_value(data, edge_field_k),
        });
    }
}

void fill_array_from_table(graph_t& array, std::shared_ptr<arrow::Table>& table) {

    auto sources = table->GetColumnByName(source_field_k);
    auto targets = table->GetColumnByName(target_field_k);
    auto edges = table->GetColumnByName(edge_field_k);
    size_t count = sources->num_chunks();
    array.reserve(ustore_size_t(sources->chunk(0)->length()));

    for (size_t chunk_idx = 0; chunk_idx != count; ++chunk_idx) {
        auto source_chunk = sources->chunk(chunk_idx);
        auto target_chunk = targets->chunk(chunk_idx);
        auto edge_chunk = edges->chunk(chunk_idx);
        auto source_array = std::static_pointer_cast<arrow::Int64Array>(source_chunk);
        auto target_array = std::static_pointer_cast<arrow::Int64Array>(target_chunk);
        auto edge_array = std::static_pointer_cast<arrow::Int64Array>(edge_chunk);
        for (size_t value_idx = 0; value_idx != source_array->length(); ++value_idx) {
            array.push_back(edge_t {
                source_array->Value(value_idx),
                target_array->Value(value_idx),
                edge_array->Value(value_idx),
            });
        }
    }
}

void fill_array(ustore_str_view_t file_name, graph_t& array) {

    auto ext = std::filesystem::path(file_name).extension();

    if (ext == ".ndjson") {
        std::filesystem::path pt {file_name};
        size_t size = std::filesystem::file_size(pt);

        auto handle = open(file_name, O_RDONLY);
        auto begin = mmap(NULL, size, PROT_READ, MAP_PRIVATE, handle, 0);
        std::string_view mapped_content = std::string_view(reinterpret_cast<char const*>(begin), size);
        madvise(begin, size, MADV_SEQUENTIAL);
        fill_array_from_ndjson(array, mapped_content);
        munmap((void*)mapped_content.data(), mapped_content.size());
    }
    else {
        std::shared_ptr<arrow::Table> table;
        if (ext == ".parquet") {
            arrow::MemoryPool* pool = arrow::default_memory_pool();
            auto input = *arrow::io::ReadableFile::Open(file_name);
            std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
            parquet::arrow::OpenFile(input, pool, &arrow_reader);
            arrow_reader->ReadTable(&table);
        }
        else if (ext == ".csv") {
            std::shared_ptr<arrow::io::InputStream> input = *arrow::io::ReadableFile::Open(file_name);
            auto read_options = arrow::csv::ReadOptions::Defaults();
            auto parse_options = arrow::csv::ParseOptions::Defaults();
            auto convert_options = arrow::csv::ConvertOptions::Defaults();
            arrow::io::IOContext io_context = arrow::io::default_io_context();

            std::shared_ptr<arrow::csv::TableReader> reader =
                *arrow::csv::TableReader::Make(io_context, input, read_options, parse_options, convert_options);
            table = *reader->Read();
        }
        fill_array_from_table(array, table);
    }
}

bool cmp_graph(ustore_str_view_t left) {

    graph_t edges;

    fill_array(left, edges);
    EXPECT_EQ(edges.size(), expected_edges.size());
    std::sort(edges.begin(), edges.end(), [](auto const& lhs, auto const& rhs) {
        return lhs.source_id < rhs.source_id;
    });

    for (size_t idx = 0; idx < expected_edges.size(); ++idx) {
        EXPECT_EQ(expected_edges[idx].source_id, edges[idx].source_id);
        EXPECT_EQ(expected_edges[idx].target_id, edges[idx].target_id);
        EXPECT_EQ(expected_edges[idx].id, edges[idx].id);
    }
    return true;
}

bool test_graph(ustore_str_view_t file, ustore_str_view_t ext) {

    database_t db;
    auto collection = db.main();
    arena_t arena(db);
    status_t status;

    std::vector<std::string> updated_paths;
    std::string new_file;

    std::string dataset_path = file;
    if (std::strcmp(sample_path_k, file) != 0)
        dataset_path = home_path / dataset_path.substr(2);

    ustore_graph_import_t imp {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = dataset_path.c_str(),
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = source_field_k,
        .target_id_field = target_field_k,
        .edge_id_field = edge_field_k,
    };
    ustore_graph_import(&imp);

    EXPECT_TRUE(status);

    ustore_graph_export_t exp {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = ext,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = source_field_k,
        .target_id_field = target_field_k,
        .edge_id_field = edge_field_k,
    };
    ustore_graph_export(&exp);

    EXPECT_TRUE(status);

    for (const auto& entry : fs::directory_iterator(path_k))
        updated_paths.push_back(entry.path());

    EXPECT_GT(updated_paths.size(), paths.size());

    for (size_t idx = 0; idx < paths.size(); ++idx) {
        if (paths[idx] != updated_paths[idx]) {
            new_file = updated_paths[idx];
            break;
        }
    }
    if (new_file.size() == 0)
        new_file = updated_paths.back();

    new_file.erase(0, 2);
    EXPECT_TRUE(cmp_graph(new_file.c_str()));

    std::remove(new_file.c_str());
    db.clear().throw_unhandled();
    return true;
}

void fill_from_table(std::shared_ptr<arrow::Table>& table) {
    auto id = table->GetColumnByName(id_k);
    auto doc = table->GetColumnByName(doc_k);
    size_t count = id->num_chunks();

    for (size_t chunk_idx = 0; chunk_idx != count; ++chunk_idx) {
        auto id_chunk = id->chunk(chunk_idx);
        auto doc_chunk = doc->chunk(chunk_idx);
        auto id_array = std::static_pointer_cast<arrow::Int64Array>(id_chunk);
        auto doc_array = std::static_pointer_cast<arrow::BinaryArray>(doc_chunk);
        for (size_t value_idx = 0; value_idx != id_array->length(); ++value_idx)
            docs_w_keys[id_array->Value(value_idx)] = doc_array->Value(value_idx);
    }
}

void fill_from_ndjson(ustore_str_view_t file_name) {

    std::filesystem::path pt {file_name};
    size_t size = std::filesystem::file_size(pt);

    auto handle = open(file_name, O_RDONLY);
    auto begin = mmap(NULL, size, PROT_READ, MAP_PRIVATE, handle, 0);
    std::string_view mapped_content = std::string_view(reinterpret_cast<char const*>(begin), size);
    madvise(begin, size, MADV_SEQUENTIAL);

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs = parser.iterate_many( //
        mapped_content.data(),
        mapped_content.size(),
        1000000ul);

    for (auto doc : docs) {
        simdjson::ondemand::object obj = doc.get_object().value();
        auto data = rewind(obj)[doc_k].get_object().value().raw_json().value();
        auto str = std::string(data.data(), data.size());
        ustore_key_t key = rewind(obj)[id_k];
        docs_w_keys[key] = str;
    }
    close(handle);
}

void fill_docs_w_keys(ustore_str_view_t file_name) {

    docs_w_keys.clear();
    auto ext = std::filesystem::path(file_name).extension();

    if (ext == ".ndjson")
        fill_from_ndjson(file_name);
    else {
        std::shared_ptr<arrow::Table> table;
        if (ext == ".parquet") {
            arrow::MemoryPool* pool = arrow::default_memory_pool();
            auto input = *arrow::io::ReadableFile::Open(file_name);
            std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
            auto _ = parquet::arrow::OpenFile(input, pool, &arrow_reader);
            _ = arrow_reader->ReadTable(&table);
        }
        else if (ext == ".csv") {
            std::shared_ptr<arrow::io::InputStream> input = *arrow::io::ReadableFile::Open(file_name);
            auto read_options = arrow::csv::ReadOptions::Defaults();
            auto parse_options = arrow::csv::ParseOptions::Defaults();
            auto convert_options = arrow::csv::ConvertOptions::Defaults();
            arrow::io::IOContext io_context = arrow::io::default_io_context();

            std::shared_ptr<arrow::csv::TableReader> reader =
                *arrow::csv::TableReader::Make(io_context, input, read_options, parse_options, convert_options);
            table = *reader->Read();
        }
        fill_from_table(table);
    }
}

bool cmp_ndjson_docs_sub(ustore_str_view_t lhs, ustore_str_view_t rhs) {

    std::filesystem::path pt {lhs};
    size_t size = std::filesystem::file_size(pt);

    auto handle = open(lhs, O_RDONLY);
    auto begin = mmap(NULL, size, PROT_READ, MAP_PRIVATE, handle, 0);
    std::string_view mapped_content = std::string_view(reinterpret_cast<char const*>(begin), size);
    madvise(begin, size, MADV_SEQUENTIAL);
    fill_docs_w_keys(rhs);

    auto get_value = [](simdjson::ondemand::object& obj, ustore_str_view_t field) {
        return (field[0] == '/') ? rewind(obj).at_pointer(field) : rewind(obj)[field];
    };

    simdjson::ondemand::parser parser_l;
    simdjson::ondemand::parser parser_r;
    simdjson::ondemand::document_stream docs = parser_l.iterate_many( //
        mapped_content.data(),
        mapped_content.size(),
        1000000ul);

    for (auto doc_l : docs) {
        simdjson::ondemand::object obj_l = doc_l.get_object().value();
        simdjson::ondemand::document doc_r = parser_r.iterate( //
            docs_w_keys[get_value(obj_l, fields_paths_ak[0])],
            1000000);
        simdjson::ondemand::object obj_r = doc_r.get_object().value();

        for (size_t idx = 0; idx < fields_paths_count_k; ++idx) {
            auto data_l = get_value(obj_l, fields_paths_ak[idx]);
            auto data_r = get_value(obj_r, fields_paths_ak[idx]);
            switch (data_l.type()) {
            case simdjson::ondemand::json_type::object:
                EXPECT_EQ(data_l.get_object().value().raw_json().value(),
                          data_r.get_object().value().raw_json().value());
                break;
            case simdjson::ondemand::json_type::array:
                EXPECT_EQ(data_l.get_array().value().raw_json().value(), data_r.get_array().value().raw_json().value());
                break;
            case simdjson::ondemand::json_type::string:
                EXPECT_EQ(data_l.raw_json_token().value(), data_r.raw_json_token().value());
                break;
            case simdjson::ondemand::json_type::number: {
                if (data_l.is_integer())
                    EXPECT_EQ(ustore_key_t(data_l), ustore_key_t(data_r));
                else
                    EXPECT_EQ(double(data_l), double(data_r));
            } break;
            case simdjson::ondemand::json_type::boolean: EXPECT_EQ(bool(data_l), bool(data_r)); break;
            default: break;
            }
        }
    }

    munmap((void*)mapped_content.data(), mapped_content.size());
    close(handle);
    return true;
}

bool cmp_ndjson_docs_whole(ustore_str_view_t lhs, ustore_str_view_t rhs) {

    std::filesystem::path pt {lhs};
    size_t size = std::filesystem::file_size(pt);

    auto handle = open(lhs, O_RDONLY);
    auto begin = mmap(NULL, size, PROT_READ, MAP_PRIVATE, handle, 0);
    std::string_view mapped_content = std::string_view(reinterpret_cast<char const*>(begin), size);
    madvise(begin, size, MADV_SEQUENTIAL);
    fill_docs_w_keys(rhs);

    auto get_value = [](simdjson::ondemand::object& obj, ustore_str_view_t field) {
        return (field[0] == '/') ? rewind(obj).at_pointer(field) : rewind(obj)[field];
    };

    simdjson::ondemand::parser parser_l;
    simdjson::ondemand::parser parser_r;
    simdjson::ondemand::document_stream docs = parser_l.iterate_many( //
        mapped_content.data(),
        mapped_content.size(),
        1000000ul);

    for (auto doc_l : docs) {
        simdjson::ondemand::object obj_l = doc_l.get_object().value();
        simdjson::ondemand::document doc_r = parser_r.iterate( //
            docs_w_keys[get_value(obj_l, fields_paths_ak[0])],
            1000000);
        simdjson::ondemand::object obj_r = doc_r.get_object().value();

        auto data_l = rewind(obj_l).raw_json().value();
        auto data_r = rewind(obj_r).raw_json().value();
        std::string str_l(data_l.data(), data_l.size() - 1);
        std::string str_r(data_r.data(), data_r.size());
        EXPECT_EQ(str_l, str_r);
    }

    munmap((void*)mapped_content.data(), mapped_content.size());
    close(handle);
    return true;
}

bool cmp_table_docs_whole(ustore_str_view_t lhs, ustore_str_view_t rhs) {

    docs_t docs_w_keys_;
    std::string json = "{";
    std::vector<ustore_key_t> keys;
    arrow_visitor_at visitor(json);
    std::shared_ptr<arrow::Table> table;
    auto ext = std::filesystem::path(lhs).extension();
    std::vector<std::shared_ptr<arrow::Array>> chunks;
    std::vector<std::shared_ptr<arrow::ChunkedArray>> columns;

    if (ext == ".csv") {
        std::shared_ptr<arrow::io::InputStream> input = *arrow::io::ReadableFile::Open(lhs);
        auto read_options = arrow::csv::ReadOptions::Defaults();
        auto parse_options = arrow::csv::ParseOptions::Defaults();
        auto convert_options = arrow::csv::ConvertOptions::Defaults();
        arrow::io::IOContext io_context = arrow::io::default_io_context();
        std::shared_ptr<arrow::csv::TableReader> reader =
            *arrow::csv::TableReader::Make(io_context, input, read_options, parse_options, convert_options);
        table = *reader->Read();
    }
    else if (ext == ".parquet") {
        arrow::MemoryPool* pool = arrow::default_memory_pool();
        auto input = *arrow::io::ReadableFile::Open(lhs);
        std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
        auto _ = parquet::arrow::OpenFile(input, pool, &arrow_reader);
        _ = arrow_reader->ReadTable(&table);
    }
    fill_docs_w_keys(rhs);

    auto fields = table->ColumnNames();
    chunks.resize(fields.size());
    columns.resize(fields.size());
    for (size_t idx = 0; idx < fields.size(); ++idx)
        columns[idx] = table->GetColumnByName(fields[idx]);

    size_t count = columns[0]->num_chunks();
    for (size_t chunk_idx = 0, g_idx = 0; chunk_idx != count; ++chunk_idx, g_idx = 0) {
        for (auto column : columns) {
            chunks[g_idx] = column->chunk(chunk_idx);
            ++g_idx;
        }
        for (size_t value_idx = 0; value_idx < columns[0]->chunk(chunk_idx)->length(); ++value_idx) {
            g_idx = 0;
            visitor.id_field = true;
            for (auto field : fields) {
                fmt::format_to(std::back_inserter(json), "\"{}\":", field);
                visitor.idx = value_idx;
                auto _ = arrow::VisitArrayInline(*chunks[g_idx].get(), &visitor);
                ++g_idx;
            }
            json[json.size() - 1] = '}';
            docs_w_keys_[visitor.key] = json;
            keys.push_back(visitor.key);
            json = "{";
        }
    }

    for (auto key : keys)
        EXPECT_EQ(docs_w_keys[key], docs_w_keys_[key]);

    return true;
}

bool cmp_table_docs_sub(ustore_str_view_t lhs, ustore_str_view_t rhs) {

    docs_t docs_w_keys_;
    std::string json = "{";
    std::vector<ustore_key_t> keys;
    arrow_visitor_at visitor(json);
    std::shared_ptr<arrow::Table> table;
    auto ext = std::filesystem::path(lhs).extension();
    std::vector<std::shared_ptr<arrow::Array>> chunks(fields_columns_count_k);
    std::vector<std::shared_ptr<arrow::ChunkedArray>> columns(fields_columns_count_k);

    if (ext == ".csv") {
        std::shared_ptr<arrow::io::InputStream> input = *arrow::io::ReadableFile::Open(lhs);
        auto read_options = arrow::csv::ReadOptions::Defaults();
        auto parse_options = arrow::csv::ParseOptions::Defaults();
        auto convert_options = arrow::csv::ConvertOptions::Defaults();
        arrow::io::IOContext io_context = arrow::io::default_io_context();
        std::shared_ptr<arrow::csv::TableReader> reader =
            *arrow::csv::TableReader::Make(io_context, input, read_options, parse_options, convert_options);
        table = *reader->Read();
    }
    else if (ext == ".parquet") {
        arrow::MemoryPool* pool = arrow::default_memory_pool();
        auto input = *arrow::io::ReadableFile::Open(lhs);
        std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
        auto _ = parquet::arrow::OpenFile(input, pool, &arrow_reader);
        _ = arrow_reader->ReadTable(&table);
    }
    fill_docs_w_keys(rhs);

    for (size_t idx = 0; idx < fields_columns_count_k; ++idx)
        columns[idx] = table->GetColumnByName(fields_columns_ak[idx]);

    size_t count = columns[0]->num_chunks();
    for (size_t chunk_idx = 0, g_idx = 0; chunk_idx != count; ++chunk_idx, g_idx = 0) {
        for (auto column : columns) {
            chunks[g_idx] = column->chunk(chunk_idx);
            ++g_idx;
        }
        for (size_t value_idx = 0; value_idx < columns[0]->chunk(chunk_idx)->length(); ++value_idx) {
            g_idx = 0;
            visitor.id_field = true;
            for (auto it = fields_columns_ak; g_idx < fields_columns_count_k; ++g_idx, ++it) {
                fmt::format_to(std::back_inserter(json), "\"{}\":", *it);
                visitor.idx = value_idx;
                auto _ = arrow::VisitArrayInline(*chunks[g_idx].get(), &visitor);
            }
            json[json.size() - 1] = '}';
            docs_w_keys_[visitor.key] = json;
            keys.push_back(visitor.key);
            json = "{";
        }
    }

    for (auto key : keys)
        EXPECT_EQ(docs_w_keys[key], docs_w_keys_[key]);

    return true;
}

template <typename comparator>
bool test_sub_docs(ustore_str_view_t file, ustore_str_view_t ext, comparator cmp, bool state = false) {
    clear_environment();

    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    auto collection = db.main();
    arena_t arena(db);
    status_t status;

    std::vector<std::string> updated_paths;
    std::string new_file;

    std::string dataset_path = file;
    if (std::strcmp(sample_path_k, file) != 0)
        dataset_path = home_path / dataset_path.substr(2);

    ustore_docs_import_t docs {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = dataset_path.c_str(),
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .fields_count = state ? fields_columns_count_k : fields_paths_count_k,
        .fields = state ? fields_columns_ak : fields_paths_ak,
        .fields_stride = sizeof(ustore_str_view_t),
        .id_field = fields_columns_ak[0],
    };
    ustore_docs_import(&docs);

    EXPECT_TRUE(status);

    ustore_docs_export_t exdocs {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = ext,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .fields_count = state ? fields_columns_count_k : fields_paths_count_k,
        .fields = state ? fields_columns_ak : fields_paths_ak,
        .fields_stride = sizeof(ustore_str_view_t),
    };
    ustore_docs_export(&exdocs);

    EXPECT_TRUE(status);

    for (const auto& entry : fs::directory_iterator(path_k))
        updated_paths.push_back(entry.path());

    EXPECT_GT(updated_paths.size(), paths.size());

    for (size_t idx = 0; idx < paths.size(); ++idx) {
        if (paths[idx] != updated_paths[idx]) {
            new_file = updated_paths[idx];
            break;
        }
    }
    if (new_file.size() == 0)
        new_file = updated_paths.back();

    new_file.erase(0, 2);
    EXPECT_TRUE(cmp(dataset_path.c_str(), new_file.data()));

    std::remove(new_file.data());
    db.clear().throw_unhandled();
    return true;
}

template <typename comparator>
bool test_whole_docs(ustore_str_view_t file, ustore_str_view_t ext, comparator cmp, bool state = false) {
    clear_environment();

    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    auto collection = db.main();
    arena_t arena(db);
    status_t status;

    std::vector<std::string> updated_paths;
    std::string new_file;

    std::string dataset_path = file;
    if (std::strcmp(sample_path_k, file) != 0)
        dataset_path = home_path / dataset_path.substr(2);

    ustore_docs_import_t docs {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = dataset_path.c_str(),
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .id_field = fields_paths_ak[0],
    };
    ustore_docs_import(&docs);

    EXPECT_TRUE(status);

    ustore_docs_export_t exdocs {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = ext,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
    };
    ustore_docs_export(&exdocs);

    EXPECT_TRUE(status);

    for (const auto& entry : fs::directory_iterator(path_k))
        updated_paths.push_back(entry.path());

    EXPECT_GT(updated_paths.size(), paths.size());

    for (size_t idx = 0; idx < paths.size(); ++idx) {
        if (paths[idx] != updated_paths[idx]) {
            new_file = updated_paths[idx];
            break;
        }
    }
    if (new_file.size() == 0)
        new_file = updated_paths.back();

    new_file.erase(0, 2);
    EXPECT_TRUE(cmp(dataset_path.c_str(), new_file.data()));

    std::remove(new_file.data());
    db.clear().throw_unhandled();
    return true;
}

bool test_crash_cases_graph_import(ustore_str_view_t file) {
    database_t db;
    auto collection = db.main();
    arena_t arena(db);
    status_t status;

    std::string dataset_path = file;
    if (std::strcmp(sample_path_k, file) != 0)
        dataset_path = home_path / dataset_path.substr(2);

    ustore_graph_import_t imp_path_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = nullptr,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = source_field_k,
        .target_id_field = target_field_k,
        .edge_id_field = edge_field_k,
    };
    ustore_graph_import(&imp_path_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_graph_import_t imp_source_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = dataset_path.c_str(),
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = nullptr,
        .target_id_field = target_field_k,
        .edge_id_field = edge_field_k,
    };
    ustore_graph_import(&imp_source_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_graph_import_t imp_target_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = dataset_path.c_str(),
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = source_field_k,
        .target_id_field = nullptr,
        .edge_id_field = edge_field_k,
    };
    ustore_graph_import(&imp_target_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_graph_import_t imp_edge_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = dataset_path.c_str(),
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = source_field_k,
        .target_id_field = target_field_k,
        .edge_id_field = nullptr,
    };
    ustore_graph_import(&imp_edge_null);
    EXPECT_TRUE(status);
    status.release_error();

    ustore_graph_import_t imp_db_null {
        .db = nullptr,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = dataset_path.c_str(),
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = source_field_k,
        .target_id_field = target_field_k,
        .edge_id_field = edge_field_k,
    };
    ustore_graph_import(&imp_db_null);
    EXPECT_FALSE(status);
    db.clear().throw_unhandled();
    return true;
}

bool test_crash_cases_graph_export(ustore_str_view_t ext) {
    database_t db;
    auto collection = db.main();
    arena_t arena(db);
    status_t status;

    ustore_graph_export_t exp_path_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = nullptr,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = source_field_k,
        .target_id_field = target_field_k,
        .edge_id_field = edge_field_k,
    };
    ustore_graph_export(&exp_path_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_graph_export_t exp_source_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = ext,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = nullptr,
        .target_id_field = target_field_k,
        .edge_id_field = edge_field_k,
    };
    ustore_graph_export(&exp_source_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_graph_export_t exp_target_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = ext,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = source_field_k,
        .target_id_field = nullptr,
        .edge_id_field = edge_field_k,
    };
    ustore_graph_export(&exp_target_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_graph_export_t exp_edge_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = ext,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = source_field_k,
        .target_id_field = target_field_k,
        .edge_id_field = nullptr,
    };
    ustore_graph_export(&exp_edge_null);
    EXPECT_TRUE(status);
    status.release_error();

    ustore_graph_export_t exp_db_null {
        .db = nullptr,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = ext,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .source_id_field = source_field_k,
        .target_id_field = target_field_k,
        .edge_id_field = edge_field_k,
    };
    ustore_graph_export(&exp_db_null);
    EXPECT_FALSE(status);

    for (const auto& entry : fs::directory_iterator(path_k)) {
        std::string path = entry.path();
        if (std::strcmp(path.data() + (path.size() - strlen(ext)), ext) == 0)
            std::remove(path.data());
    }
    db.clear().throw_unhandled();
    return true;
}

bool test_crash_cases_docs_import(ustore_str_view_t file) {
    clear_environment();

    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    auto collection = db.main();
    arena_t arena(db);
    status_t status;

    std::string dataset_path = file;
    if (std::strcmp(sample_path_k, file) != 0)
        dataset_path = home_path / dataset_path.substr(2);

    ustore_docs_import_t imp_path_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = nullptr,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .fields_count = prefixes_count_k,
        .fields = prefixes_ak,
        .fields_stride = sizeof(ustore_str_view_t),
    };
    ustore_docs_import(&imp_path_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_docs_import_t imp_count_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = dataset_path.c_str(),
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .fields_count = 0,
        .fields = prefixes_ak,
        .fields_stride = sizeof(ustore_str_view_t),
    };
    ustore_docs_import(&imp_count_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_docs_import_t imp_fields_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = dataset_path.c_str(),
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .fields_count = prefixes_count_k,
        .fields = nullptr,
        .fields_stride = sizeof(ustore_str_view_t),
    };
    ustore_docs_import(&imp_fields_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_docs_import_t imp_stride_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = dataset_path.c_str(),
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .fields_count = prefixes_count_k,
        .fields = prefixes_ak,
        .fields_stride = 0,
    };
    ustore_docs_import(&imp_stride_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_docs_import_t imp_db_null {
        .db = nullptr,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .collection = collection,
        .paths_pattern = dataset_path.c_str(),
        .fields_count = prefixes_count_k,
        .fields = prefixes_ak,
        .fields_stride = sizeof(ustore_str_view_t),
    };
    ustore_docs_import(&imp_db_null);
    EXPECT_FALSE(status);
    db.clear().throw_unhandled();
    return true;
}

bool test_crash_cases_docs_export(ustore_str_view_t ext) {
    clear_environment();

    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    auto collection = db.main();
    arena_t arena(db);
    status_t status;

    ustore_docs_export_t imp_path_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = nullptr,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .fields_count = prefixes_count_k,
        .fields = prefixes_ak,
        .fields_stride = sizeof(ustore_str_view_t),
    };
    ustore_docs_export(&imp_path_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_docs_export_t imp_count_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = ext,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .fields_count = 0,
        .fields = prefixes_ak,
        .fields_stride = sizeof(ustore_str_view_t),
    };
    ustore_docs_export(&imp_count_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_docs_export_t imp_fields_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = ext,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .fields_count = prefixes_count_k,
        .fields = nullptr,
        .fields_stride = sizeof(ustore_str_view_t),
    };
    ustore_docs_export(&imp_fields_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_docs_export_t imp_stride_null {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = ext,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .fields_count = prefixes_count_k,
        .fields = prefixes_ak,
        .fields_stride = 0,
    };
    ustore_docs_export(&imp_stride_null);
    EXPECT_FALSE(status);
    status.release_error();

    ustore_docs_export_t imp_db_null {
        .db = nullptr,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = ext,
        .max_batch_size = max_batch_size_k,
        .callback = nullptr,
        .callback_payload = nullptr,
        .fields_count = prefixes_count_k,
        .fields = prefixes_ak,
        .fields_stride = sizeof(ustore_str_view_t),
    };
    ustore_docs_export(&imp_db_null);
    EXPECT_FALSE(status);
    db.clear().throw_unhandled();
    return true;
}

TEST(import_export_graph, ndjosn_ndjson) {
    test_graph(ndjson_path_k, ext_ndjson_k);
}
TEST(import_export_graph, ndjosn_parquet) {
    test_graph(ndjson_path_k, ext_parquet_k);
}
TEST(import_export_graph, ndjosn_csv) {
    test_graph(ndjson_path_k, ext_csv_k);
}

TEST(import_export_graph, parquet_ndjson) {
    test_graph(parquet_path_k, ext_ndjson_k);
}
TEST(import_export_graph, parquet_parquet) {
    test_graph(parquet_path_k, ext_parquet_k);
}
TEST(import_export_graph, parquet_csv) {
    test_graph(parquet_path_k, ext_csv_k);
}

TEST(import_export_graph, csv_ndjson) {
    test_graph(csv_path_k, ext_ndjson_k);
}
TEST(import_export_graph, csv_parquet) {
    test_graph(csv_path_k, ext_parquet_k);
}
TEST(import_export_graph, csv_csv) {
    test_graph(csv_path_k, ext_csv_k);
}

TEST(import_export_docs_whole, ndjosn_ndjson) {
    test_whole_docs(sample_path_k, ext_ndjson_k, cmp_ndjson_docs_whole);
}
TEST(import_export_docs_whole, ndjosn_parquet) {
    test_whole_docs(sample_path_k, ext_parquet_k, cmp_ndjson_docs_whole);
}
TEST(import_export_docs_whole, ndjosn_csv) {
    test_whole_docs(sample_path_k, ext_csv_k, cmp_ndjson_docs_whole);
}

TEST(import_export_docs_whole, parquet_ndjson) {
    test_whole_docs(parquet_path_k, ext_ndjson_k, cmp_table_docs_whole, true);
}
TEST(import_export_docs_whole, parquet_parquet) {
    test_whole_docs(parquet_path_k, ext_parquet_k, cmp_table_docs_whole, true);
}
TEST(import_export_docs_whole, parquet_csv) {
    test_whole_docs(parquet_path_k, ext_csv_k, cmp_table_docs_whole, true);
}

TEST(import_export_docs_whole, csv_ndjson) {
    test_whole_docs(csv_path_k, ext_ndjson_k, cmp_table_docs_whole, true);
}
TEST(import_export_docs_whole, csv_parquet) {
    test_whole_docs(csv_path_k, ext_parquet_k, cmp_table_docs_whole, true);
}
TEST(import_export_docs_whole, csv_csv) {
    test_whole_docs(csv_path_k, ext_csv_k, cmp_table_docs_whole, true);
}

TEST(import_export_docs_sub, ndjosn_ndjson) {
    test_sub_docs(sample_path_k, ext_ndjson_k, cmp_ndjson_docs_sub);
}
TEST(import_export_docs_sub, ndjosn_parquet) {
    test_sub_docs(sample_path_k, ext_parquet_k, cmp_ndjson_docs_sub);
}
TEST(import_export_docs_sub, ndjosn_csv) {
    test_sub_docs(sample_path_k, ext_csv_k, cmp_ndjson_docs_sub);
}

TEST(import_export_docs_sub, parquet_ndjson) {
    test_sub_docs(parquet_path_k, ext_ndjson_k, cmp_table_docs_sub, true);
}
TEST(import_export_docs_sub, parquet_parquet) {
    test_sub_docs(parquet_path_k, ext_parquet_k, cmp_table_docs_sub, true);
}
TEST(import_export_docs_sub, parquet_csv) {
    test_sub_docs(parquet_path_k, ext_csv_k, cmp_table_docs_sub, true);
}

TEST(import_export_docs_sub, csv_ndjson) {
    test_sub_docs(csv_path_k, ext_ndjson_k, cmp_table_docs_sub, true);
}
TEST(import_export_docs_sub, csv_parquet) {
    test_sub_docs(csv_path_k, ext_parquet_k, cmp_table_docs_sub, true);
}
TEST(import_export_docs_sub, csv_csv) {
    test_sub_docs(csv_path_k, ext_csv_k, cmp_table_docs_sub, true);
}

TEST(crash_cases, graph_import) {
    test_crash_cases_graph_import(ndjson_path_k);
    test_crash_cases_graph_import(parquet_path_k);
    test_crash_cases_graph_import(csv_path_k);
}

TEST(crash_cases, graph_export) {
    test_crash_cases_graph_export(ext_ndjson_k);
    test_crash_cases_graph_export(ext_parquet_k);
    test_crash_cases_graph_export(ext_csv_k);
}

TEST(crash_cases, docs_import) {
    test_crash_cases_docs_import(sample_path_k);
    test_crash_cases_docs_import(parquet_path_k);
    test_crash_cases_docs_import(csv_path_k);
}

TEST(crash_cases, docs_export) {
    test_crash_cases_docs_export(ext_ndjson_k);
    test_crash_cases_docs_export(ext_parquet_k);
    test_crash_cases_docs_export(ext_csv_k);
}


#if defined(USTORE_CLI)
template <typename... args>
void run_command(const char* command, args... arguments) {
    pid_t pid = fork();
    if (pid == -1)
        EXPECT_TRUE(false) << "Failed to run command";
    else if (pid == 0)
        EXPECT_NE(execl(command, command, arguments..., (char*)(NULL)), -1) << "Fail to execute command";
    else
        wait(NULL);
}

bool test_import_export_cli(database_t& db, ustore_str_view_t url, ustore_str_view_t coll_name = nullptr) {

    std::vector<std::string> updated_paths;
    std::string new_file;

    if (coll_name) {
        run_command(cli_path.c_str(),
                    "--url",
                    url,
                    "collection",
                    "import",
                    "--input",
                    ndjson_path_k,
                    "--id",
                    "id",
                    "--mlimit",
                    "1073741824",
                    "--name",
                    coll_name);

        run_command(cli_path.c_str(),
                    "--url",
                    url,
                    "collection",
                    "export",
                    "--output",
                    ".ndjson",
                    "--mlimit",
                    "1073741824",
                    "--name",
                    coll_name);
    }
    else {
        run_command(cli_path.c_str(),
                    "--url",
                    url,
                    "collection",
                    "import",
                    "--input",
                    ndjson_path_k,
                    "--id",
                    "id",
                    "--mlimit",
                    "1073741824");

        run_command(cli_path.c_str(),
                    "--url",
                    url,
                    "collection",
                    "export",
                    "--output",
                    ".ndjson",
                    "--mlimit",
                    "1073741824",
                    "--name",
                    coll_name);
    }

    for (const auto& entry : fs::directory_iterator(path_k))
        updated_paths.push_back(entry.path());

    EXPECT_GT(updated_paths.size(), paths.size());

    for (size_t idx = 0; idx < paths.size(); ++idx) {
        if (paths[idx] != updated_paths[idx]) {
            new_file = updated_paths[idx];
            break;
        }
    }
    if (new_file.size() == 0)
        new_file = updated_paths.back();

    new_file.erase(0, 2);
    EXPECT_TRUE(cmp_ndjson_docs_whole(ndjson_path_k, new_file.data()));

    std::remove(new_file.data());
    return true;
}

TEST(db, cli) {
    clear_environment();

    database_t db;
    auto url = "grpc://0.0.0.0:38709";
    EXPECT_TRUE(db.open(url));
    EXPECT_TRUE(db.clear());
    auto context = context_t {db, nullptr};
    auto maybe_cols = context.collections();
    EXPECT_TRUE(maybe_cols);
    EXPECT_EQ(maybe_cols->ids.size(), 0);

    run_command(cli_path.c_str(), "--url", url, "collection", "create", "--name", "collection1");
    EXPECT_TRUE(db.contains("collection1"));
    EXPECT_TRUE(*db.contains("collection1"));

    EXPECT_TRUE(test_import_export_cli(db, url, "collection1"));

    run_command(cli_path.c_str(), "--url", url, "collection", "drop", "--name", "collection1");
    EXPECT_TRUE(db.contains("collection1"));
    EXPECT_FALSE(*db.contains("collection1"));

    EXPECT_TRUE(test_import_export_cli(db, url));
}

#endif // USTORE_CLI

int main(int argc, char** argv) {

#if defined(USTORE_CLI)
    std::string exec_path = argv[0];
    cli_path = exec_path.substr(0, exec_path.find_last_of("/") + 1) + "ustore";
    srv_path = exec_path.substr(0, exec_path.find_last_of("/") + 1) + "ustore_flight_server_ucset";
#endif // USTORE_CLI

    make_ndjson_docs();
    fill_expected();
    for (const auto& entry : fs::directory_iterator(path_k))
        paths.push_back(ustore_str_span_t(entry.path().c_str()));

    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();

#if defined(USTORE_CLI)
    kill(srv_id, SIGKILL);
    waitpid(srv_id, nullptr, 0);
#endif // USTORE_CLI

    delete_test_file();
    return result;
}
