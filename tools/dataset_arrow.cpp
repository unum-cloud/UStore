/**
 *  @file tools/dataset_arrow.cpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief Imports and exports whole collections of every modality as Apache Arrow IPC streams and
 *      files through the codec of <tt>tools/arrow_ipc.hpp</tt>.
 *
 *  It spells the columns each modality has, reads batches in the byte counts each message names,
 *  and exports a collection batch by batch through <tt>ustore/arrow.h</tt>.
 */

#include <cerrno>  // `errno`, `EINTR`, `EIO`, `EINVAL`, `ENOMEM`, `ENOTSUP`
#include <cstddef> // `std::size_t`, `std::byte`, `std::ptrdiff_t`
#include <cstdint> // `std::int32_t`, `std::int64_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcmp`, `std::memcpy`

#include "ustore/types.h" // `USTORE_OS_WINDOWS_`

#if USTORE_OS_WINDOWS_
#include <fcntl.h>    // `_O_RDONLY`, `_O_BINARY`
#include <io.h>       // `_read`, `_write`, `_open`, `_close`, `_commit`
#include <sys/stat.h> // `_S_IREAD`, `_S_IWRITE`
#else
#include <fcntl.h>   // `open`
#include <sys/uio.h> // `writev`, `iovec`
#include <unistd.h>  // `read`, `close`
#endif

#include <array>       // `std::array`
#include <algorithm>   // `std::make_heap`, `std::pop_heap`, `std::push_heap`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`
#include <utility>     // `std::move`, `std::swap`, `std::exchange`
#include <variant>     // `std::variant`, `std::visit`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/shared.hpp>       // `st::assume_uninitialized`
#include <stringzilla/stringzilla.hpp> // `sz::string_t`

#include "ustore/arrow.h"    // `ustore_keys_view_from_arrow`, `ustore_strings_view_to_arrow`
#include "ustore/docs.h"     // `ustore_docs_write_t`
#include "ustore/graph.h"    // `ustore_graph_write_edges_t`
#include "ustore/ranges.hpp" // `u64_tape`, `tape_string`, `strided`
#include "ustore/types.hpp"  // `sz`, `st`

#include "ustore/utilities/blocking_files.hpp" // `mapped_file_t`
#include "ustore/utilities/ram_buffers.hpp"    // `arena_allocate`, `arena_grow`, `arena_rewind`

#include "arrow_ipc.hpp" // `ipc_read_prefix`, `ipc_write_batch`, `ipc_batch_to_arrow`
#include "dataset.h"     // `ustore_dataset_import_t`, `ustore_dataset_export_t`
#include "dataset.hpp"   // `format_of`, `failed`, `copy_path`, `staged_output_t`

namespace unum::ustore::tools {

using namespace unum::ustore::utilities;

#pragma region Arrow

/** The schema metadata key naming the modality of an exported collection. */
constexpr std::string_view modality_key_k = "ustore.modality";

/** The schema metadata key naming the collection an export came from. */
constexpr std::string_view collection_key_k = "ustore.collection";

/** The field metadata key marking the XML values of a pages collection. */
constexpr std::string_view page_encoding_key_k = "ustore.page_encoding";

/** Pairs of metadata a schema read off an input may hold across its fields and itself. */
constexpr std::size_t schema_pairs_max_k = 64;

/** Zeros that the padding after every gathered buffer points into, as long as the
 *  widest alignment pads. */
constexpr std::byte zeros_k[ipc_alignment_direct_k] {};

/** The outcome a failure of the codec reports. */
[[nodiscard]] ustore_outcome_t outcome_of(ipc_status_t status) noexcept {
    switch (status) {
    case ipc_status_t::success_k: return {};
    case ipc_status_t::truncated_k: return {ustore_status_malformed_k, "The Arrow input ends inside a message"};
    case ipc_status_t::malformed_k: return {ustore_status_malformed_k, "The Arrow input is malformed"};
    case ipc_status_t::unsupported_k:
        return {
            ustore_status_unsupported_k,
            "The Arrow input is compressed, big-endian, dictionary-encoded, a union or a map, or holds nulls or views"};
    case ipc_status_t::too_small_k:
        return {ustore_status_unsupported_k, "The Arrow schema holds too many fields or pairs of metadata"};
    }
    return {ustore_status_internal_k, "The Arrow codec answered an unknown status"};
}

/** The @c errno code a stream callback answers for @p outcome. */
[[nodiscard]] int errno_of(ustore_outcome_t const& outcome) noexcept {
    switch (outcome.status) {
    case ustore_status_out_of_memory_k: return ENOMEM;
    case ustore_status_malformed_k: return EINVAL;
    case ustore_status_unsupported_k: return ENOTSUP;
    default: return EIO;
    }
}

/** One piece of a gathered write. */
struct output_piece_t {
    std::byte const* data;
    std::size_t size;
};

#if USTORE_OS_WINDOWS_

/** Reads up to @p count bytes into @p data, answering how many, zero at the end, or a negative
 *  count on a failure. */
[[nodiscard]] std::ptrdiff_t read_some(int descriptor, std::byte* data, std::size_t count) noexcept {
    return ::_read(descriptor, data, static_cast<unsigned>(count < (1u << 30) ? count : (1u << 30)));
}

/** Writes every one of @p pieces in order, answering whether all of them landed. */
[[nodiscard]] bool write_pieces(int descriptor, std::span<output_piece_t const> pieces) noexcept {
    for (output_piece_t const& piece : pieces)
        for (std::size_t done = 0; done != piece.size;) {
            std::size_t const left = piece.size - done;
            int const written = ::_write(descriptor, piece.data + done,
                                         static_cast<unsigned>(left < (1u << 30) ? left : (1u << 30)));
            if (written <= 0) return false;
            done += static_cast<std::size_t>(written);
        }
    return true;
}

[[nodiscard]] int open_for_reading(char const* path) noexcept { return ::_open(path, _O_RDONLY | _O_BINARY); }
[[nodiscard]] int create_for_writing(char const* path) noexcept {
    return ::_open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
}
[[nodiscard]] bool flush_descriptor(int descriptor) noexcept { return ::_commit(descriptor) == 0; }
[[nodiscard]] bool close_descriptor(int descriptor) noexcept { return ::_close(descriptor) == 0; }

#else

/** Reads up to @p count bytes into @p data, answering how many, zero at the end, or a negative
 *  count on a failure. */
[[nodiscard]] std::ptrdiff_t read_some(int descriptor, std::byte* data, std::size_t count) noexcept {
    for (;;) {
        ssize_t const bytes_read = ::read(descriptor, data, count);
        if (bytes_read >= 0 || errno != EINTR) return bytes_read;
    }
}

/** Writes every one of @p pieces in order through as few @c writev calls as a short write allows,
 *  answering whether all of them landed. */
[[nodiscard]] bool write_pieces(int descriptor, std::span<output_piece_t const> pieces) noexcept {
    constexpr std::size_t vectors_max_k = 64;
    std::size_t index = 0, done = 0;
    for (;;) {
        while (index != pieces.size() && pieces[index].size == done) ++index, done = 0;
        if (index == pieces.size()) return true;
        std::array<iovec, vectors_max_k> vectors;
        std::size_t count = 0;
        for (std::size_t next = index; next != pieces.size() && count != vectors_max_k; ++next) {
            std::size_t const skip = next == index ? done : 0;
            vectors[count++] = {const_cast<std::byte*>(pieces[next].data) + skip, pieces[next].size - skip};
        }
        ssize_t const written = ::writev(descriptor, vectors.data(), static_cast<int>(count));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        for (std::size_t left = static_cast<std::size_t>(written); left != 0;) {
            std::size_t const step = fu::min_of(left, pieces[index].size - done);
            left -= step, done += step;
            if (done == pieces[index].size) ++index, done = 0;
        }
    }
}

[[nodiscard]] int open_for_reading(char const* path) noexcept { return ::open(path, O_RDONLY | O_CLOEXEC); }
[[nodiscard]] int create_for_writing(char const* path) noexcept {
    // A link at the temporary's name is refused, not followed, as the library's temporaries are
    return ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
}
[[nodiscard]] bool flush_descriptor(int descriptor) noexcept {
    return utilities::flush_native_file(descriptor).succeeded();
}
[[nodiscard]] bool close_descriptor(int descriptor) noexcept { return ::close(descriptor) == 0; }

#endif

/** A descriptor this process opened, closed on destruction, or none for a negative one. */
class owned_descriptor_t {
    int descriptor_ = -1;

  public:
    owned_descriptor_t() noexcept = default;
    explicit owned_descriptor_t(int descriptor) noexcept : descriptor_(descriptor) {}
    owned_descriptor_t(owned_descriptor_t const&) = delete;
    owned_descriptor_t& operator=(owned_descriptor_t const&) = delete;
    owned_descriptor_t& operator=(owned_descriptor_t&& other) noexcept {
        std::swap(descriptor_, other.descriptor_);
        return *this;
    }
    ~owned_descriptor_t() noexcept {
        if (descriptor_ >= 0) { [[maybe_unused]] bool const closed = close_descriptor(descriptor_); }
    }

    /** Closes the descriptor now, answering whether it closed, which the
     *  destructor would ignore. */
    [[nodiscard]] bool close() noexcept { return descriptor_ < 0 || close_descriptor(std::exchange(descriptor_, -1)); }

    [[nodiscard]] int get() const noexcept { return descriptor_; }
};

/** The bytes of an Arrow input: a whole mapped file taken in place, or a descriptor read in
 *  exactly the counts the codec names, which is how standard input, pipes and anything that does
 *  not map are read. */
class input_bytes_t {
    utilities::mapped_file_t mapped_;
    std::span<std::byte const> whole_;
    std::size_t cursor_ = 0;

