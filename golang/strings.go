// Collections mapping variable-length strings onto keys of other collections, mirroring strings.h.
//
// File: golang/strings.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore

import "unsafe"

/*
#cgo noescape ustore_strings_write
#cgo nocallback ustore_strings_write
#cgo noescape ustore_strings_remove
#cgo nocallback ustore_strings_remove
#cgo noescape ustore_strings_read
#cgo nocallback ustore_strings_read
#cgo noescape ustore_strings_scan
#cgo noescape ustore_strings_cursor_init
#cgo noescape ustore_strings_cursor_next
#cgo noescape ustore_strings_sample
#cgo nocallback ustore_strings_sample
#cgo noescape ustore_strings_measure
#cgo nocallback ustore_strings_measure

#include "shim.h"
*/
import "C"

// region: Writes

// StringsWrite maps every string onto one key, its value.
func (on writes[Handle]) StringsWrite(collections Collections, strings Tape, values []Key, config *WriteConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.stringsWrite(&call, C.ustore_completion_blocking_k, collections, strings, values, config))
	return err
}

// StringsWrite submits [DatabaseOn.StringsWrite].
func (on deferredWrites[Handle]) StringsWrite(collections Collections, strings Tape, values []Key, config *WriteConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.stringsWrite(&deferred.call, C.ustore_completion_deferred_k, collections, strings, values, config))
}

func (receiver storeReceiver[Handle]) stringsWrite(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, strings Tape, values []Key, config *WriteConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	count := int(strings.Len())
	if err := shape(collections, count, len(values)); err != nil {
		return err
	}
	options := configOf(config)
	arguments, inputs := stage[C.ustore_strings_write_t](call)
	*arguments = C.ustore_strings_write_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(count),
		collections: call.collections(inputs, collections),
		strings:     call.strings(strings),
		values:      call.keys(values),
		durability:  C.ustore_durability_t(options.Durability),
		watch:       C.ustore_watch_t(receiver.watch),
	}
	C.ustore_strings_write(arguments)
	return nil
}

// StringsRemoveConfig carries the optional arguments of [DatabaseOn.StringsRemove].
type StringsRemoveConfig struct {
	// EndStrings turns every string into the inclusive start of a range ending before its end string,
	// one per task, or removes the strings alone when empty.
	EndStrings Tape
	// Durability is how durable the removals are once the call finishes at the head.
	Durability Durability
}

// StringsRemove removes strings with the keys they map onto, or every string of a range per task
// given end strings.
func (on writes[Handle]) StringsRemove(collections Collections, strings Tape, config *StringsRemoveConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.stringsRemove(&call, C.ustore_completion_blocking_k, collections, strings, config))
	return err
}

// StringsRemove submits [DatabaseOn.StringsRemove].
func (on deferredWrites[Handle]) StringsRemove(collections Collections, strings Tape, config *StringsRemoveConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.stringsRemove(&deferred.call, C.ustore_completion_deferred_k, collections, strings, config))
}

func (receiver storeReceiver[Handle]) stringsRemove(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, strings Tape, config *StringsRemoveConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	options := configOf(config)
	count := int(strings.Len())
	if err := shape(collections, count, int(options.EndStrings.Len())); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_strings_remove_t](call)
	*arguments = C.ustore_strings_remove_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(count),
		collections: call.collections(inputs, collections),
		strings:     call.strings(strings),
		end_strings: call.strings(options.EndStrings),
		durability:  C.ustore_durability_t(options.Durability),
		watch:       C.ustore_watch_t(receiver.watch),
	}
	C.ustore_strings_remove(arguments)
	return nil
}

// endregion: Writes

// region: Reads

// Mapped is the key every string a read looked up maps onto, in the order of the strings, readable
// until the arena serves another call.
type Mapped struct {
	presences *C.ustore_octet_t
	values    *C.ustore_key_t
	count     uint
	arena     arenaWrite
}

// Len reports how many strings the call looked up.
func (mapped Mapped) Len() uint { return mapped.count }

