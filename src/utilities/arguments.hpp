/**
 *  @file src/utilities/arguments.hpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The members the argument structures of the C ABI may carry, named as concepts for the
 *      code that branches on them at compile time.
 */

#pragma once

namespace unum::ustore {

#pragma region Handles

/** Whether a call names its database itself. */
template <typename arguments_type_>
concept names_database_concept = requires(arguments_type_ const& arguments) { arguments.database; };

/** Whether a call pulls through a cursor an init opened, keeping the database and view it reads. */
template <typename arguments_type_>
concept walks_cursor_concept = requires(arguments_type_ const& arguments) { arguments.cursor->cursor; };

/** Whether a call appends to or finishes an ingest an init opened. */
template <typename arguments_type_>
concept feeds_ingest_concept = requires(arguments_type_ const& arguments) { arguments.ingest->database; };

/** Whether a call reads through a view either view constructor made. */
template <typename arguments_type_>
concept reads_view_concept = requires(arguments_type_ const& arguments) { arguments.view; };

/** Whether a call names how it watches the keys it reads. */
template <typename arguments_type_>
concept names_watch_concept = requires(arguments_type_ const& arguments) { arguments.watch; };

/** Whether a call writes its outputs into an arena of the caller's. */
template <typename arguments_type_>
concept fills_arena_concept = requires(arguments_type_ const& arguments) { arguments.arena; };

/** Whether a call names a collection per task. */
template <typename arguments_type_>
concept names_collections_concept = requires(arguments_type_ const& arguments) { arguments.collections.data; };

/** Whether a call answers the stamp of the commit it landed. */
template <typename arguments_type_>
concept stamps_commit_concept = requires(arguments_type_ const& arguments) { arguments.commit_stamp; };

/** Whether an open names the directory its database lives in. */
template <typename arguments_type_>
concept names_directory_concept = requires(arguments_type_ const& arguments) { arguments.directory.length; };

/** Whether a cursor init may name a key its walk stops before. */
template <typename arguments_type_>
concept ends_at_key_concept = requires(arguments_type_ const& arguments) { arguments.end_key; };

#pragma endregion Handles

#pragma region Matches

/** Whether a call narrows the keys it lists by substring patterns over their values. */
template <typename arguments_type_>
concept matches_patterns_concept = requires(arguments_type_ const& arguments) { arguments.patterns; };

/** Whether a call narrows the keys it lists by a filter over their documents or pages. */
template <typename arguments_type_>
concept matches_filter_concept = requires(arguments_type_ const& arguments) { arguments.filter; };

/** Whether a call may answer a score per key it lists. */
template <typename arguments_type_>
concept answers_scores_concept = requires(arguments_type_ const& arguments) { arguments.scores; };

#pragma endregion Matches

#pragma region Outputs

/** Whether a call carries keys, which a pull writes as the keys it walked. */
template <typename arguments_type_>
concept carries_keys_concept = requires(arguments_type_ const& arguments) { arguments.keys; };

/** Whether a call carries strings, which a pull writes as the strings it walked. */
template <typename arguments_type_>
concept carries_strings_concept = requires(arguments_type_ const& arguments) { arguments.strings; };

/** Whether a call carries values in a strings layout, which a pull writes as the values it read. */
template <typename arguments_type_>
concept carries_ragged_values_concept = requires(arguments_type_ const& arguments) { arguments.values->layout; };

/** Whether a call carries values of any kind, which a pull writes as the values it read. */
template <typename arguments_type_>
concept carries_values_concept = requires(arguments_type_ const& arguments) { arguments.values; };

#pragma endregion Outputs

#pragma region Inputs

/** Whether a call takes a strided key per task. */
template <typename arguments_type_>
concept takes_keys_concept = requires(arguments_type_ const& arguments) { arguments.keys.stride; };

/** Whether a call takes a strided key per task to start its range from. */
template <typename arguments_type_>
concept takes_start_keys_concept = requires(arguments_type_ const& arguments) { arguments.start_keys.stride; };

/** Whether a call takes a strided vertex per task. */
template <typename arguments_type_>
concept takes_vertices_concept = requires(arguments_type_ const& arguments) { arguments.vertices.stride; };

/** Whether a call takes a strided vertex role per task. */
template <typename arguments_type_>
concept takes_roles_concept = requires(arguments_type_ const& arguments) { arguments.roles.stride; };

/** Whether a call takes strided elements, such as the keys of a set's members. */
template <typename arguments_type_>
concept takes_elements_concept = requires(arguments_type_ const& arguments) { arguments.elements.stride; };

/** Whether a call takes a strided offset per task. */
template <typename arguments_type_>
concept takes_offsets_concept = requires(arguments_type_ const& arguments) { arguments.offsets.stride; };

/** Whether a call takes a strided limit per task, beside the offsets its outputs start at. */
template <typename arguments_type_>
concept takes_limits_concept = requires(arguments_type_ const& arguments) { arguments.limits.stride; };

/** Whether a call takes a string per task. */
template <typename arguments_type_>
concept takes_strings_concept = requires(arguments_type_ const& arguments) { arguments.strings.layout; };

/** Whether a call takes a prefix per task. */
template <typename arguments_type_>
concept takes_prefixes_concept = requires(arguments_type_ const& arguments) { arguments.prefixes.layout; };

/** Whether a call takes a string per task to start its range from. */
template <typename arguments_type_>
concept takes_start_strings_concept = requires(arguments_type_ const& arguments) { arguments.start_strings.layout; };

/** Whether a call takes a string per task to end its range before. */
template <typename arguments_type_>
concept takes_end_strings_concept = requires(arguments_type_ const& arguments) { arguments.end_strings.layout; };

/** Whether a call takes a field list of its own length, with an output type per field. */
template <typename arguments_type_>
concept takes_typed_fields_concept = requires(arguments_type_ const& arguments) { arguments.fields_count; };

/** Whether a call takes a field per task. */
template <typename arguments_type_>
concept takes_fields_concept = requires(arguments_type_ const& arguments) { arguments.fields.layout; };

#pragma endregion Inputs

} // namespace unum::ustore