    /** The descriptor read in order, viewed from standard input or from @c owned_, or
     *  negative when mapped. */
    int descriptor_ = -1;

    /** The descriptor this input opened, none for standard input and mappings. */
    owned_descriptor_t owned_;

    /** Bytes @c take handed out off the descriptor, which a progress report counts. */
    std::size_t taken_off_descriptor_ = 0;

    /** Bytes @c peek read ahead off the descriptor, which @c take hands out first. */
    std::array<std::byte, 8> peeked_ {};
    std::size_t peeked_size_ = 0;
    std::size_t peeked_taken_ = 0;

    /** Reads off the descriptor into @p data until @p count bytes or the end, answering
     *  how many landed. */
    [[nodiscard]] std::size_t read_into(std::byte* data, std::size_t count, ustore_outcome_t* outcome) noexcept {
        std::size_t filled = 0;
        for (; peeked_taken_ != peeked_size_ && filled != count; ++filled) data[filled] = peeked_[peeked_taken_++];
        while (filled != count) {
            std::ptrdiff_t const bytes_read = read_some(descriptor_, data + filled, count - filled);
            if (bytes_read < 0) {
                *outcome = {ustore_status_io_k, "Failed to read the Arrow input"};
                return filled;
            }
            if (bytes_read == 0) break;
            filled += static_cast<std::size_t>(bytes_read);
        }
        return filled;
    }

  public:
    input_bytes_t() noexcept = default;
    input_bytes_t(input_bytes_t const&) = delete;
    input_bytes_t& operator=(input_bytes_t const&) = delete;

    /** Opens @p path, mapping it whole, or reading it in order when it is @c - or does not
     *  map into bytes. */
    void open(char const* path, ustore_outcome_t* outcome) noexcept {
        if (std::string_view {path} == "-") {
            descriptor_ = 0;
            return;
        }
        // A pipe or a FIFO maps empty or not at all, so it is read in order instead
        auto [file, map_status] = utilities::mapped_file_t::map(path);
        if (map_status.succeeded() && !file.view().empty()) {
            mapped_ = std::move(file);
            std::string_view const bytes = mapped_.view();
            whole_ = {reinterpret_cast<std::byte const*>(bytes.data()), bytes.size()};
            return;
        }
        owned_ = owned_descriptor_t {open_for_reading(path)};
        descriptor_ = owned_.get();
        if (descriptor_ < 0) *outcome = {ustore_status_io_k, "Failed to open the Arrow input"};
    }

    /** Views the first @p count bytes, at most 8, without taking them, fewer only at the end. */
    [[nodiscard]] std::span<std::byte const> peek(std::size_t count, ustore_outcome_t* outcome) noexcept {
        if (descriptor_ < 0) return whole_.first(fu::min_of(count, whole_.size()));
        while (peeked_size_ < count) {
            std::ptrdiff_t const bytes_read = read_some(descriptor_, peeked_.data() + peeked_size_,
                                                        count - peeked_size_);
            if (bytes_read < 0) *outcome = {ustore_status_io_k, "Failed to read the Arrow input"};
            if (bytes_read <= 0) break;
            peeked_size_ += static_cast<std::size_t>(bytes_read);
        }
        return {peeked_.data(), peeked_size_};
    }

    /** Takes the next @p count bytes, viewing the mapping or read into @p storage, and fewer only
     *  at the end. */
    [[nodiscard]] std::span<std::byte const> take(std::size_t count, st::vector<std::byte>& storage,
                                                  ustore_outcome_t* outcome) noexcept {
        if (descriptor_ < 0) {
            std::span<std::byte const> const taken = whole_.subspan(cursor_,
                                                                    fu::min_of(count, whole_.size() - cursor_));
            cursor_ += taken.size();
            return taken;
        }
        if (st::failed(storage.resize(st::assume_uninitialized, count))) {
            *outcome = {ustore_status_out_of_memory_k, "Failed to buffer the Arrow input"};
            return {};
        }
        std::size_t const filled = read_into(storage.data(), count, outcome);
        taken_off_descriptor_ += filled;
        return {storage.data(), filled};
    }

    /** Bytes taken so far, which a progress report counts. */
    [[nodiscard]] std::size_t taken() const noexcept { return descriptor_ < 0 ? cursor_ : taken_off_descriptor_; }

    /** Bytes of a mapped input, or zero for one read in order, whose end is unknown. */
    [[nodiscard]] std::size_t size() const noexcept { return whole_.size(); }
};

/** One batch handed out of an input stream, which owns its body when it was read off a descriptor,
 *  freed by its release. */
struct ipc_input_batch_t {
    st::vector<std::byte> body;
    std::array<ipc_node_t, ipc_fields_max_k> nodes {};
    std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
    ipc_arrow_storage_t storage {};
};

/** The schema handed out of an input stream, freed by its release. */
struct ipc_input_schema_t {
    st::vector<char> text;
    ipc_arrow_storage_t storage {};
};

/** Releases a batch of an input stream: its children, then the memory it owns. */
void release_input_batch(ArrowArray* array) noexcept {
    for (std::int64_t index = 0; index != array->n_children; ++index)
        if (array->children[index]->release) array->children[index]->release(array->children[index]);
    delete static_cast<ipc_input_batch_t*>(array->private_data);
    array->release = nullptr;
}

/** Releases the schema of an input stream: its children, then the memory it owns. */
void release_input_schema(ArrowSchema* schema) noexcept {
    for (std::int64_t index = 0; index != schema->n_children; ++index)
        if (schema->children[index]->release) schema->children[index]->release(schema->children[index]);
    delete static_cast<ipc_input_schema_t*>(schema->private_data);
    schema->release = nullptr;
}

/**
 *  @brief The state of the @c ArrowArrayStream an Arrow input is handed over as, which the import
 *      call owns and which outlives every batch, so releasing the stream frees nothing.
 *
 *  The schema is read before the stream is handed over, so the modality of a new collection can
 *  come from it.
 */
struct ipc_input_stream_t {
    input_bytes_t* input = nullptr;

    /** The schema message read off a descriptor, which the schema views. */
    st::vector<std::byte> schema_metadata;

    /** The metadata of the message being read, reused across batches. */
    st::vector<std::byte> metadata;

    std::array<ipc_field_t, ipc_fields_max_k> fields {};
    std::array<ipc_key_value_t, schema_pairs_max_k> pairs {};
    ipc_schema_t schema {};