// Get reports the key string index maps onto, and false for a missing string.
func (mapped Mapped) Get(index uint) (Key, bool) {
	mapped.arena.mustOpen()
	defer mapped.arena.close()
	if !present(mapped.presences, index) {
		return Key{}, false
	}
	return at((*Key)(unsafe.Pointer(mapped.values)), index), true
}

// StringsRead looks up the key every string maps onto into arena.
func (on reads[Handle]) StringsRead(collections Collections, strings Tape, arena *Arena) (Mapped, error) {
	var call callWithResult[Mapped]
	return block(&call, on.stringsRead(&call, C.ustore_completion_blocking_k, collections, strings, arena))
}

// StringsRead submits [DatabaseOn.StringsRead].
func (on deferredReads[Handle]) StringsRead(collections Collections, strings Tape, arena *Arena) (*Deferred[Mapped], error) {
	deferred := new(Deferred[Mapped])
	return submitted(deferred, on.stringsRead(&deferred.call, C.ustore_completion_deferred_k, collections, strings, arena))
}

func (receiver storeReceiver[Handle]) stringsRead(call *callWithResult[Mapped], completion C.ustore_completion_t, collections Collections, strings Tape, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	count := int(strings.Len())
	if err := shape(collections, count); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_strings_read_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	*arguments = C.ustore_strings_read_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		view:        receiver.view(),
		count:       C.ustore_size_t(count),
		collections: call.collections(inputs, collections),
		strings:     call.strings(strings),
		presences:   &call.result.presences,
		values:      &call.result.values,
		arena:       outputs,
	}
	C.ustore_strings_read(arguments)
	return nil
}

// endregion: Reads

// region: Scans

// Listing is what a batch of string scans or samples wrote into its arena, strings scan after scan,
// readable until the arena serves another call.
type Listing struct {
	offsets *C.ustore_length_t
	strings Strings
	scores  *C.float
	count   uint
	arena   arenaWrite
}

// Len reports how many scans or samples the call ran.
func (listing Listing) Len() uint { return listing.count }

// Count reports how many strings scan task found.
func (listing Listing) Count(task uint) uint {
	listing.arena.mustOpen()
	defer listing.arena.close()
	return uint(at(listing.offsets, task+1) - at(listing.offsets, task))
}

// Get views string index of scan task in the arena; index must be below [Listing.Count].
func (listing Listing) Get(task, index uint) []byte {
	listing.arena.mustOpen()
	defer listing.arena.close()
	return listing.strings.sized(uint(at(listing.offsets, listing.count))).Get(uint(at(listing.offsets, task)) + index)
}

// Scores views the BM25 score of every string scan task found, and nothing unless the patterns
// carried weights.
func (listing Listing) Scores(task uint) []float32 {
	listing.arena.mustOpen()
	defer listing.arena.close()
	begin, end := at(listing.offsets, task), at(listing.offsets, task+1)
	if listing.scores == nil || begin == end {
		return nil
	}
	return unsafe.Slice((*float32)(unsafe.Add(unsafe.Pointer(listing.scores), uintptr(begin)*4)), end-begin)
}

// StringsScanConfig carries the optional arguments of [DatabaseOn.StringsScan].
type StringsScanConfig struct {
	// Prefixes every scan matches, one per task, or every string when empty.
	Prefixes Tape
	// StartStrings are the inclusive starts of the scans, one per task, or the first string in
	// Direction when empty.
	StartStrings Tape
	// EndStrings are the strings every scan stops before, one per task, or no bound when empty.
	EndStrings Tape
	// Direction is which way every scan goes from its start.
	Direction ScanDirection
	// Patterns keep the strings matching, shared by every scan.
	Patterns Patterns
	// Predicate admits or passes over the keys the listed strings map onto.
	Predicate Admitter
	// Mode is in what order the strings come.
	Mode ScanMode
	// Progress reports the scans finished, and may cancel the call.
	Progress ProgressCallback
}

