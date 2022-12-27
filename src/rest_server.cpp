/**
 * @file rest_server.cpp
 * @author Ashot Vardanian
 * @date 2022-06-18
 *
 * @brief A web server implementing @b REST backend on top of any other
 * UKV implementation using pre-release draft of C++23 Networking TS,
 * through the means of @b Boost.Beast, @b Boost.ASIO and @b NLohmann.JSON.
 */

#include <cstdlib>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <thread>   // Thread pool
#include <charconv> // Parsing integers
#include <iostream> // Logging to `std::cerr`
#include <fstream>  // Parsing config file

// Boost files are quite noisy in terms of warnings,
// so let's silence them a bit.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__) || defined(__GNUG__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#elif defined(_MSC_VER)
#endif

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/config.hpp>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__) || defined(__GNUG__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#endif

#include "ukv/ukv.hpp"

namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>

using namespace unum::ukv;
using namespace unum;

static constexpr char const* server_name_k = "unum-cloud/ukv/beast_server";
static constexpr char const* mime_binary_k = "application/octet-stream";
static constexpr char const* mime_json_k = "application/json";
static constexpr char const* mime_msgpack_k = "application/msgpack";
static constexpr char const* mime_cbor_k = "application/cbor";
static constexpr char const* mime_bson_k = "application/bson";
static constexpr char const* mime_ubjson_k = "application/ubjson";

ukv_doc_field_type_t mime_to_format(beast::string_view mime) {
    if (mime == mime_json_k)
        return ukv_field_json_k;
    else if (mime == "application/json-patch+json")
        return ukv_format_json_patch_k;
    else if (mime == mime_msgpack_k)
        return ukv_format_msgpack_k;
    else if (mime == mime_bson_k)
        return ukv_format_bson_k;
    else if (mime == mime_cbor_k)
        return ukv_format_cbor_k;
    else if (mime == mime_ubjson_k)
        return ukv_format_ubjson_k;
    else if (mime == "application/vnd.apache.arrow.stream" || mime == "application/vnd.apache.arrow.file")
        return ukv_format_arrow_k;
    else if (mime == "application/vnd.apache.parquet")
        return ukv_format_parquet_k;
    else
        return ukv_doc_field_default_k;
}

struct db_w_clients_t : public std::enable_shared_from_this<db_w_clients_t> {
    database_t session;
    int running_transactions;
};

void log_failure(beast::error_code ec, char const* what) {
    std::cerr << what << ": " << ec.message() << "\n";
}

template <typename body_at, typename allocator_at>
http::response<http::string_body> make_error(http::request<body_at, http::basic_fields<allocator_at>> const& req,
                                             http::status status,
                                             beast::string_view why) {

    http::response<http::string_body> res {status, req.version()};
    res.set(http::field::server, server_name_k);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req.keep_alive());
    res.body() = std::string(why);
    res.prepare_payload();
    return res;
}

/**
 * @brief Searches for a "value" among key-value pairs passed in URI after path.
 * @param query_params  Must begin with "?" or "/".
 * @param param_name    Must end with "=".
 */
std::optional<beast::string_view> param_value(beast::string_view query_params, beast::string_view param_name) {

    auto key_begin = std::search(query_params.begin(), query_params.end(), param_name.begin(), param_name.end());
    if (key_begin == query_params.end())
        return std::nullopt;

    char preceding_char = *(key_begin - 1);
    bool isnt_part_of_bigger_key = (preceding_char != '?') & (preceding_char != '&') & (preceding_char != '/');
    if (!isnt_part_of_bigger_key)
        return std::nullopt;

    auto value_begin = key_begin + param_name.size();
    auto value_end = std::find(value_begin, query_params.end(), '&');
    return beast::string_view {value_begin, static_cast<size_t>(value_end - value_begin)};
}