    /** Why the last callback failed. */
    ustore_outcome_t failure {};
};

/** Reads the next message into @p message, its metadata into @p storage, answering false at the end
 *  of the stream, which is its end-of-stream marker or the end of the input between messages, or on
 *  a failure in @p outcome. */
[[nodiscard]] bool next_message(input_bytes_t& input, st::vector<std::byte>& storage, ipc_message_t& message,
                                ustore_outcome_t* outcome) noexcept {
    std::array<std::byte, 8> prefix {};
    std::size_t have = 0, wanted = 4, prefix_bytes = 0;
    std::uint32_t metadata_bytes = 0;
    ipc_status_t status = ipc_status_t::truncated_k;
    while (status == ipc_status_t::truncated_k && have < wanted) {
        std::span<std::byte const> const taken = input.take(wanted - have, storage, outcome);
        if (failed(outcome)) return false;
        if (taken.empty() && have == 0) return false;
        if (taken.empty()) break;
        std::memcpy(prefix.data() + have, taken.data(), taken.size());
        have += taken.size();
        status = ipc_read_prefix({prefix.data(), have}, metadata_bytes, prefix_bytes);
        wanted = prefix_bytes;
    }
    if (status != ipc_status_t::success_k) {
        *outcome = outcome_of(status);
        return false;
    }
    if (metadata_bytes == 0) return false;
    std::span<std::byte const> const metadata = input.take(metadata_bytes, storage, outcome);
    if (failed(outcome)) return false;
    status = metadata.size() == metadata_bytes ? ipc_read_message(metadata, message) : ipc_status_t::truncated_k;
    if (status != ipc_status_t::success_k) *outcome = outcome_of(status);
    return status == ipc_status_t::success_k;
}

/** Records @p outcome as the failure of a stream callback and answers its @c errno code. */
[[nodiscard]] int stream_failure(ipc_input_stream_t& state, ustore_outcome_t outcome) noexcept {
    state.failure = outcome;
    return errno_of(outcome);
}

int input_get_schema(ArrowArrayStream* stream, ArrowSchema* schema) noexcept {
    ipc_input_stream_t& state = *static_cast<ipc_input_stream_t*>(stream->private_data);
    *schema = {};
    ipc_input_schema_t* const holder = new (std::nothrow) ipc_input_schema_t;
    if (!holder) return stream_failure(state, {ustore_status_out_of_memory_k, "Failed to allocate the schema"});
    ipc_status_t status = ipc_status_t::too_small_k;
    for (std::size_t bytes = 1024; status == ipc_status_t::too_small_k && bytes <= (1u << 26); bytes *= 2) {
        if (st::failed(holder->text.resize(bytes))) break;
        holder->storage.text = holder->text;
        status = ipc_schema_to_arrow(state.schema, holder->storage, *schema);
    }
    if (status != ipc_status_t::success_k) {
        delete holder;
        *schema = {};
        return stream_failure(state, outcome_of(status));
    }
    schema->release = &release_input_schema;
    schema->private_data = holder;
    return 0;
}

int input_get_next(ArrowArrayStream* stream, ArrowArray* array) noexcept {
    ipc_input_stream_t& state = *static_cast<ipc_input_stream_t*>(stream->private_data);
    *array = {};
    ustore_outcome_t outcome {};
    ipc_message_t message;
    if (!next_message(*state.input, state.metadata, message, &outcome))
        return failed(&outcome) ? stream_failure(state, outcome) : 0;
    if (message.kind != ipc_message_kind_t::record_batch_k)
        return stream_failure(state, {ustore_status_malformed_k, "The Arrow input holds a second schema"});

    ipc_input_batch_t* const batch = new (std::nothrow) ipc_input_batch_t;
    if (!batch) return stream_failure(state, {ustore_status_out_of_memory_k, "Failed to allocate a batch"});
    std::span<std::byte const> const body = state.input->take(static_cast<std::size_t>(message.body_bytes), batch->body,
                                                              &outcome);
    ipc_batch_t decoded;
    ipc_status_t status = ipc_read_batch(message, body, state.schema, batch->nodes, batch->buffers, decoded);
    if (status == ipc_status_t::success_k) status = ipc_validate_batch(state.schema, decoded);
    if (status == ipc_status_t::success_k) status = ipc_batch_to_arrow(state.schema, decoded, batch->storage, *array);
    if (failed(&outcome) || status != ipc_status_t::success_k) {
        delete batch;
        *array = {};
        return stream_failure(state, failed(&outcome) ? outcome : outcome_of(status));
    }
    array->release = &release_input_batch;
    array->private_data = batch;
    return 0;
}

char const* input_get_last_error(ArrowArrayStream* stream) noexcept {
    return static_cast<ipc_input_stream_t*>(stream->private_data)->failure.message;
}

void input_release(ArrowArrayStream* stream) noexcept { stream->release = nullptr; }

/** The value of the schema metadata @p key, empty when absent. */
[[nodiscard]] std::string_view metadata_value(std::span<ipc_key_value_t const> pairs, std::string_view key) noexcept {
    for (ipc_key_value_t const& pair : pairs)
        if (pair.key == key) return pair.value;
    return {};
}

/** The field named @p name, or null. */
[[nodiscard]] ipc_field_t const* field_named(ipc_schema_t const& schema, std::string_view name) noexcept {
    for (ipc_field_t const& field : schema.fields)
        if (field.name == name) return &field;
    return nullptr;
}

/** What @p schema holds: its @c ustore.modality, else what its columns spell, where documents read
 *  as blobs, or zero when neither tells. */
[[nodiscard]] ustore_modality_t modality_of(ipc_schema_t const& schema) noexcept {
    if (std::string_view const named = metadata_value(schema.metadata, modality_key_k); !named.empty()) {
        ustore_modality_t modality {};
        ustore_modality_from_name(bytes_of(named), &modality);
        return modality;
    }
    if (field_named(schema, "source") && field_named(schema, "target")) return ustore_modality_graph_k;
    ipc_field_t const* const key = field_named(schema, "key");
    ipc_field_t const* const value = field_named(schema, "value");
    if (!key || !value) return ustore_modality_t {};
    if (ipc_type_is_variable(key->type)) return ustore_modality_strings_k;
    if (metadata_value(value->metadata, page_encoding_key_k) == "xml") return ustore_modality_pages_k;
    return ustore_modality_blobs_k;
}

/** Finds the collection @p name addresses into @p id, answering its modality, or zero when no
 *  collection holds the name. */
[[nodiscard]] ustore_modality_t find_named_collection(ustore_database_t database, ustore_executor_t executor,
                                                      ustore_arena_t arena, std::string_view name,
                                                      ustore_collection_id_t& id, ustore_outcome_t* outcome) noexcept {
    std::uint64_t const offsets[2] {0, name.size()};
    ustore_octet_t const* presences = nullptr;
    ustore_collection_id_t const* ids = nullptr;
    ustore_modality_t const* modalities = nullptr;
    ustore_collection_find_t find {};
    find.database = database;
    find.executor = executor;
    find.outcome = outcome;
    find.count = 1;
    find.names = u64_tape(name.data(), offsets);
    find.presences = &presences;
    find.collections = &ids;
    find.modalities = &modalities;
    find.arena = arena;
    ustore_collection_find(&find);
    if (failed(outcome) || !(presences[0] & 1)) return ustore_modality_t {};
    id = ids[0];
    return modalities[0];
}

/** Writes an Arrow stream or file to a descriptor as batches are exported, gathering every body
 *  without a copy and never seeking, so standard output holds it too. */
class ipc_output_t {
    int descriptor_ = -1;
    ustore_dataset_format_t format_ = ustore_dataset_format_arrow_stream_k;
    ipc_options_t options_ {};
    std::uint64_t position_ = 0;
    st::vector<std::byte> metadata_;
    st::vector<ipc_block_t> blocks_;
    std::array<ipc_gather_t, ipc_buffers_max_k> gathers_ {};

    /** Writes @p pieces, advancing the position past them. */
    void write(std::span<output_piece_t const> pieces, ustore_outcome_t* outcome) noexcept {
        if (!write_pieces(descriptor_, pieces)) {
            *outcome = {ustore_status_io_k, "Failed to write the Arrow output"};
            return;
        }
        for (output_piece_t const& piece : pieces) position_ += piece.size;
    }

    /** Sizes the metadata buffer to @p bytes. */
    [[nodiscard]] bool reserve_metadata(std::size_t bytes, ustore_outcome_t* outcome) noexcept {
        if (!st::failed(metadata_.resize(st::assume_uninitialized, bytes))) return true;
        *outcome = {ustore_status_out_of_memory_k, "Failed to buffer the Arrow metadata"};
        return false;
    }

  public:
    ipc_output_t(int descriptor, ustore_dataset_format_t format, ipc_options_t options) noexcept
        : descriptor_(descriptor), format_(format), options_(options) {}

    /** Writes the file's magic, when writing a file, and the schema message. */
    void begin(ipc_schema_t const& schema, ustore_outcome_t* outcome) noexcept {
        if (format_ == ustore_dataset_format_arrow_file_k) {
            output_piece_t const magic {reinterpret_cast<std::byte const*>(ipc_file_magic_k.data()),
                                        ipc_file_magic_k.size()};
            write({&magic, 1}, outcome);
        }
        std::size_t needed = 0, written = 0;
        ipc_status_t status = ipc_write_schema(schema, options_, position_, {}, needed);
        if (failed(outcome) || !reserve_metadata(needed, outcome)) return;
        if (status == ipc_status_t::too_small_k)
            status = ipc_write_schema(schema, options_, position_, metadata_, written);
        if (status != ipc_status_t::success_k) {
            *outcome = outcome_of(status);
            return;
        }
        output_piece_t const piece {metadata_.data(), written};
        write({&piece, 1}, outcome);
    }

    /** Writes one batch: its metadata, then every buffer of its body in place, each followed
     *  by its padding. */
    void append(ipc_schema_t const& schema, ipc_batch_t const& batch, ustore_outcome_t* outcome) noexcept {
        std::size_t needed = 0, written = 0;
        std::uint64_t body_bytes = 0;
        ipc_status_t status = ipc_write_batch(schema, batch, options_, position_, {}, needed, gathers_, body_bytes);
        if (status == ipc_status_t::too_small_k && reserve_metadata(needed, outcome))
            status = ipc_write_batch(schema, batch, options_, position_, metadata_, written, gathers_, body_bytes);
        if (failed(outcome)) return;
        if (status != ipc_status_t::success_k) {
            *outcome = outcome_of(status);
            return;
        }
        std::array<output_piece_t, 1 + 2 * ipc_buffers_max_k> pieces;
        std::size_t count = 0;
        pieces[count++] = {metadata_.data(), written};
        for (std::size_t index = 0; index != batch.buffers.size(); ++index) {
            pieces[count++] = {gathers_[index].bytes.data(), gathers_[index].bytes.size()};
            pieces[count++] = {zeros_k, gathers_[index].padding};
        }
        ipc_block_t const block {static_cast<std::int64_t>(position_), static_cast<std::int32_t>(written),
                                 static_cast<std::int64_t>(body_bytes)};
        write({pieces.data(), count}, outcome);
        if (!failed(outcome) && format_ == ustore_dataset_format_arrow_file_k &&
            st::failed(blocks_.push_back(ipc_block_t {block})))
            *outcome = {ustore_status_out_of_memory_k, "Failed to record the Arrow block"};
    }

    /** Writes the end-of-stream marker and, when writing a file, the footer naming every batch. */
    void finish(ipc_schema_t const& schema, ustore_outcome_t* outcome) noexcept {
        std::array<std::byte, 8> end {};
        std::size_t written = 0;
        [[maybe_unused]] ipc_status_t const ended = ipc_write_end_of_stream(end, written);
        output_piece_t const end_piece {end.data(), written};
        write({&end_piece, 1}, outcome);
        if (failed(outcome) || format_ != ustore_dataset_format_arrow_file_k) return;
        std::span<ipc_block_t const> const blocks {blocks_};
        std::size_t needed = 0;
        ipc_status_t status = ipc_write_footer(schema, blocks, {}, needed);
        if (!reserve_metadata(needed, outcome)) return;
        if (status == ipc_status_t::too_small_k) status = ipc_write_footer(schema, blocks, metadata_, written);
        if (status != ipc_status_t::success_k) {
            *outcome = outcome_of(status);
            return;
        }
        output_piece_t const footer {metadata_.data(), written};
        write({&footer, 1}, outcome);
    }
};

/** The descriptor an export writes, created for a path and closed on destruction, or standard
 *  output for @c -. */
class output_descriptor_t {

    /** The descriptor written, viewed from standard output or from @c owned_. */
    int descriptor_ = -1;

    /** The file this output created, none for standard output. */
    owned_descriptor_t owned_;

    /** The target and the temporary a file output writes first, unused for standard output. */
    staged_output_t staged_;

  public:

    /** Creates the temporary beside @p path, or takes standard output for @c -. */
    void open(char const* path, ustore_outcome_t* outcome) noexcept {
        if (std::string_view {path} == "-") {
            descriptor_ = 1;
            return;
        }
        staged_.stage(path, outcome);
        if (failed(outcome)) return;
        owned_ = owned_descriptor_t {create_for_writing(staged_.temporary())};
        descriptor_ = owned_.get();
        if (descriptor_ < 0) *outcome = {ustore_status_io_k, "Failed to create the Arrow output"};
    }