// StringsScan lists up to wanted strings per task into arena, one task per collection collections
// names.
func (on reads[Handle]) StringsScan(collections Collections, wanted uint, arena *Arena, config *StringsScanConfig) (Listing, error) {
	var call callWithResult[Listing]
	return block(&call, on.stringsScan(&call, C.ustore_completion_blocking_k, collections, wanted, arena, config))
}

// StringsScan submits [DatabaseOn.StringsScan].
func (on deferredReads[Handle]) StringsScan(collections Collections, wanted uint, arena *Arena, config *StringsScanConfig) (*Deferred[Listing], error) {
	deferred := new(Deferred[Listing])
	return submitted(deferred, on.stringsScan(&deferred.call, C.ustore_completion_deferred_k, collections, wanted, arena, config))
}

func (receiver storeReceiver[Handle]) stringsScan(call *callWithResult[Listing], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena, config *StringsScanConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	count := tasks(collections)
	if err := shape(collections, count, int(options.Prefixes.Len()), int(options.StartStrings.Len()), int(options.EndStrings.Len())); err != nil {
		return err
	}
	if err := options.Patterns.check(); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_strings_scan_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	progress, state := call.progress(options.Progress)
	*arguments = C.ustore_strings_scan_t{
		database:       database,
		executor:       executor,
		outcome:        call.outcome(),
		completion:     completion,
		view:           receiver.view(),
		count:          C.ustore_size_t(count),
		collections:    call.collections(inputs, collections),
		prefixes:       call.strings(options.Prefixes),
		start_strings:  call.strings(options.StartStrings),
		end_strings:    call.strings(options.EndStrings),
		direction:      C.ustore_scan_direction_t(options.Direction),
		patterns:       call.patterns(options.Patterns),
		predicate:      call.predicate(options.Predicate),
		offsets:        &call.result.offsets,
		strings:        &call.result.strings.raw,
		arena:          outputs,
		wanted:         C.ustore_size_t(wanted),
		mode:           C.ustore_scan_mode_t(options.Mode),
		progress:       progress,
		progress_state: state,
	}
	if len(options.Patterns.Weights) != 0 {
		arguments.scores = &call.result.scores
	}
	C.ustore_strings_scan(arguments)
	return nil
}

// endregion: Scans

// region: Cursors

// StringsCursorConfig carries the optional arguments of [DatabaseOn.StringsCursorInit].
type StringsCursorConfig struct {
	// Prefix is what every string the walk returns starts with, or nothing to match every string.
	Prefix string
	// StartString is where the walk starts, inclusive, or the first string of the prefix in Direction
	// when empty.
	StartString string
	// EndString is the string the walk stops before, or no bound when empty.
	EndString string
	// Direction is which way the walk goes from its start.
	Direction ScanDirection
	// Patterns keep the strings matching, compiled once.
	Patterns Patterns
	// Predicate admits or passes over the keys of every page.
	Predicate Admitter
}

// StringsCursor walks the strings of one collection page by page, in the order of their text,
// every pull resuming past the last string the pull before it handed out.
type StringsCursor struct {
	StringsCursorOn
	walk
	handle C.ustore_strings_cursor_t
}

// StringsCursorOn is the receiver whose pulls block until their call is final: on the calling
// goroutine when embedded in a [StringsCursor], or on an executor that [StringsCursorOn.On]
// picks.
type StringsCursorOn struct {
	childReceiver[C.ustore_strings_cursor_t, C.ustore_database_t]
}

// On is the receiver whose pulls block on executor.
func (on StringsCursorOn) On(executor *Executor) StringsCursorOn {
	on.executor = executor
	return on
}