template <typename body_at, typename allocator_at, typename send_response_at>
void respond_to_one(db_session_t& session,
                    http::request<body_at, http::basic_fields<allocator_at>>&& req,
                    send_response_at&& send_response) {

    http::verb received_verb = req.method();
    beast::string_view received_path = req.target();

    transaction_t txn(session.db());
    blobs_collection_t collection;
    ukv_key_t key = 0;
    ukv_options_t options = ukv_options_default_k;

    // Parse the `key`
    auto key_begin = received_path.substr(5).begin();
    auto key_end = std::find(key_begin, received_path.end(), '?');
    auto key_parse_result = std::from_chars(key_begin, key_end, key);
    if (key_parse_result.ec != std::errc())
        return send_response(make_error(req, http::status::bad_request, "Couldn't parse the integer key"));

    // Parse the following free-order parameters, starting with transaction identifier.
    auto params_str = beast::string_view {key_end, static_cast<size_t>(received_path.end() - key_end)};
    if (auto txn_val = param_value(params_str, "txn="); txn_val) {
        auto txn_id = size_t {0};
        auto txn_parse_result = std::from_chars(txn_val->data(), txn_val->data() + txn_val->size(), txn_id);
        if (txn_parse_result.ec != std::errc())
            return send_response(make_error(req, http::status::bad_request, "Couldn't parse the transaction id"));
    }

    // Parse the collection name string.
    if (auto collection_val = param_value(params_str, "col="); collection_val) {
        char collection_name_buffer[65] = {0};
        std::memcpy(collection_name_buffer, collection_val->data(), std::min(collection_val->size(), 64ul));

        status_t status;
        ukv_collection_create_t collection_init {
            .db = session.db(),
            .error = error.member_ptr(),
            .name = collection_name_buffer,
            .id = &collection.raw,
        };

        ukv_collection_create(&collection_init);
        if (!status)
            return send_response(make_error(req, http::status::internal_server_error, error.raw));
    }

    // Once we know, which collection, key and transaction user is
    // interested in - perform the actions depending on verbs.
    switch (received_verb) {

        // Read the data:
    case http::verb::get: {

        arena_t tape(session.db());
        status_t status;
        ukv_read_t read {
            .db = session.db(),
            .error = error.member_ptr(),
            .transaction = txn.raw,
            .options = options,
            .collections = &collection.raw,
            .keys = &key,
            .keys_stride = 1,
            .lengths = &tape.capacity,
            .values = &tape.ptr,
        };

        ukv_read(&read);
        if (!status)
            return send_response(make_error(req, http::status::internal_server_error, error.raw));

        ukv_bytes_ptr_t begin = tape.ptr + sizeof(ukv_length_t);
        ukv_length_t len = reinterpret_cast<ukv_length_t*>(tape.ptr)[0];
        if (!len)
            return send_response(make_error(req, http::status::not_found, "Missing key"));

        http::buffer_body::value_type body;
        body.data = begin;
        body.size = len;
        body.more = false;

        http::response<http::buffer_body> res {
            std::piecewise_construct,
            std::make_tuple(std::move(body)),
            std::make_tuple(http::status::ok, req.version()),
        };
        res.set(http::field::server, server_name_k);
        res.set(http::field::content_type, mime_binary_k);
        res.content_length(len);
        res.keep_alive(req.keep_alive());
        return send_response(std::move(res));
    }

        // Check the data:
    case http::verb::head: {

        arena_t tape(session.db());
        status_t status;
        options = ukv_option_read_lengths_k;
        ukv_read_t read {
            .db = session.db(),
            .transaction = txn.raw,
            .collections = &collection.raw,
            .keys = &key,
            .keys_stride = 1,
            .options = options,
            .lengths = &tape.capacity,
            .values = &tape.ptr,
            .error = error.member_ptr(),
        };

        ukv_read(&read);
        if (!status)
            return send_response(make_error(req, http::status::internal_server_error, error.raw));

        ukv_length_t len = reinterpret_cast<ukv_length_t*>(tape.ptr)[0];
        if (!len)
            return send_response(make_error(req, http::status::not_found, "Missing key"));

        http::response<http::empty_body> res;
        res.set(http::field::server, server_name_k);
        res.set(http::field::content_type, mime_binary_k);
        res.content_length(len);
        res.keep_alive(req.keep_alive());
        return send_response(std::move(res));
    }

    // Insert data if it's missing:
    case http::verb::post: {
        arena_t tape(session.db());
        status_t status;
        options = ukv_option_read_lengths_k;
        ukv_read_t read {
            .db = session.db(),
            .transaction = txn.raw,
            .collections = &collection.raw,
            .keys = &key,
            .keys_stride = 1,
            .options = options,
            .lengths = &tape.capacity,
            .values = &tape.ptr,
            .error = error.member_ptr(),
        };

        ukv_read(&read);
        if (!status)
            return send_response(make_error(req, http::status::internal_server_error, error.raw));

        ukv_length_t len = reinterpret_cast<ukv_length_t*>(tape.ptr)[0];
        if (len)
            return send_response(make_error(req, http::status::conflict, "Duplicate key"));

        [[fallthrough]];
    }

        // Upsert data:
    case http::verb::put: {

        auto opt_payload_len = req.payload_size();
        if (!opt_payload_len)
            return send_response(
                make_error(req, http::status::length_required, "Chunk Transfer Encoding isn't supported"));

        // Should we explicitly check the type of the input here?
        auto payload_type = req[http::field::content_type];
        if (payload_type != mime_binary_k)
            return send_response(
                make_error(req, http::status::unsupported_media_type, "Only binary payload is allowed"));

        status_t status;
        auto value = req.body();
        auto value_ptr = reinterpret_cast<ukv_bytes_ptr_t>(value.data());
        auto value_len = static_cast<ukv_length_t>(*opt_payload_len);
        ukv_length_t value_off = 0;

        ukv_write_t write {
            .db = session.db(),
            .error = error.member_ptr(),
            .transaction = txn.raw,
            .options = options.collections = &collection.raw,
            .keys = &key,
            .keys_stride = 1,
            .offsets = &value_off,
            .lengths = &value_len,
            .values = &value_ptr,
        };

        ukv_write(&write);
        if (!status)
            return send_response(make_error(req, http::status::internal_server_error, error.raw));

        http::response<http::empty_body> res;
        res.set(http::field::server, server_name_k);
        res.set(http::field::content_type, mime_binary_k);
        res.keep_alive(req.keep_alive());
        return send_response(std::move(res));
    }

        // Upsert data:
    case http::verb::delete_: {

        status_t status;
        ukv_bytes_ptr_t value_ptr = nullptr;
        ukv_length_t value_len = 0;
        ukv_length_t value_off = 0;

        ukv_write_t write {
            .db = session.db(),
            .error = error.member_ptr(),
            .transaction = txn.raw,
            .options = options.collections = &collection.raw,
            .keys = &key,
            .keys_stride = 1,
            .offsets = &value_off,
            .lengths = &value_len,
            .values = &value_ptr,
        };

        ukv_write(&write);
        if (!status)
            return send_response(make_error(req, http::status::internal_server_error, error.raw));

        http::response<http::empty_body> res;
        res.set(http::field::server, server_name_k);
        res.set(http::field::content_type, mime_binary_k);
        res.keep_alive(req.keep_alive());
        return send_response(std::move(res));
    }

    //
    default: {
        return send_response(make_error(req, http::status::bad_request, "Unsupported HTTP verb"));
    }
    }
}