    /** Makes a created temporary durable and closes it, reporting a failure the destructor would
     *  ignore, then lands it over the target while @p outcome holds success and removes it
     *  otherwise, so a file already at the target never holds a partial export. */
    void close(ustore_outcome_t* outcome) noexcept {
        if (owned_.get() >= 0 && !failed(outcome) && !flush_descriptor(owned_.get()))
            *outcome = {ustore_status_io_k, "Failed to flush the Arrow output"};
        if (!owned_.close() && !failed(outcome)) *outcome = {ustore_status_io_k, "Failed to close the Arrow output"};
        staged_.land(outcome);
    }

    [[nodiscard]] int get() const noexcept { return descriptor_; }
};

/** The index a @c modality_columns_t holds for a column the schema left out. */
constexpr std::size_t absent_column_k = static_cast<std::size_t>(-1);

/** Where the columns of a modality sit among the children of an Arrow schema. */
struct modality_columns_t {

    /** Keys of blobs, documents and pages, or the keys strings map onto. */
    std::size_t keys = absent_column_k;

    /** Values of blobs, documents and pages, or the strings of a strings collection. */
    std::size_t strings = absent_column_k;

    /** The sources of edges. */
    std::size_t sources = absent_column_k;

    /** The targets of edges. */
    std::size_t targets = absent_column_k;

    /** The identifiers of edges, which an input may leave out for the default edge. */
    std::size_t edges = absent_column_k;
};

/** Matches the children of @p schema to the columns of @p modality by name, refusing a missing,
 *  repeated or unknown one. */
[[nodiscard]] bool resolve_columns(ArrowSchema const& schema, ustore_modality_t modality, modality_columns_t& columns,
                                   ustore_outcome_t* outcome) noexcept {
    bool const graph = modality == ustore_modality_graph_k;
    bool const strings = modality == ustore_modality_strings_k;
    for (std::int64_t index = 0; index != schema.n_children; ++index) {
        std::string_view const name = schema.children[index]->name ? schema.children[index]->name : "";
        std::size_t* slot = nullptr;
        if (graph)
            slot = name == "source"   ? &columns.sources
                   : name == "target" ? &columns.targets
                   : name == "edge"   ? &columns.edges
                                      : nullptr;
        else if (name == "key") slot = strings ? &columns.strings : &columns.keys;
        else if (name == "value") slot = strings ? &columns.keys : &columns.strings;
        if (!slot || *slot != absent_column_k) {
            *outcome = {ustore_status_invalid_argument_k,
                        "The Arrow input holds a column its modality does not name, or names one twice"};
            return false;
        }
        *slot = static_cast<std::size_t>(index);
    }
    bool const complete = graph ? columns.sources != absent_column_k && columns.targets != absent_column_k
                                : columns.keys != absent_column_k && columns.strings != absent_column_k;
    if (!complete) {
        *outcome = {ustore_status_invalid_argument_k, "The Arrow input lacks a column its modality needs"};
        return false;
    }
    return true;
}

/** The columns of one batch as the views of the C ABI, read in place, those its modality
 *  lacks left empty. */
struct batch_views_t {
    ustore_size_t count = 0;
    ustore_keys_view_t keys {};
    ustore_strings_view_t strings {};
    ustore_keys_view_t sources {};
    ustore_keys_view_t targets {};
    ustore_keys_view_t edges {};
};

/** The outcome a converter's @p status reports for a column of an Arrow input. */
[[nodiscard]] ustore_outcome_t column_outcome(ustore_status_t status) noexcept {
    switch (status) {
    case ustore_status_unsupported_k:
        return {status, "An Arrow column is of another type, a dictionary or a view, which no write reads"};
    case ustore_status_malformed_k: return {status, "An Arrow column misses a buffer or is shorter than its batch"};
    default: return {status, "An Arrow column holds a null"};
    }
}

/** Reads the columns of @p batch, laid out as @p schema says, into @p rows in place. */
[[nodiscard]] bool rows_of(ArrowSchema const& schema, ArrowArray const& batch, modality_columns_t const& columns,
                           batch_views_t& rows, ustore_outcome_t* outcome) noexcept {
    rows = {};
    rows.count = static_cast<ustore_size_t>(batch.length);
    ustore_status_t status = batch.n_children == schema.n_children && batch.null_count <= 0 ? ustore_status_success_k
                                                                                            : ustore_status_malformed_k;
    auto const read_keys = [&](std::size_t column, ustore_keys_view_t& keys) noexcept {
        if (status != ustore_status_success_k || column == absent_column_k) return;
        ustore_size_t count = 0;
        status = ustore_keys_view_from_arrow(schema.children[column], batch.children[column], &keys, &count);
        if (status == ustore_status_success_k && count < static_cast<ustore_size_t>(batch.offset + batch.length))
            status = ustore_status_malformed_k;
        if (keys.data) keys.data += batch.offset;
    };
    read_keys(columns.keys, rows.keys);
    read_keys(columns.sources, rows.sources);
    read_keys(columns.targets, rows.targets);
    read_keys(columns.edges, rows.edges);
    if (status == ustore_status_success_k && columns.strings != absent_column_k) {
        ustore_size_t count = 0;
        status = ustore_strings_view_from_arrow(schema.children[columns.strings], batch.children[columns.strings],
                                                &rows.strings, &count);
        if (status == ustore_status_success_k && count < static_cast<ustore_size_t>(batch.offset + batch.length))
            status = ustore_status_malformed_k;
        rows.strings = tape_slice(rows.strings, static_cast<std::size_t>(batch.offset));
    }
    if (status != ustore_status_success_k) *outcome = column_outcome(status);
    return status == ustore_status_success_k;
}

/** Rows one write of a batched import holds at most, halved for a write the head refuses
 *  for its size. */
constexpr std::size_t import_chunk_rows_k = 65536;

/** Writes rows @b [first,first+count) of @p rows into @p collection through the writes
 *  of @p modality. */
void write_chunk(ustore_database_t database, ustore_executor_t executor, ustore_collection_id_t collection,
                 ustore_modality_t modality, batch_views_t const& rows, std::size_t first, std::size_t count,
                 ustore_durability_t durability, ustore_outcome_t* outcome) noexcept {
    ustore_collections_view_t const collections {&collection, 0};
    auto const slice = [&](ustore_keys_view_t keys) noexcept {
        return ustore_keys_view_t {keys.data ? keys.data + first : nullptr, keys.stride};
    };
    ustore_strings_view_t const strings = tape_slice(rows.strings, first);
    switch (modality) {
    case ustore_modality_docs_k: {
        ustore_docs_write_t write {.database = database,
                                   .executor = executor,
                                   .outcome = outcome,
                                   .count = count,
                                   .collections = collections,
                                   .keys = slice(rows.keys),
                                   .values = strings,
                                   .type = ustore_docs_field_type_json_k,
                                   .modification = ustore_docs_modify_upsert_k,
                                   .durability = durability};
        return ustore_docs_write(&write);
    }
    case ustore_modality_pages_k: {
        ustore_pages_import_t import {.database = database,
                                      .executor = executor,
                                      .outcome = outcome,
                                      .count = count,
                                      .collections = collections,
                                      .keys = slice(rows.keys),
                                      .values = strings,
                                      .encoding = ustore_page_encoding_xml_k,
                                      .durability = durability};
        return ustore_pages_import(&import);
    }
    case ustore_modality_strings_k: {
        ustore_strings_write_t write {.database = database,
                                      .executor = executor,
                                      .outcome = outcome,
                                      .count = count,
                                      .collections = collections,
                                      .strings = strings,
                                      .values = slice(rows.keys),
                                      .durability = durability};
        return ustore_strings_write(&write);
    }
    case ustore_modality_graph_k: {
        ustore_graph_write_edges_t write {.database = database,
                                          .executor = executor,
                                          .outcome = outcome,
                                          .count = count,
                                          .collections = collections,
                                          .edges = slice(rows.edges),
                                          .sources = slice(rows.sources),
                                          .targets = slice(rows.targets),
                                          .durability = durability};
        return ustore_graph_write_edges(&write);
    }
    default: {
        ustore_blobs_write_t write {.database = database,
                                    .executor = executor,
                                    .outcome = outcome,
                                    .count = count,
                                    .collections = collections,
                                    .keys = slice(rows.keys),
                                    .values = strings,
                                    .durability = durability};
        return ustore_blobs_write(&write);
    }
    }
}

/** Writes every row of @p rows in chunks, halving one the head refused for its size, each buffered
 *  but the last, which pays @p durability. */
void write_rows(ustore_database_t database, ustore_executor_t executor, ustore_collection_id_t collection,
                ustore_modality_t modality, batch_views_t const& rows, ustore_durability_t durability,
                ustore_outcome_t* outcome) noexcept {
    std::size_t chunk = import_chunk_rows_k;
    for (std::size_t first = 0; first != rows.count;) {
        std::size_t const count = fu::min_of<std::size_t>(chunk, rows.count - first);
        bool const last = first + count == rows.count;
        write_chunk(database, executor, collection, modality, rows, first, count,
                    last ? durability : ustore_durability_buffered_k, outcome);
        if (outcome->status == ustore_status_capacity_exhausted_k && count > 1) {
            *outcome = {};
            chunk = count / 2;
            continue;
        }
        if (failed(outcome)) return;
        first += count;
    }
}

/** Appends @p rows to @p ingest on @p executor, final on return or once submitted, as @p completion
 *  says. */
void append_rows(ustore_ingest_t ingest, ustore_executor_t executor, ustore_completion_t completion,
                 batch_views_t const& rows, ustore_outcome_t* outcome) noexcept {
    ustore_ingest_append_t append {.ingest = ingest,
                                   .executor = executor,
                                   .outcome = outcome,
                                   .completion = completion,
                                   .count = rows.count,
                                   .keys = rows.keys,
                                   .values = rows.strings,
                                   .sources = rows.sources,
                                   .targets = rows.targets,
                                   .edges = rows.edges};
    ustore_ingest_append(&append);
}

/** The modality of @p collection as @p view lists it, or zero with a failure in @p outcome. */
[[nodiscard]] ustore_modality_t listed_modality(ustore_database_t database, ustore_executor_t executor,
                                                ustore_arena_t arena, ustore_snapshot_t snapshot,
                                                ustore_collection_id_t collection, ustore_outcome_t* outcome) noexcept {
    ustore_collection_id_t const* collections = nullptr;
    ustore_modality_t const* modalities = nullptr;
    ustore_size_t written = 0;
    ustore_collection_list_t list {.database = database,
                                   .executor = executor,
                                   .outcome = outcome,
                                   .snapshot = snapshot,
                                   .collections = &collections,
                                   .modalities = &modalities,
                                   .written = &written,
                                   .arena = arena};
    ustore_collection_list(&list);
    for (ustore_size_t index = 0; !failed(outcome) && index != written; ++index)
        if (collections[index] == collection) return modalities[index];
    if (!failed(outcome)) *outcome = {ustore_status_not_found_k, "The collection does not exist"};
    return ustore_modality_t {};
}

/** The fields of the schema a collection of @p modality exports, their names
 *  pointing at literals. */
[[nodiscard]] std::span<ipc_field_t const> export_fields(ustore_modality_t modality) noexcept {
    static constexpr ipc_key_value_t uuid_pairs_k[] {{ipc_extension_name_key_k, ipc_extension_uuid_k},
                                                     {"ARROW:extension:metadata", ""}};
    static constexpr ipc_key_value_t xml_pairs_k[] {{page_encoding_key_k, "xml"}};
    static constexpr ipc_field_t uuid_k {.type = ipc_type_t::fixed_size_binary_k,
                                         .width = 16,
                                         .metadata = uuid_pairs_k};
    static constexpr ipc_field_t keyed_k[] {{.name = "key", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k},
                                            {.name = "value", .type = ipc_type_t::binary_k}};
    static constexpr ipc_field_t pages_k[] {{.name = "key", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k},
                                            {.name = "value", .type = ipc_type_t::binary_k, .metadata = xml_pairs_k}};
    static constexpr ipc_field_t strings_k[] {
        {.name = "key", .type = ipc_type_t::binary_k},
        {.name = "value", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k}};
    static constexpr ipc_field_t graph_k[] {
        {.name = "source", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k},
        {.name = "target", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k},
        {.name = "edge", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k}};
    switch (modality) {
    case ustore_modality_pages_k: return pages_k;
    case ustore_modality_strings_k: return strings_k;
    case ustore_modality_graph_k: return graph_k;
    default: return keyed_k;
    }
}

/** One batch of an export: the columns the exporters handed out of its arena, released together. */
struct export_batch_t {
    std::array<ArrowSchema, 3> schemas {};
    std::array<ArrowDeviceArray, 3> columns {};
    std::size_t count = 0;