// StringsCursorInit opens a walk over collection, copying its prefix and bounds, reading nothing
// until its first pull and blocking whatever receiver it is called on.
func (on reads[Handle]) StringsCursorInit(collection CollectionID, config *StringsCursorConfig) (*StringsCursor, error) {
	database, executor, err := on.lower()
	if err != nil {
		return nil, err
	}
	options := configOf(config)
	if err := options.Patterns.check(); err != nil {
		return nil, err
	}
	cursor := new(StringsCursor)
	var call callWithResult[C.ustore_strings_cursor_t]
	arguments := open[C.ustore_strings_cursor_init_t](&call)
	*arguments = C.ustore_strings_cursor_init_t{
		cursor:       call.result,
		database:     database,
		executor:     executor,
		outcome:      call.outcome(),
		view:         on.view(),
		collection:   rawCollection(collection),
		prefix:       call.text(options.Prefix),
		start_string: call.text(options.StartString),
		end_string:   call.text(options.EndString),
		direction:    C.ustore_scan_direction_t(options.Direction),
		patterns:     call.patterns(options.Patterns),
		predicate:    cursor.admits(options.Predicate),
	}
	C.ustore_strings_cursor_init(arguments)
	if cursor.handle, err = block(&call, nil); err != nil {
		cursor.end()
		return nil, err
	}
	cursor.database = database
	cursor.childReceiver = childReceiver[C.ustore_strings_cursor_t, C.ustore_database_t]{receiver: receiver[C.ustore_strings_cursor_t]{handle: &cursor.handle}, parent: &cursor.database}
	return cursor, nil
}

// StringsPage is one pull of a strings walk, written into its arena and readable until the arena
// serves another call.
type StringsPage struct {
	strings Strings
	values  *C.ustore_key_t
	written C.ustore_size_t
	arena   arenaWrite
}

// Len reports how many strings the pull handed out; fewer than wanted exhausts the walk for good.
func (page StringsPage) Len() uint { return uint(page.written) }

// Strings views the strings of the page in the arena.
func (page StringsPage) Strings() Strings {
	page.arena.mustOpen()
	defer page.arena.close()
	return page.strings.sized(uint(page.written))
}

// Values views the key every string maps onto, and is empty after a
// [StringsCursor.NextStrings] pull.
func (page StringsPage) Values() []Key {
	page.arena.mustOpen()
	defer page.arena.close()
	return keysAt(page.values, 0, uint64(page.written))
}

// NextStrings pulls up to wanted strings of the walk into arena.
func (on StringsCursorOn) NextStrings(wanted uint, arena *Arena) (StringsPage, error) {
	var call callWithResult[StringsPage]
	arguments, err := on.next(&call, wanted, arena)
	if err != nil {
		return StringsPage{}, err
	}
	C.ustore_strings_cursor_next(arguments)
	return block(&call, nil)
}

// NextPairs pulls up to wanted strings of the walk with the keys they map onto into arena.
func (on StringsCursorOn) NextPairs(wanted uint, arena *Arena) (StringsPage, error) {
	var call callWithResult[StringsPage]
	arguments, err := on.next(&call, wanted, arena)
	if err != nil {
		return StringsPage{}, err
	}
	arguments.values = &call.result.values
	C.ustore_strings_cursor_next(arguments)
	return block(&call, nil)
}

// next opens a pull and fills every argument but the optional values.
func (on StringsCursorOn) next(call *callWithResult[StringsPage], wanted uint, arena *Arena) (*C.ustore_strings_cursor_next_t, error) {
	cursor, database, executor, outputs, err := on.lowerInto(arena)
	if err != nil {
		return nil, err
	}
	arguments := open[C.ustore_strings_cursor_next_t](call)
	call.result.arena = call.stamp(arena, outputs)
	*arguments = C.ustore_strings_cursor_next_t{
		database: database,
		executor: executor,
		outcome:  call.outcome(),
		cursor:   cursor,
		strings:  &call.result.strings.raw,
		written:  &call.result.written,
		arena:    outputs,
		wanted:   C.ustore_size_t(wanted),
	}
	return arguments, nil
}

// Close frees the walk, giving back the holds it kept. It is idempotent.
func (cursor *StringsCursor) Close() error {
	if cursor == nil || cursor.handle == nil {
		return nil
	}
	C.ustore_strings_cursor_free(cursor.handle)
	cursor.handle = nil
	cursor.end()
	return nil
}