template <typename body_at, typename allocator_at, typename send_response_at>
void respond_to_aos(db_session_t& session,
                    http::request<body_at, http::basic_fields<allocator_at>>&& req,
                    send_response_at&& send_response) {

    http::verb received_verb = req.method();
    beast::string_view received_path = req.target();

    transaction_t txn(session.db());
    std::vector<blobs_collection_t> collections(session.db());
    ukv_options_t options = ukv_options_default_k;
    std::vector<ukv_key_t> keys;

    // Parse the free-order parameters, starting with transaction identifier.
    auto params_begin = std::find(received_path.begin(), received_path.end(), '?');
    auto params_str = beast::string_view {params_begin, static_cast<size_t>(received_path.end() - params_begin)};
    if (auto txn_val = param_value(params_str, "txn="); txn_val) {
        auto txn_id = size_t {0};
        auto txn_parse_result = std::from_chars(txn_val->data(), txn_val->data() + txn_val->size(), txn_id);
        if (txn_parse_result.ec != std::errc())
            return send_response(make_error(req, http::status::bad_request, "Couldn't parse the transaction id"));
    }

    // Parse the collection name string.
    if (auto collection_val = param_value(params_str, "col="); collection_val) {
        char collection_name_buffer[65] = {0};
        std::memcpy(collection_name_buffer, collection_val->data(), std::min(collection_val->size(), 64ul));

        status_t status;
        blobs_collection_t collection(session.db());
        ukv_collection_create_t collection_init {
            .db = session.db(),
            .error = error.member_ptr(),
            .name = collection_name_buffer,
            .id = &collection.raw,
        };

        ukv_collection_create(&collection_init);
        if (!status)
            return send_response(make_error(req, http::status::internal_server_error, error.raw));

        collections.raw_array.emplace_back(std::exchange(collection.raw, nullptr));
        // ukv_option_read_colocated(&options, true);
    }

    // Make sure we support the requested content type
    auto payload_type = req[http::field::content_type];
    auto payload_format = mime_to_format(payload_type);
    if (payload_type != mime_json_k && payload_type != mime_msgpack_k && payload_type != mime_cbor_k &&
        payload_type != mime_bson_k && payload_type != mime_ubjson_k)
        return send_response(make_error(req,
                                        http::status::unsupported_media_type,
                                        "We only support json, msgpack, cbor, bson and ubjson MIME types"));

    // Make sure the payload is present, as it handles the heavy part of the query
    auto opt_payload_len = req.payload_size();
    if (!opt_payload_len)
        return send_response(make_error(req, http::status::length_required, "Chunk Transfer Encoding isn't supported"));

    // Parse the payload, that will contain auxiliary data
    auto payload = req.body();
    auto payload_ptr = reinterpret_cast<char const*>(payload.data());
    auto payload_len = static_cast<ukv_size_t>(*opt_payload_len);

#if 0
    // Once we know, which collection, key and transaction user is
    // interested in - perform the actions depending on verbs.
    //
    // Just write: PUT, DELETE without `fields`.
    // Every other request is dominated by a read.
    bool needs_to_parse = payload_dict.contains("fields");
    bool needs_to_reads = needs_to_parse || received_verb == http::verb::patch || received_verb == http::verb::get ||
                          received_verb == http::verb::get || received_verb == http::verb::get;
    switch (received_verb) {

        // Read the data:
    case http::verb::get: {

        // Validate and deserialize the requested keys
        auto keys_it = payload_dict.find("keys");
        if (keys_it != payload_dict.end() || !keys_it->is_array() || keys_it->empty())
            return send_response(make_error(req,
                                            http::status::bad_request,
                                            "GET request must provide a non-empty list of integer keys"));

        for (auto const& key_json : *keys_it) {
            if (!key_json.is_number_unsigned())
                return send_response(make_error(req,
                                                http::status::bad_request,
                                                "GET request must provide a non-empty list of integer keys"));
            keys.push_back(key_json.template get<ukv_key_t>());
        }

        // Pull the entire objects before we start sampling their fields
        arena_t tape(session.db());
        status_t status;
        // ukv_read(session.db(),
        //          txn.raw,
        //          keys.data(),
        //          keys.size(),
        //          collections.raw_array.data(),
        //          options,
        //          &tape.ptr,
        //          &tape.capacity,
        //          error.member_ptr());
        if (!status)
            return send_response(make_error(req, http::status::internal_server_error, error.raw));

        ukv_length_t const* values_lens = reinterpret_cast<ukv_length_t*>(tape.ptr);
        ukv_bytes_ptr_t values_begin = tape.ptr + sizeof(ukv_length_t) * keys.size();

        std::size_t exported_bytes = 0;
        std::vector<json_t> parsed_vals(keys.size());
        for (std::size_t key_idx = 0; key_idx != keys.size(); ++key_idx) {
            ukv_bytes_ptr_t begin = values_begin + exported_bytes;
            ukv_length_t len = values_lens[key_idx];
            json_t& val = parsed_vals[key_idx];
            if (!len)
                continue;

            val = json_t::from_msgpack(begin, begin + len, true, false);
            exported_bytes += len;
        }

        // Now post-process sampling only a fraction of keys
        auto fields_it = payload_dict.find("fields");
        if (fields_it != payload_dict.end()) {
            if (!fields_it->is_array() || fields_it->empty())
                return send_response(make_error(req,
                                                http::status::bad_request,
                                                "GET request must provide a non-empty list of string paths"));

            // Parse distinct paths
            std::vector<std::string> path_strs;
            std::vector<json_ptr_t> path_ptrs;
            path_strs.reserve(fields_it->size());
            path_ptrs.reserve((fields_it->size()));
            for (auto const& field_json : *fields_it) {
                if (!field_json.is_string())
                    return send_response(make_error(req,
                                                    http::status::bad_request,
                                                    "GET request must provide a non-empty list of string paths"));
                auto const& key_str = field_json.template get<std::string>();
                path_strs.emplace_back(key_str);
                path_ptrs.emplace_back(key_str);
            }

            // Export them from every document
            for (auto& val : parsed_vals)
                val = sample_fields(std::move(val), path_ptrs, path_strs);
        }

        response_dict = json_t::object({"objs", std::move(parsed_vals)});
        break;
    }

        //
    default: {
        return send_response(make_error(req, http::status::bad_request, "Unsupported HTTP verb"));
    }
    }
#endif
    // Export the response dictionary into the desired format
    std::string response_str;
    http::response<http::string_body> res {
        std::piecewise_construct,
        std::make_tuple(std::move(response_str)),
        std::make_tuple(http::status::ok, req.version()),
    };
    res.set(http::field::server, server_name_k);
    res.set(http::field::content_type, payload_type);
    res.content_length(response_str.size());
    res.keep_alive(req.keep_alive());
    return send_response(std::move(res));
}