    export_batch_t() noexcept = default;
    export_batch_t(export_batch_t const&) = delete;
    export_batch_t& operator=(export_batch_t const&) = delete;
    ~export_batch_t() noexcept {
        for (std::size_t index = 0; index != count; ++index) {
            if (columns[index].array.release) columns[index].array.release(&columns[index].array);
            if (schemas[index].release) schemas[index].release(&schemas[index]);
        }
    }

    /** Exports @p keys of @p rows rows as the next column. */
    void add(ustore_keys_view_t keys, ustore_size_t rows, ustore_arena_t arena, ustore_outcome_t* outcome) noexcept {
        if (failed(outcome)) return;
        ustore_status_t const status = ustore_keys_view_to_arrow(keys, rows, arena, &schemas[count], &columns[count]);
        if (status != ustore_status_success_k) *outcome = {status, "Failed to export the keys of a batch"};
        else ++count;
    }

    /** Exports @p strings of @p rows rows as the next column, as @c binary, which the stream's
     *  one schema names. */
    void add(ustore_strings_view_t strings, ustore_size_t rows, ustore_arena_t arena,
             ustore_outcome_t* outcome) noexcept {
        if (failed(outcome)) return;
        ustore_status_t const status = ustore_strings_view_to_arrow(strings, rows, ustore_strings_u32_tape_k, arena,
                                                                    &schemas[count], &columns[count]);
        if (status != ustore_status_success_k) *outcome = {status, "Failed to export the values of a batch"};
        else ++count;
    }
};

/** The cursor an export walks, whose alternative names the modality of its collection. */
using export_cursor_t = std::variant<ustore_blobs_cursor_t, ustore_docs_cursor_t, ustore_pages_cursor_t,
                                     ustore_strings_cursor_t, ustore_graph_vertices_cursor_t>;

void free_cursor(ustore_blobs_cursor_t cursor) noexcept { ustore_blobs_cursor_free(cursor); }
void free_cursor(ustore_docs_cursor_t cursor) noexcept { ustore_docs_cursor_free(cursor); }
void free_cursor(ustore_pages_cursor_t cursor) noexcept { ustore_pages_cursor_free(cursor); }
void free_cursor(ustore_strings_cursor_t cursor) noexcept { ustore_strings_cursor_free(cursor); }
void free_cursor(ustore_graph_vertices_cursor_t cursor) noexcept { ustore_graph_vertices_cursor_free(cursor); }

/** The walk an export takes over a collection of one modality, one batch at a time. */
class export_walk_t {
    ustore_database_t database_ = nullptr;
    ustore_executor_t executor_ = nullptr;
    ustore_view_t view_ = nullptr;
    ustore_collection_id_t collection_ {};
    export_cursor_t cursor_;

    /** The vertices of a graph's batch, kept while their edges are read into their own arena. */
    st::vector<ustore_key_t> vertices_;

    /** The batches of the last pull of documents not exported yet, in the arena that pull
     *  filled. */
    std::span<ustore_docs_batch_view_t const> pending_documents_;

    template <typename cursor_type_, typename init_type_>
    void open_(void (*init_)(init_type_*), ustore_outcome_t* outcome) noexcept {
        init_type_ init {};
        init.cursor = &cursor_.template emplace<cursor_type_>(), init.database = database_, init.executor = executor_;
        init.outcome = outcome;
        init.view = view_, init.collection = collection_;
        // Documents export whole, as the one JSON column of the empty field
        static constexpr std::uint32_t whole_document_offsets_k[2] = {0, 0};
        static constexpr ustore_docs_field_type_t whole_document_type_k = ustore_docs_field_type_json_k;
        if constexpr (std::is_same_v<init_type_, ustore_docs_cursor_init_t>) {
            init.fields_count = 1;
            init.fields.layout = ustore_strings_u32_tape_k;
            init.fields.data.u32_tape.data = reinterpret_cast<ustore_byte_t const*>("");
            init.fields.data.u32_tape.offsets = whole_document_offsets_k;
            init.columns_types = &whole_document_type_k;
        }
        init_(&init);
    }

    /** Hands out the next batch of the last pull of documents, pulling again once none is left. */
    [[nodiscard]] std::size_t pull_documents_(ustore_docs_cursor_t cursor, ustore_size_t wanted, ustore_arena_t arena,
                                              export_batch_t& batch, ustore_outcome_t* outcome) noexcept {
        if (pending_documents_.empty()) {
            ustore_docs_batch_view_t const* batches = nullptr;
            ustore_size_t count = 0;
            ustore_docs_cursor_next_t next {.database = database_,
                                            .executor = executor_,
                                            .outcome = outcome,
                                            .cursor = cursor,
                                            .batches = &batches,
                                            .batches_count = &count,
                                            .arena = arena,
                                            .wanted = wanted};
            ustore_docs_cursor_next(&next);
            if (failed(outcome) || !count) return 0;
            pending_documents_ = {batches, static_cast<std::size_t>(count)};
        }
        ustore_docs_batch_view_t const& documents = pending_documents_.front();
        pending_documents_ = pending_documents_.subspan(1);
        batch.add(ustore_keys_view_t {documents.keys, sizeof(ustore_key_t)}, documents.count, arena, outcome);
        batch.add(documents.columns[0].strings, documents.count, arena, outcome);
        return documents.count;
    }

    template <typename next_type_, typename cursor_type_>
    [[nodiscard]] std::size_t pull_keyed_(void (*next_)(next_type_*), cursor_type_ cursor, ustore_size_t wanted,
                                          ustore_arena_t arena, export_batch_t& batch,
                                          ustore_outcome_t* outcome) noexcept {
        ustore_key_t const* keys = nullptr;
        ustore_strings_view_t values {};
        ustore_size_t rows = 0;
        next_type_ next {};
        next.database = database_, next.executor = executor_, next.outcome = outcome, next.cursor = cursor;
        next.keys = &keys;
        next.values = &values, next.written = &rows, next.arena = arena, next.wanted = wanted;
        if constexpr (requires { next.outputs_encoding; }) next.outputs_encoding = ustore_page_encoding_xml_k;
        next_(&next);
        if (failed(outcome) || !rows) return 0;
        batch.add(ustore_keys_view_t {keys, sizeof(ustore_key_t)}, rows, arena, outcome);
        batch.add(values, rows, arena, outcome);
        return rows;
    }

