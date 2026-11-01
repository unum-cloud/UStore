/**
 *  @file src/vram/uploads.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The upload tower: the device verbs of every engine without device kernels, served by its host verbs.
 *
 *  A call on a VRAM executor runs the engine's host verb with its outputs in the executor's pinned bounce, then carves
 *  every output column off the call's device arena and queues one copy to the device per column, repointing the
 *  caller's outputs there, and waits once. Its inputs reach the host through @c stage_inputs before the verb runs.
 *  Cursors keep refusing devices.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <tuple>   // `std::tuple`, `std::get`
#include <utility> // `std::index_sequence`, `std::make_index_sequence`

#include "ustore/utilities/ram_buffers.hpp" // `device_blocks_t`
#include "ustore/utilities/vram_device.hpp" // `device_blocks_of`

#include "engine.hpp"       // `home_tables`, `home_executor`, `engine_of`, `database_of`
#include "lib.hpp"          // `verb_slot`
#include "vram/columns.hpp" // `upload_output_columns`

namespace unum::ustore {

/**
 *  @brief Serves a device verb of @p home_ through the host verb of the same slot, as the upload tower does.
 *
 *  The trailing @p rest are the ragged inputs the router gathered, then the view and the executor, so one adapter fills
 *  every slot. A warm call allocates nothing: the bounce and the arena keep their newest block and run.
 */
template <home_t home_, typename arguments_type_, typename... rest_types_>
status_t upload_through_host(arguments_type_ const& arguments, rest_types_&... rest) noexcept {
    constexpr std::size_t ragged_count = sizeof...(rest_types_) - 2;
    std::tuple<rest_types_&...> const tail {rest...};
    view_t const& view = std::get<ragged_count>(tail);
    home_executor<home_>& executor = std::get<ragged_count + 1>(tail);

    engine_t const& engine = engine_of(database_of(arguments));
    auto const* const verbs = engine.host.*verb_slot<arguments_type_>::template table<home_t::host_k>;
    if (!verbs) return {status_code_t::unsupported_k, "The engine keeps no collections of that modality"};
    auto const verb = verbs->*verb_slot<arguments_type_>::template verb<home_t::host_k>;
    if (!verb) return {status_code_t::unsupported_k, "The engine does not implement this call"};
    utilities::device_blocks_t* const blocks = utilities::device_blocks_of(arguments.arena, executor);
    if (!blocks) return {status_code_t::device_k, "The arena lives where another executor computes"};

    arguments_type_ hosted = arguments;
    hosted.arena = executor.bounce();
    status_t const served = [&]<std::size_t... index_>(std::index_sequence<index_...>) noexcept {
        return verb(hosted, std::get<index_>(tail)..., view);
    }(std::make_index_sequence<ragged_count> {});
    if (served.failed()) return served;
    return upload_output_columns(hosted, *blocks, executor);
}

template <home_t home_>
inline constexpr blobs_verbs<home_> uploading_vram_blobs {
    .read = &upload_through_host<home_, ustore_blobs_read_t>,
    .scan = &upload_through_host<home_, ustore_blobs_scan_t>,
    .sample = &upload_through_host<home_, ustore_blobs_sample_t>,
};

template <home_t home_>
inline constexpr strings_verbs<home_> uploading_vram_strings {
    .read = &upload_through_host<home_, ustore_strings_read_t>,
    .scan = &upload_through_host<home_, ustore_strings_scan_t>,
};

template <home_t home_>
inline constexpr docs_verbs<home_> uploading_vram_docs {
    .read = &upload_through_host<home_, ustore_docs_read_t>,
    .gather = &upload_through_host<home_, ustore_docs_gather_t>,
    .scan = &upload_through_host<home_, ustore_docs_scan_t>,
    .sample = &upload_through_host<home_, ustore_docs_sample_t>,
};

template <home_t home_>
inline constexpr graph_verbs<home_> uploading_vram_graph {
    .read_edges = &upload_through_host<home_, ustore_graph_read_edges_t>,
    .scan_vertices = &upload_through_host<home_, ustore_graph_scan_vertices_t>,
};

template <home_t home_>
inline constexpr pages_verbs<home_> uploading_vram_pages {
    .read = &upload_through_host<home_, ustore_pages_read_t>,
    .scan = &upload_through_host<home_, ustore_pages_scan_t>,
};

/** The device tables at @p home_ of an engine with no device kernels, every verb served by its host twin through the
 *  bounce. */
template <home_t home_>
inline constexpr home_tables<home_> uploading_vram_tables {
    .blobs = &uploading_vram_blobs<home_>,
    .strings = &uploading_vram_strings<home_>,
    .docs = &uploading_vram_docs<home_>,
    .graph = &uploading_vram_graph<home_>,
    .pages = &uploading_vram_pages<home_>,
};

} // namespace unum::ustore