/**
 * @brief Primary dispatch point, routing incoming HTTP requests
 *        into underlying UKV calls, preparing results and sending back.
 */
template <typename body_at, typename allocator_at, typename send_response_at>
void route_request(db_session_t& session,
                   http::request<body_at, http::basic_fields<allocator_at>>&& req,
                   send_response_at&& send_response) {

    http::verb received_verb = req.method();
    beast::string_view received_path = req.target();
    fmt::print("Received path: {} {}\n",
               std::string(beast::http::to_string(received_verb)),
               std::string(received_path));

    // Modifying single entries:
    if (received_path.starts_with("/one/"))
        return respond_to_one(session, std::move(req), send_response);

    // Modifying collections:
    else if (received_path.starts_with("/col/")) {
    }

    // Global operations:
    else if (received_path.starts_with("/all/")) {
    }

    // Supporting transactions:
    else if (received_path.starts_with("/txn/"))
        return send_response(make_error(req, http::status::bad_request, "Transactions aren't implemented yet"));

    // Array-of-Structures:
    else if (received_path.starts_with("/aos/"))
        return respond_to_aos(session, std::move(req), send_response);

    // Structure-of-Arrays:
    else if (received_path.starts_with("/soa/"))
        return send_response(make_error(req, http::status::bad_request, "Batch API aren't implemented yet"));

    // Array-of-Structures:
    else if (received_path.starts_with("/arrow/"))
        return send_response(make_error(req, http::status::bad_request, "Batch API aren't implemented yet"));

    return send_response(make_error(req, http::status::bad_request, "Unknown request"));
}