    [[nodiscard]] std::size_t pull_strings_(ustore_strings_cursor_t cursor, ustore_size_t wanted, ustore_arena_t arena,
                                            export_batch_t& batch, ustore_outcome_t* outcome) noexcept {
        ustore_strings_view_t strings {};
        ustore_key_t const* values = nullptr;
        ustore_size_t rows = 0;
        ustore_strings_cursor_next_t next {.database = database_,
                                           .executor = executor_,
                                           .outcome = outcome,
                                           .cursor = cursor,
                                           .strings = &strings,
                                           .values = &values,
                                           .written = &rows,
                                           .arena = arena,
                                           .wanted = wanted};
        ustore_strings_cursor_next(&next);
        if (failed(outcome) || !rows) return 0;
        batch.add(strings, rows, arena, outcome);
        batch.add(ustore_keys_view_t {values, sizeof(ustore_key_t)}, rows, arena, outcome);
        return rows;
    }

  public:
    export_walk_t(ustore_database_t database, ustore_executor_t executor, ustore_view_t view,
                  ustore_collection_id_t collection, ustore_modality_t modality, ustore_outcome_t* outcome) noexcept
        : database_(database), executor_(executor), view_(view), collection_(collection) {
        if (failed(outcome)) return;
        switch (modality) {
        case ustore_modality_docs_k: open_<ustore_docs_cursor_t>(&ustore_docs_cursor_init, outcome); break;
        case ustore_modality_pages_k: open_<ustore_pages_cursor_t>(&ustore_pages_cursor_init, outcome); break;
        case ustore_modality_strings_k: open_<ustore_strings_cursor_t>(&ustore_strings_cursor_init, outcome); break;
        case ustore_modality_graph_k:
            open_<ustore_graph_vertices_cursor_t>(&ustore_graph_vertices_cursor_init, outcome);
            break;
        default: open_<ustore_blobs_cursor_t>(&ustore_blobs_cursor_init, outcome);
        }
    }
    export_walk_t(export_walk_t const&) = delete;
    export_walk_t& operator=(export_walk_t const&) = delete;
    ~export_walk_t() noexcept {
        std::visit([]<typename cursor_type_>(cursor_type_ cursor) noexcept { free_cursor(cursor); }, cursor_);
    }

    /** Pulls up to @p wanted rows, or the edges of up to @p wanted source vertices, into @p arena
     *  and exports them into @p batch, answering the rows, zero once the walk ends. */
    [[nodiscard]] std::size_t next(ustore_size_t wanted, ustore_arena_t arena, export_batch_t& batch,
                                   ustore_outcome_t* outcome) noexcept {
        return std::visit(
            [&]<typename cursor_type_>(cursor_type_ cursor) noexcept -> std::size_t {
                if constexpr (std::is_same_v<cursor_type_, ustore_docs_cursor_t>)
                    return pull_documents_(cursor, wanted, arena, batch, outcome);
                else if constexpr (std::is_same_v<cursor_type_, ustore_pages_cursor_t>)
                    return pull_keyed_(&ustore_pages_cursor_next, cursor, wanted, arena, batch, outcome);
                else if constexpr (std::is_same_v<cursor_type_, ustore_strings_cursor_t>)
                    return pull_strings_(cursor, wanted, arena, batch, outcome);
                else if constexpr (std::is_same_v<cursor_type_, ustore_graph_vertices_cursor_t>)
                    return next_edges_(cursor, wanted, arena, batch, outcome);
                else return pull_keyed_(&ustore_blobs_cursor_next, cursor, wanted, arena, batch, outcome);
            },
            cursor_);
    }

  private:

    /** Pulls the edges leaving the next pages of up to @p wanted vertices, skipping pages
     *  whose vertices leave none, and exports their sources, targets and identifiers out
     *  of the triples. */
    [[nodiscard]] std::size_t next_edges_(ustore_graph_vertices_cursor_t cursor, ustore_size_t wanted,
                                          ustore_arena_t arena, export_batch_t& batch,
                                          ustore_outcome_t* outcome) noexcept {
        for (;;) {
            ustore_key_t const* vertices = nullptr;
            ustore_size_t written = 0;
            ustore_graph_vertices_cursor_next_t next {.database = database_,
                                                      .executor = executor_,
                                                      .outcome = outcome,
                                                      .cursor = cursor,
                                                      .keys = &vertices,
                                                      .written = &written,
                                                      .arena = arena,
                                                      .wanted = wanted};
            ustore_graph_vertices_cursor_next(&next);
            if (failed(outcome) || !written) return 0;
            // The read of their edges rewinds the arena the vertices came in
            if (st::failed(vertices_.resize(written))) {
                *outcome = {ustore_status_out_of_memory_k, "Failed to keep the vertices of a batch"};
                return 0;
            }
            std::memcpy(vertices_.data(), vertices, written * sizeof(ustore_key_t));
            ustore_vertex_role_t const role = ustore_vertex_role_source_k;
            ustore_vertex_degree_t const* degrees = nullptr;
            ustore_length_t const* offsets = nullptr;
            ustore_key_t const* triples = nullptr;
            ustore_graph_read_edges_t read {.database = database_,
                                            .executor = executor_,
                                            .outcome = outcome,
                                            .view = view_,
                                            .count = written,
                                            .collections = {&collection_, 0},
                                            .vertices = {vertices_.data(), sizeof(ustore_key_t)},
                                            .roles = {&role, 0},
                                            .degrees_per_vertex = &degrees,
                                            .edges_offsets = &offsets,
                                            .edges_per_vertex = &triples,
                                            .arena = arena};
            ustore_graph_read_edges(&read);
            if (failed(outcome)) return 0;
            std::size_t const edges = offsets ? static_cast<std::size_t>(offsets[written]) : 0;
            if (!edges && written == wanted) continue;
            if (!edges) return 0;
            for (std::size_t column = 0; column != 3; ++column)
                batch.add(ustore_keys_view_t {triples + column, 3 * sizeof(ustore_key_t)}, edges, arena, outcome);
            return edges;
        }
    }
};

#pragma region Imports

/** One Arrow input of an import: its bytes, the stream its batches come through, its schema and
 *  where its columns sit. It neither moves nor copies, as the stream points at its state and the
 *  state at its bytes. */
struct import_input_t {
    input_bytes_t input;
    ipc_input_stream_t state;
    ArrowArrayStream stream {};
    ArrowSchema schema {};
    modality_columns_t columns;

    import_input_t() noexcept = default;
    import_input_t(import_input_t const&) = delete;
    import_input_t& operator=(import_input_t const&) = delete;
    ~import_input_t() noexcept {
        if (schema.release) schema.release(&schema);
    }

    /** Opens @p path, spelled as @p asked names or as its extension does, and reads its schema,
     *  sniffing whether standard input holds a file or a stream. */
    void open(char const* path, ustore_dataset_format_t asked, ustore_outcome_t* outcome) noexcept {
        std::string_view const spelled {path};
        ustore_dataset_format_t format = format_of(spelled, asked);
        if (format == ustore_dataset_format_ndjson_k || format == ustore_dataset_format_csv_k) {
            *outcome = {ustore_status_invalid_argument_k, "NDJSON and CSV import through ustore_docs_import"};
            return;
        }
        // Standard input names no format, so it is sniffed: a file when it starts with the magic
        bool const sniffed = format == ustore_dataset_format_automatic_k && spelled == "-";
        if (format != ustore_dataset_format_arrow_stream_k && format != ustore_dataset_format_arrow_file_k &&
            !sniffed) {
            *outcome = {ustore_status_unsupported_k, "Unsupported file format, expected .arrows, .arrow or .feather"};
            return;
        }
        input.open(path, outcome);
        if (failed(outcome)) return;
        std::string_view const magic = ipc_file_magic_k.substr(0, 6);
        if (sniffed) {
            std::span<std::byte const> const head = input.peek(magic.size(), outcome);
            if (failed(outcome)) return;
            bool const is_file = head.size() == magic.size() &&
                                 std::memcmp(head.data(), magic.data(), magic.size()) == 0;
            format = is_file ? ustore_dataset_format_arrow_file_k : ustore_dataset_format_arrow_stream_k;
        }
        state.input = &input;
        if (format == ustore_dataset_format_arrow_file_k) {
            std::span<std::byte const> const head = input.take(ipc_file_magic_k.size(), state.metadata, outcome);
            if (failed(outcome)) return;
            if (head.size() != ipc_file_magic_k.size() || std::memcmp(head.data(), magic.data(), magic.size()) != 0) {
                *outcome = {ustore_status_malformed_k, "The Arrow file does not start with its magic"};
                return;
            }
        }
        ipc_message_t message;
        if (!next_message(input, state.schema_metadata, message, outcome)) {
            if (!failed(outcome)) *outcome = {ustore_status_malformed_k, "The Arrow input holds no schema"};
            return;
        }
        if (message.kind != ipc_message_kind_t::schema_k) {
            *outcome = {ustore_status_malformed_k, "The Arrow input starts with a batch, not a schema"};
            return;
        }
        if (ipc_status_t const status = ipc_read_schema(message, state.fields, state.pairs, state.schema);
            status != ipc_status_t::success_k) {
            *outcome = outcome_of(status);
            return;
        }
        stream = {&input_get_schema, &input_get_next, &input_get_last_error, &input_release, &state};
        if (stream.get_schema(&stream, &schema) != 0) *outcome = state.failure;
    }