// endregion: Cursors

// region: Samples and Measures

// StringsSample draws up to wanted strings uniformly at random into arena, one task per collection
// collections names.
func (on reads[Handle]) StringsSample(collections Collections, wanted uint, arena *Arena) (Listing, error) {
	var call callWithResult[Listing]
	return block(&call, on.stringsSample(&call, C.ustore_completion_blocking_k, collections, wanted, arena))
}

// StringsSample submits [DatabaseOn.StringsSample].
func (on deferredReads[Handle]) StringsSample(collections Collections, wanted uint, arena *Arena) (*Deferred[Listing], error) {
	deferred := new(Deferred[Listing])
	return submitted(deferred, on.stringsSample(&deferred.call, C.ustore_completion_deferred_k, collections, wanted, arena))
}

func (receiver storeReceiver[Handle]) stringsSample(call *callWithResult[Listing], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	count := tasks(collections)
	arguments, inputs := stage[C.ustore_strings_sample_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	*arguments = C.ustore_strings_sample_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		view:        receiver.view(),
		count:       C.ustore_size_t(count),
		collections: call.collections(inputs, collections),
		offsets:     &call.result.offsets,
		strings:     &call.result.strings.raw,
		arena:       outputs,
		wanted:      C.ustore_size_t(wanted),
	}
	C.ustore_strings_sample(arguments)
	return nil
}

// StringsMeasureConfig carries the optional arguments of [DatabaseOn.StringsMeasure].
type StringsMeasureConfig struct {
	// StartStrings are the inclusive starts of the ranges, one per task, or the first string in
	// Direction when empty.
	StartStrings Tape
	// EndStrings are the strings every range stops before, one per task, or no bound when empty.
	EndStrings Tape
	// Direction is which way every range goes from its start.
	Direction ScanDirection
}

// StringsMeasure estimates the entries and the space inside a range of strings per task into arena,
// one task per collection collections names.
func (on reads[Handle]) StringsMeasure(collections Collections, arena *Arena, config *StringsMeasureConfig) (Measurement, error) {
	var call callWithResult[Measurement]
	return block(&call, on.stringsMeasure(&call, C.ustore_completion_blocking_k, collections, arena, config))
}

// StringsMeasure submits [DatabaseOn.StringsMeasure].
func (on deferredReads[Handle]) StringsMeasure(collections Collections, arena *Arena, config *StringsMeasureConfig) (*Deferred[Measurement], error) {
	deferred := new(Deferred[Measurement])
	return submitted(deferred, on.stringsMeasure(&deferred.call, C.ustore_completion_deferred_k, collections, arena, config))
}

func (receiver storeReceiver[Handle]) stringsMeasure(call *callWithResult[Measurement], completion C.ustore_completion_t, collections Collections, arena *Arena, config *StringsMeasureConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	count := tasks(collections)
	if err := shape(collections, count, int(options.StartStrings.Len()), int(options.EndStrings.Len())); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_strings_measure_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	*arguments = C.ustore_strings_measure_t{
		database:          database,
		executor:          executor,
		outcome:           call.outcome(),
		completion:        completion,
		view:              receiver.view(),
		count:             C.ustore_size_t(count),
		collections:       call.collections(inputs, collections),
		start_strings:     call.strings(options.StartStrings),
		end_strings:       call.strings(options.EndStrings),
		direction:         C.ustore_scan_direction_t(options.Direction),
		min_cardinalities: &call.result.minCardinalities,
		max_cardinalities: &call.result.maxCardinalities,
		min_value_bytes:   &call.result.minValueBytes,
		max_value_bytes:   &call.result.maxValueBytes,
		min_space_usages:  &call.result.minSpaceUsages,
		max_space_usages:  &call.result.maxSpaceUsages,
		arena:             outputs,
	}
	C.ustore_strings_measure(arguments)
	return nil
}

// endregion: Samples and Measures