/**
 * @brief A communication channel/session for a single client.
 */
class web_db_session_t : public std::enable_shared_from_this<web_db_session_t> {
    // This is the C++11 equivalent of a generic lambda.
    // The function object is used to send an HTTP message.
    struct send_request_t {
        web_db_session_t& self_;

        explicit send_request_t(web_db_session_t& self) noexcept : self_(self) {}

        template <bool ir_request_ak, typename body_at, typename fields_at>
        void operator()(http::message<ir_request_ak, body_at, fields_at>&& msg) const {
            // The lifetime of the message has to extend
            // for the duration of the async operation so
            // we use a `std::shared_ptr` to manage it.
            auto sp = std::make_shared<http::message<ir_request_ak, body_at, fields_at>>(std::move(msg));

            // Store a type-erased version of the shared
            // pointer in the class to keep it alive.
            self_.res_ = sp;

            // Write the response
            http::async_write(
                self_.stream_,
                *sp,
                beast::bind_front_handler(&web_db_session_t::on_write, self_.shared_from_this(), sp->need_eof()));
        }
    };

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    std::shared_ptr<db_w_clients_t> db_;
    db_session_t db_session_;
    http::request<http::string_body> req_;
    std::shared_ptr<void> res_;
    send_request_t send_request_;

  public:
    web_db_session_t(tcp::socket&& socket, std::shared_ptr<db_w_clients_t> const& session)
        : stream_(std::move(socket)), db_(session), db_session_(session->session()), send_request_(*this) {}

    /**
     * @brief Start the asynchronous operation.
     */
    void run() {
        // We need to be executing within a strand to perform async operations
        // on the I/O objects in this web_db_session_t. Although not strictly necessary
        // for single-threaded contexts, this example code is written to be
        // thread-safe by default.
        net::dispatch(stream_.get_executor(),
                      beast::bind_front_handler(&web_db_session_t::do_read, shared_from_this()));
    }