    /** Pulls the next batch of the input into @p batch, released by its caller, and its columns
     *  into @p rows, answering false at the end of the input or on a failure in @p outcome. */
    [[nodiscard]] bool next(ArrowArray& batch, batch_views_t& rows, ustore_outcome_t* outcome) noexcept {
        batch = {};
        if (stream.get_next(&stream, &batch) != 0) {
            *outcome = state.failure;
            return false;
        }
        return batch.release && rows_of(schema, batch, columns, rows, outcome);
    }
};

/** Hands @p consume the rows of every input of @p inputs in their order, one batch at a time. */
template <typename consumer_type_>
void read_inputs_in_turn(std::span<import_input_t> inputs, consumer_type_&& consume,
                         ustore_outcome_t* outcome) noexcept {
    for (import_input_t& input : inputs)
        for (;;) {
            ArrowArray batch {};
            batch_views_t rows;
            bool const pulled = input.next(batch, rows, outcome);
            if (pulled) consume(rows);
            if (batch.release) batch.release(&batch);
            if (!pulled || failed(outcome)) break;
        }
}

/** Rows and bytes one merged batch of a sorted import holds at most before it lands. */
constexpr std::size_t merged_batch_rows_k = 65536;
constexpr std::size_t merged_batch_bytes_k = std::size_t {64} << 20;

/** The rows a merge of sorted inputs lays out in an arena: keys, and values on a tape. */
struct merged_batch_t {
    std::span<ustore_key_t> keys;
    std::span<std::uint64_t> offsets;
    std::span<ustore_byte_t> bytes;
    std::size_t count = 0;

    /** Rewinds @p arena and carves room for a full batch's keys and offsets in it. */
    void start(ustore_arena_t arena, ustore_outcome_t* outcome) noexcept {
        arena_rewind(arena);
        auto [carved_keys, keys_status] = arena_allocate<ustore_key_t>(arena, merged_batch_rows_k);
        auto [carved_offsets, offsets_status] = arena_allocate<std::uint64_t>(arena, merged_batch_rows_k + 1);
        if (keys_status.failed() || offsets_status.failed()) {
            *outcome = {ustore_status_out_of_memory_k, "Failed to lay out a merged batch"};
            return;
        }
        keys = carved_keys, offsets = carved_offsets, bytes = {}, count = 0;
        offsets[0] = 0;
    }

    /** Appends a row of @p key and @p value, growing the tape in @p arena. */
    void append(ustore_key_t const& key, ustore_bytes_view_t value, ustore_arena_t arena,
                ustore_outcome_t* outcome) noexcept {
        std::size_t const used = static_cast<std::size_t>(offsets[count]);
        if (used + value.length > bytes.size()) {
            std::size_t const grown = fu::max_of<std::size_t>(bytes.size(), used + value.length);
            auto [larger, larger_status] = arena_grow<ustore_byte_t>(arena, bytes, grown);
            if (larger_status.failed()) {
                *outcome = {ustore_status_out_of_memory_k, "Failed to grow a merged batch"};
                return;
            }
            bytes = larger;
        }
        if (value.length) std::memcpy(bytes.data() + used, value.data, value.length);
        keys[count] = key;
        offsets[count + 1] = used + value.length;
        ++count;
    }

    [[nodiscard]] bool full() const noexcept {
        return count == merged_batch_rows_k || offsets[count] >= merged_batch_bytes_k;
    }

    /** The batch as the views the writes and the ingest take. */
    [[nodiscard]] batch_views_t rows() const noexcept {
        batch_views_t rows;
        rows.count = count;
        rows.keys = {keys.data(), sizeof(ustore_key_t)};
        rows.strings.layout = ustore_strings_u64_tape_k;
        rows.strings.data.u64_tape.data = bytes.data();
        rows.strings.data.u64_tape.offsets = offsets.data();
        return rows;
    }
};

/**
 *  @brief Hands @p consume the rows of every sorted input of @p inputs merged into one key order,
 *      in batches laid out in @p arena, a key several inputs hold landing once, from the last.
 *
 *  Keys order as their 16 bytes do, and the strings of a strings collection as their bytes do, the
 *  order a sorted ingest checks. Every input holds one batch at a time, so memory holds a batch per
 *  input and one merged batch.
 */
template <typename consumer_type_>
void merge_sorted_inputs(std::span<import_input_t> inputs, ustore_modality_t modality,
                         std::span<ustore_arena_t const, 2> arenas, consumer_type_&& consume,
                         ustore_outcome_t* outcome) noexcept {
    std::size_t const count = inputs.size();
    st::vector<ArrowArray> batches;
    st::vector<batch_views_t> rows;
    st::vector<std::size_t> positions;
    st::vector<std::size_t> repeating;
    if (st::failed(batches.resize(count)) || st::failed(rows.resize(count)) || st::failed(positions.resize(count)) ||
        st::failed(repeating.reserve(count))) {
        *outcome = {ustore_status_out_of_memory_k, "Failed to allocate the merge"};
        return;
    }
    // Moves input @p index past its exhausted batches, answering whether it holds a row
    auto const refill = [&](std::size_t index) noexcept {
        while (batches[index].release && positions[index] == rows[index].count) {
            batches[index].release(&batches[index]);
            positions[index] = 0;
            if (!inputs[index].next(batches[index], rows[index], outcome)) {
                if (batches[index].release) batches[index].release(&batches[index]);
                batches[index] = {};
            }
        }
        return batches[index].release != nullptr;
    };
    auto const order_of = [&](std::size_t index) noexcept -> std::string_view {
        std::size_t const row = positions[index];
        if (modality == ustore_modality_strings_k) return string_of(tape_string(rows[index].strings, row));
        strided_iterator<ustore_key_t const> const keys = strided(rows[index].keys);
        ustore_key_t const& key = keys[row];
        return {reinterpret_cast<char const*>(key.uuid.bytes), sizeof(ustore_key_t)};
    };
    for (std::size_t index = 0; index != count && !failed(outcome); ++index)
        if (inputs[index].next(batches[index], rows[index], outcome)) [[maybe_unused]]
            bool const held = refill(index);

    // A min-heap of the inputs holding rows, by next key and then by position, so equal keys pop in
    // input order and the last one popped is the one that lands
    auto const after = [&](std::size_t left, std::size_t right) noexcept {
        std::string_view const left_key = order_of(left), right_key = order_of(right);
        return left_key == right_key ? left > right : left_key > right_key;
    };
    st::vector<std::size_t> heap;
    if (st::failed(heap.reserve(count))) {
        *outcome = {ustore_status_out_of_memory_k, "Failed to allocate the merge"};
        return;
    }
    for (std::size_t index = 0; index != count; ++index)
        if (batches[index].release) [[maybe_unused]]
            st::status_t const pushed = heap.push_back(st::assume_reserved, std::size_t {index});
    std::make_heap(heap.begin(), heap.end(), after);

    // Batches fill the two arenas in turn, so one lands while the next one merges
    std::size_t filling = 0;
    merged_batch_t merged;
    merged.start(arenas[filling], outcome);
    while (!failed(outcome) && heap.size() != 0) {
        repeating.clear();
        do {
            std::pop_heap(heap.begin(), heap.end(), after);
            [[maybe_unused]] st::status_t const kept = repeating.push_back(st::assume_reserved,
                                                                           std::size_t {heap.back()});
            heap.pop_back();
        } while (heap.size() != 0 && order_of(heap.front()) == order_of(repeating[0]));
        std::size_t const winner = repeating.back();
        std::size_t const row = positions[winner];
        strided_iterator<ustore_key_t const> const keys = strided(rows[winner].keys);
        merged.append(keys[row], tape_string(rows[winner].strings, row), arenas[filling], outcome);
        for (std::size_t const index : repeating) {
            ++positions[index];
            if (!refill(index)) continue;
            [[maybe_unused]] st::status_t const pushed = heap.push_back(st::assume_reserved, std::size_t {index});
            std::push_heap(heap.begin(), heap.end(), after);
        }
        if (failed(outcome) || !merged.full()) continue;
        consume(merged.rows());
        filling = 1 - filling;
        if (!failed(outcome)) merged.start(arenas[filling], outcome);
    }
    if (!failed(outcome) && merged.count) consume(merged.rows());
    for (ArrowArray& batch : batches)
        if (batch.release) batch.release(&batch);
}

#pragma endregion Imports

#pragma endregion Arrow

} // namespace unum::ustore::tools

#pragma region C Interface

