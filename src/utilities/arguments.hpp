/**
 *  @file src/utilities/arguments.hpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The members the argument structures of the C ABI may hold, named as concepts for the
 *      code that branches on them at compile time.
 */

#pragma once

namespace unum::ustore {

#pragma region Handles

/** Whether a call names its database itself. */
template <typename arguments_type_>
concept a_call_naming_database = requires(arguments_type_ const& arguments) { arguments.database; };

/** Whether a call pulls through a cursor an init opened, keeping the database and view it reads. */
template <typename arguments_type_>
concept a_cursor_walker = requires(arguments_type_ const& arguments) { arguments.cursor->cursor; };

/** Whether a call appends to or finishes an ingest an init opened. */
template <typename arguments_type_>
concept a_call_feeding_ingest = requires(arguments_type_ const& arguments) { arguments.ingest->database; };

/** Whether a call reads through a view either view constructor made. */
template <typename arguments_type_>
concept a_call_reading_view = requires(arguments_type_ const& arguments) { arguments.view; };

/** Whether a call names how it watches the keys it reads. */
template <typename arguments_type_>
concept a_call_naming_watch = requires(arguments_type_ const& arguments) { arguments.watch; };

/** Whether a call writes its outputs into an arena of the caller's. */
template <typename arguments_type_>
concept a_call_filling_arena = requires(arguments_type_ const& arguments) { arguments.arena; };

/** Whether a call names a collection per task. */
template <typename arguments_type_>
concept a_call_naming_collections = requires(arguments_type_ const& arguments) { arguments.collections.data; };

/** Whether a call answers the stamp of the commit it landed. */
template <typename arguments_type_>
concept a_call_stamping_commit = requires(arguments_type_ const& arguments) { arguments.commit_stamp; };

/** Whether an open names the directory its database lives in. */
template <typename arguments_type_>
concept an_open_naming_directory = requires(arguments_type_ const& arguments) { arguments.directory.length; };

/** Whether a cursor init may name a key its walk stops before. */
template <typename arguments_type_>
concept an_init_ending_at_key = requires(arguments_type_ const& arguments) { arguments.end_key; };

#pragma endregion Handles

#pragma region Matches

/** Whether a call narrows the keys it lists by substring patterns over their values. */
template <typename arguments_type_>
concept a_call_matching_patterns = requires(arguments_type_ const& arguments) { arguments.patterns; };

/** Whether a call narrows the keys it lists by a filter over their documents or pages. */
template <typename arguments_type_>
concept a_call_matching_filter = requires(arguments_type_ const& arguments) { arguments.filter; };

/** Whether a call may answer a score per key it lists. */
template <typename arguments_type_>
concept a_call_answering_scores = requires(arguments_type_ const& arguments) { arguments.scores; };

#pragma endregion Matches

#pragma region Outputs

/** Whether a call holds keys, which a pull writes as the keys it walked. */
template <typename arguments_type_>
concept a_call_holding_keys = requires(arguments_type_ const& arguments) { arguments.keys; };

/** Whether a call holds strings, which a pull writes as the strings it walked. */
template <typename arguments_type_>
concept a_call_holding_strings = requires(arguments_type_ const& arguments) { arguments.strings; };

/** Whether a call holds values in a strings layout, which a pull writes as the values it read. */
template <typename arguments_type_>
concept a_call_holding_ragged_values = requires(arguments_type_ const& arguments) { arguments.values->layout; };

/** Whether a call holds values of any kind, which a pull writes as the values it read. */
template <typename arguments_type_>
concept a_call_holding_values = requires(arguments_type_ const& arguments) { arguments.values; };

#pragma endregion Outputs

#pragma region Inputs

/** Whether a call takes a strided key per task. */
template <typename arguments_type_>
concept a_call_taking_keys = requires(arguments_type_ const& arguments) { arguments.keys.stride; };

/** Whether a call takes a strided key per task to start its range from. */
template <typename arguments_type_>
concept a_call_taking_start_keys = requires(arguments_type_ const& arguments) { arguments.start_keys.stride; };

/** Whether a call takes a strided vertex per task. */
template <typename arguments_type_>
concept a_call_taking_vertices = requires(arguments_type_ const& arguments) { arguments.vertices.stride; };

/** Whether a call takes a strided vertex role per task. */
template <typename arguments_type_>
concept a_call_taking_roles = requires(arguments_type_ const& arguments) { arguments.roles.stride; };

/** Whether a call takes strided elements, such as the keys of a set's members. */
template <typename arguments_type_>
concept a_call_taking_elements = requires(arguments_type_ const& arguments) { arguments.elements.stride; };

/** Whether a call takes a strided offset per task. */
template <typename arguments_type_>
concept a_call_taking_offsets = requires(arguments_type_ const& arguments) { arguments.offsets.stride; };

/** Whether a call takes a strided limit per task, beside the offsets its outputs start at. */
template <typename arguments_type_>
concept a_call_taking_limits = requires(arguments_type_ const& arguments) { arguments.limits.stride; };

/** Whether a call takes a string per task. */
template <typename arguments_type_>
concept a_call_taking_strings = requires(arguments_type_ const& arguments) { arguments.strings.layout; };

/** Whether a call takes a prefix per task. */
template <typename arguments_type_>
concept a_call_taking_prefixes = requires(arguments_type_ const& arguments) { arguments.prefixes.layout; };

/** Whether a call takes a string per task to start its range from. */
template <typename arguments_type_>
concept a_call_taking_start_strings = requires(arguments_type_ const& arguments) { arguments.start_strings.layout; };

/** Whether a call takes a string per task to end its range before. */
template <typename arguments_type_>
concept a_call_taking_end_strings = requires(arguments_type_ const& arguments) { arguments.end_strings.layout; };

/** Whether a call takes a field list of its own length, with an output type per field. */
template <typename arguments_type_>
concept a_call_taking_typed_fields = requires(arguments_type_ const& arguments) { arguments.fields_count; };

/** Whether a call takes a field per task. */
template <typename arguments_type_>
concept a_call_taking_fields = requires(arguments_type_ const& arguments) { arguments.fields.layout; };

#pragma endregion Inputs

} // namespace unum::ustore