    void do_read() {
        // Make the request empty before reading,
        // otherwise the operation behavior is undefined.
        req_ = {};

        // Set the timeout.
        stream_.expires_after(std::chrono::seconds(30));

        // Read a request
        http::async_read(stream_,
                         buffer_,
                         req_,
                         beast::bind_front_handler(&web_db_session_t::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if (ec == http::error::end_of_stream)
            // This means they closed the connection
            return do_close();

        if (ec)
            return log_failure(ec, "read");

        // send_at the response
        route_request(db_session_, std::move(req_), send_request_);
    }

    void on_write(bool close, beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if (ec)
            return log_failure(ec, "write");

        if (close)
            // This means we should close the connection, usually because
            // the response indicated the "Connection: close" semantic.
            return do_close();

        // We're done with the response so delete it
        res_ = nullptr;

        // Read another request
        do_read();
    }

    void do_close() {
        beast::error_code ec;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
    }
};

//------------------------------------------------------------------------------

/**
 * @brief Spins on sockets, listening for new connection requests.
 *        Once accepted, allocates and dispatches a new @c web_db_session_t.
 */
class listener_t : public std::enable_shared_from_this<listener_t> {
    net::io_context& io_context_;
    tcp::acceptor acceptor_;
    std::shared_ptr<db_w_clients_t> db_;

  public:
    listener_t(net::io_context& io_context, tcp::endpoint endpoint, std::shared_ptr<db_w_clients_t> const& session)
        : io_context_(io_context), acceptor_(net::make_strand(io_context)), db_(session) {
        connect_to(endpoint);
    }

    // Start accepting incoming connections
    void run() { do_accept(); }

  private:
    void do_accept() {
        // The new connection gets its own strand
        acceptor_.async_accept(net::make_strand(io_context_),
                               beast::bind_front_handler(&listener_t::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec, tcp::socket socket) {
        if (ec)
            // To avoid infinite loop
            return log_failure(ec, "accept");

        // Create the web_db_session_t and run it
        std::make_shared<web_db_session_t>(std::move(socket), db_)->run();
        // Accept another connection
        do_accept();
    }

    void connect_to(tcp::endpoint endpoint) {
        beast::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec)
            return log_failure(ec, "open");

        // Allow address reuse
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec)
            return log_failure(ec, "set_option");

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if (ec)
            return log_failure(ec, "bind");

        // Start listening for connections
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec)
            return log_failure(ec, "listen");
    }
};

//------------------------------------------------------------------------------

int main(int argc, char* argv[]) {

    // Check command line arguments
    if (argc < 4) {
        std::cerr << "Usage: ukv_beast_server <address> <port> <threads> <db_config_path>?\n"
                  << "Example:\n"
                  << "    ukv_beast_server 0.0.0.0 8080 1\n"
                  << "    ukv_beast_server 0.0.0.0 8080 1 ./config.json\n"
                  << "";
        return EXIT_FAILURE;
    }

    // Parse the arguments
    auto const address = net::ip::make_address(argv[1]);
    auto const port = static_cast<unsigned short>(std::atoi(argv[2]));
    auto const threads = std::max<int>(1, std::atoi(argv[3]));
    auto db_config = std::string();

    // Read the configuration file
    if (argc >= 5) {
        auto const db_config_path = std::string(argv[4]);
        if (!db_config_path.empty()) {
            std::ifstream ifs(db_config_path);
            db_config = std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        }
    }

    // Check if we can initialize the DB
    auto session = std::make_shared<db_w_clients_t>();
    status_t status;
    ukv_database_init_t database {
        .config = db_config.c_str(),
        .db = &session->raw,
        .error = error.member_ptr(),
    };

    ukv_database_init(&database);
    if (!status) {
        std::cerr << "Couldn't initialize DB: " << error.raw << std::endl;
        return EXIT_FAILURE;
    }

    // Create and launch a listening port
    auto io_context = net::io_context {threads};
    std::make_shared<listener_t>(io_context, tcp::endpoint {address, port}, session)->run();

    // Run the I/O service on the requested number of threads
    std::vector<std::thread> v;
    v.reserve(threads - 1);
    for (auto i = threads - 1; i > 0; --i)
        v.emplace_back([&io_context] { io_context.run(); });
    io_context.run();

    return EXIT_SUCCESS;
}