void ustore_dataset_import(ustore_dataset_import_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_dataset_import_t const& task = *task_pointer;
    if (task.written) *task.written = 0;

    if (!task.database) {
        *task.outcome = {ustore_status_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!task.arena) {
        *task.outcome = {ustore_status_invalid_argument_k, "The task needs an arena"};
        return;
    }
    if (task.ingestion != ustore_dataset_ingestion_batched_k && task.ingestion != ustore_dataset_ingestion_sorted_k &&
        task.ingestion != ustore_dataset_ingestion_unsorted_k) {
        *task.outcome = {ustore_status_invalid_argument_k, "Unknown ingestion"};
        return;
    }
    if (!task.paths_count) {
        *task.outcome = {ustore_status_invalid_argument_k, "The import needs an input"};
        return;
    }
    std::size_t const count = static_cast<std::size_t>(task.paths_count);
    std::unique_ptr<import_input_t[]> owned {new (std::nothrow) import_input_t[count]};
    if (!owned) {
        *task.outcome = {ustore_status_out_of_memory_k, "Failed to allocate the inputs"};
        return;
    }
    std::span<import_input_t> const inputs {owned.get(), count};
    for (std::size_t index = 0; index != count && !failed(task.outcome); ++index) {
        ustore_bytes_view_t path {};
        if (task.paths.layout == ustore_strings_accessor_k)
            task.paths.data.accessor.get(task.paths.data.accessor.handle, &index, 1, &path);
        else path = tape_string(task.paths, index);
        sz::string_t copied;
        copy_path(path, copied, task.outcome);
        if (!failed(task.outcome) && count > 1 && copied.view() == "-")
            *task.outcome = {ustore_status_invalid_argument_k, "Standard input is the only input of its import"};
        if (!failed(task.outcome)) inputs[index].open(copied.c_str(), task.format, task.outcome);
    }
    if (failed(task.outcome)) return;

    ipc_schema_t const& first = inputs[0].state.schema;
    std::string_view const name = task.collection_name.length ? string_of(task.collection_name)
                                                              : metadata_value(first.metadata, collection_key_k);
    if (name.empty()) {
        *task.outcome = {ustore_status_invalid_argument_k,
                         "Neither the call nor the schema's ustore.collection names it"};
        return;
    }
    ustore_collection_id_t collection {};
    ustore_modality_t const existing = find_named_collection(task.database, task.executor, task.arena, name, collection,
                                                             task.outcome);
    if (failed(task.outcome)) return;
    ustore_modality_t const wanted = task.modality ? task.modality : existing ? existing : modality_of(first);
    if (!wanted) {
        *task.outcome = {ustore_status_invalid_argument_k,
                         "Neither the schema's ustore.modality nor its columns name a modality, so name one"};
        return;
    }
    if (existing && existing != wanted) {
        *task.outcome = {ustore_status_invalid_argument_k, "The collection holds another modality"};
        return;
    }
    bool const merges = task.ingestion == ustore_dataset_ingestion_sorted_k && count > 1;
    if (merges && wanted == ustore_modality_graph_k) {
        *task.outcome = {ustore_status_unsupported_k, "Several graph inputs merge into no key order"};
        return;
    }
    for (import_input_t& input : inputs)
        if (!failed(task.outcome)) [[maybe_unused]]
            bool const resolved = resolve_columns(input.schema, wanted, input.columns, task.outcome);
    if (failed(task.outcome)) return;
    if (!existing) {
        ustore_collection_create_t create {};
        create.database = task.database;
        create.executor = task.executor;
        create.outcome = task.outcome;
        create.name = bytes_of(name);
        create.modality = wanted;
        create.collection = &collection;
        ustore_collection_create(&create);
        if (failed(task.outcome)) return;
    }

    // A merge lays its batches out in the task's arena and a spare one by turns
    ustore_arena_t spare = nullptr;
    if (merges) {
        ustore_arena_init_t made {.arena = &spare, .executor = task.executor, .outcome = task.outcome};
        ustore_arena_init(&made);
        if (failed(task.outcome)) return;
    }
    ustore_arena_t const arenas[2] {task.arena, spare};
    std::size_t total = 0;
    for (import_input_t const& input : inputs) total += input.input.size();
    ustore_size_t written = 0;
    auto const report = [&]() noexcept {
        std::size_t taken = 0;
        for (import_input_t const& input : inputs) taken += input.input.taken();
        [[maybe_unused]] bool const goes_on = advance(task, taken, total);
    };
    auto const land = [&](auto&& consume) noexcept {
        auto const counted = [&](batch_views_t const& rows) noexcept {
            consume(rows);
            if (failed(task.outcome)) return;
            written += rows.count;
            report();
        };
        if (merges) merge_sorted_inputs(inputs, wanted, arenas, counted, task.outcome);
        else read_inputs_in_turn(inputs, counted, task.outcome);
    };

    if (task.ingestion == ustore_dataset_ingestion_batched_k) {
        // Every batch lands buffered, and one flush makes all of them durable when the task asks
        land([&](batch_views_t const& rows) noexcept {
            write_rows(task.database, task.executor, collection, wanted, rows, ustore_durability_buffered_k,
                       task.outcome);
        });
        if (!failed(task.outcome) && task.durability == ustore_durability_flushed_k) {
            ustore_database_flush_t flush {.database = task.database,
                                           .executor = task.executor,
                                           .outcome = task.outcome};
            ustore_database_flush(&flush);
        }
    }
    else {
        ustore_ingest_t ingest = nullptr;
        ustore_ingest_init_t init {.ingest = &ingest,
                                   .database = task.database,
                                   .executor = task.executor,
                                   .outcome = task.outcome,
                                   .collection = collection,
                                   .order = task.ingestion == ustore_dataset_ingestion_sorted_k
                                                ? ustore_ingest_sorted_k
                                                : ustore_ingest_unsorted_k};
        ustore_ingest_init(&init);
        // A merged batch appends while the next merges into the other arena, so each waits for the
        // one before it; rows read in place append final, as their batch goes right after
        ustore_outcome_t appended {};
        bool appending = false;
        auto const wait_appended = [&]() noexcept {
            if (!appending) return;
            ustore_outcome_wait(&appended);
            appending = false;
            if (appended.status != ustore_status_success_k) *task.outcome = appended;
        };
        if (!failed(task.outcome))
            land([&](batch_views_t const& rows) noexcept {
                wait_appended();
                if (failed(task.outcome)) return;
                append_rows(ingest, task.executor, merges ? ustore_completion_deferred_k : ustore_completion_blocking_k,
                            rows, &appended);
                appending = true;
                if (!merges) wait_appended();
            });
        wait_appended();
        if (!failed(task.outcome)) {
            ustore_ingest_finish_t finish {.ingest = ingest,
                                           .executor = task.executor,
                                           .outcome = task.outcome,
                                           .durability = task.durability};
            ustore_ingest_finish(&finish);
        }
        ustore_ingest_free(ingest);
        // An ingest lands all of the stream or none of it, so a collection made for it goes with it
        if (failed(task.outcome) && !existing) {
            ustore_outcome_t dropped {};
            ustore_collection_drop_t drop {};
            drop.database = task.database;
            drop.executor = task.executor;
            drop.outcome = &dropped;
            drop.collection = collection;
            drop.mode = ustore_drop_collection_k;
            ustore_collection_drop(&drop);
        }
    }
    ustore_arena_free(spare);
    if (!failed(task.outcome) && task.written) *task.written = written;
}

void ustore_dataset_export(ustore_dataset_export_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_dataset_export_t const& task = *task_pointer;
    if (task.written) *task.written = 0;

    if (!task.database) {
        *task.outcome = {ustore_status_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!task.arena) {
        *task.outcome = {ustore_status_invalid_argument_k, "The task needs an arena"};
        return;
    }
    ustore_size_t const alignment = task.alignment ? task.alignment : ipc_alignment_default_k;
    if (alignment != ipc_alignment_default_k && alignment != ipc_alignment_direct_k) {
        *task.outcome = {ustore_status_invalid_argument_k, "Arrow buffers align to 64 or 4096 bytes"};
        return;
    }
    sz::string_t path;
    copy_path(task.path, path, task.outcome);
    if (failed(task.outcome)) return;
    ustore_dataset_format_t format = format_of(path.view(), task.format);
    if (format == ustore_dataset_format_automatic_k && path.view() == "-")
        format = ustore_dataset_format_arrow_stream_k;
    if (format == ustore_dataset_format_ndjson_k || format == ustore_dataset_format_csv_k) {
        *task.outcome = {ustore_status_invalid_argument_k, "NDJSON and CSV export through ustore_docs_export"};
        return;
    }
    if (format != ustore_dataset_format_arrow_stream_k && format != ustore_dataset_format_arrow_file_k) {
        *task.outcome = {ustore_status_unsupported_k, "Unsupported file format, expected .arrows, .arrow or .feather"};
        return;
    }

    // A head export reads one snapshot of its own, as a head cursor reads a removed key as
    // an empty value
    ustore_snapshot_t snapshot = nullptr;
    ustore_view_t view = task.view;
    if (!view) {
        ustore_snapshot_init_t init {.snapshot = &snapshot,
                                     .database = task.database,
                                     .executor = task.executor,
                                     .outcome = task.outcome};
        ustore_snapshot_init(&init);
        view = ustore_snapshot_view(snapshot);
    }
    ustore_modality_t const modality = failed(task.outcome) ? ustore_modality_t {}
                                                            : listed_modality(task.database, task.executor, task.arena,
                                                                              snapshot, task.collection, task.outcome);
    std::array<ipc_key_value_t, 2> metadata {
        {{modality_key_k, failed(task.outcome) ? "" : ustore_modality_name(modality)},
         {collection_key_k, string_of(task.collection_name)}}};
    ipc_schema_t const schema {export_fields(modality), std::span(metadata).first(task.collection_name.length ? 2 : 1)};

    output_descriptor_t output;
    if (!failed(task.outcome)) output.open(path.c_str(), task.outcome);
    ipc_output_t writer {output.get(), format, {static_cast<std::uint32_t>(alignment)}};
    if (!failed(task.outcome)) writer.begin(schema, task.outcome);
    ustore_size_t rows = 0;
    {
        export_walk_t walk {task.database, task.executor, view, task.collection, modality, task.outcome};
        ustore_size_t const wanted = task.batch_rows ? task.batch_rows : import_chunk_rows_k;
        while (!failed(task.outcome)) {
            export_batch_t batch;
            std::size_t const count = walk.next(wanted, task.arena, batch, task.outcome);
            if (failed(task.outcome) || !count) break;
            std::array<ArrowArray*, 3> children {};
            for (std::size_t index = 0; index != batch.count; ++index) children[index] = &batch.columns[index].array;
            std::array<void const*, 1> root_buffers {};
            ArrowArray const root {.length = static_cast<std::int64_t>(count),
                                   .n_buffers = 1,
                                   .n_children = static_cast<std::int64_t>(batch.count),
                                   .buffers = root_buffers.data(),
                                   .children = children.data(),
                                   .release = batch.columns[0].array.release};
            std::array<ipc_node_t, ipc_fields_max_k> nodes {};
            std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
            ipc_batch_t decoded {};
            if (failed(task.outcome)) break;
            if (ipc_status_t const status = ipc_batch_from_arrow(schema, root, nodes, buffers, decoded);
                status != ipc_status_t::success_k)
                *task.outcome = outcome_of(status);
            else writer.append(schema, decoded, task.outcome);
            rows += count;
            if (!failed(task.outcome)) [[maybe_unused]]
                bool const goes_on = advance(task, rows, 0);
        }
    }
    if (!failed(task.outcome)) writer.finish(schema, task.outcome);
    ustore_snapshot_free(snapshot);
    output.close(task.outcome);
    if (!failed(task.outcome) && task.written) *task.written = rows;
}

#pragma endregion C Interface
