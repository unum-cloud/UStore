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
	var lent lent[struct{}]
	_, err := block(&lent, on.stringsWrite(&lent, C.ustore_completion_blocking_k, collections, strings, values, config))
	return err
}

// StringsWrite submits [DatabaseOn.StringsWrite].
func (on deferredWrites[Handle]) StringsWrite(collections Collections, strings Tape, values []Key, config *WriteConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.stringsWrite(&deferred.lent, C.ustore_completion_deferred_k, collections, strings, values, config))
}

func (receiver storeReceiver[Handle]) stringsWrite(lent *lent[struct{}], completion C.ustore_completion_t, collections Collections, strings Tape, values []Key, config *WriteConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	count := int(strings.Len())
	if err := shape(collections, count, len(values)); err != nil {
		return err
	}
	options := configOf(config)
	arguments, inputs := stage[C.ustore_strings_write_t](lent)
	*arguments = C.ustore_strings_write_t{
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(count),
		collections: lent.collections(inputs, collections),
		strings:     lent.strings(strings),
		values:      lent.keys(values),
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
	var lent lent[struct{}]
	_, err := block(&lent, on.stringsRemove(&lent, C.ustore_completion_blocking_k, collections, strings, config))
	return err
}

// StringsRemove submits [DatabaseOn.StringsRemove].
func (on deferredWrites[Handle]) StringsRemove(collections Collections, strings Tape, config *StringsRemoveConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.stringsRemove(&deferred.lent, C.ustore_completion_deferred_k, collections, strings, config))
}

func (receiver storeReceiver[Handle]) stringsRemove(lent *lent[struct{}], completion C.ustore_completion_t, collections Collections, strings Tape, config *StringsRemoveConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	options := configOf(config)
	count := int(strings.Len())
	if err := shape(collections, count, int(options.EndStrings.Len())); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_strings_remove_t](lent)
	*arguments = C.ustore_strings_remove_t{
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(count),
		collections: lent.collections(inputs, collections),
		strings:     lent.strings(strings),
		end_strings: lent.strings(options.EndStrings),
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
}

// Len reports how many strings the call looked up.
func (mapped Mapped) Len() uint { return mapped.count }

// Get reports the key string index maps onto, and false for a missing string.
func (mapped Mapped) Get(index uint) (Key, bool) {
	if !present(mapped.presences, index) {
		return Key{}, false
	}
	return at((*Key)(unsafe.Pointer(mapped.values)), index), true
}

// StringsRead looks up the key every string maps onto into arena.
func (on reads[Handle]) StringsRead(collections Collections, strings Tape, arena *Arena) (Mapped, error) {
	var lent lent[Mapped]
	return block(&lent, on.stringsRead(&lent, C.ustore_completion_blocking_k, collections, strings, arena))
}

// StringsRead submits [DatabaseOn.StringsRead].
func (on deferredReads[Handle]) StringsRead(collections Collections, strings Tape, arena *Arena) (*Deferred[Mapped], error) {
	deferred := new(Deferred[Mapped])
	return submitted(deferred, on.stringsRead(&deferred.lent, C.ustore_completion_deferred_k, collections, strings, arena))
}

func (receiver storeReceiver[Handle]) stringsRead(lent *lent[Mapped], completion C.ustore_completion_t, collections Collections, strings Tape, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	count := int(strings.Len())
	if err := shape(collections, count); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_strings_read_t](lent)
	lent.result.count = uint(count)
	*arguments = C.ustore_strings_read_t{
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
		completion:  completion,
		view:        receiver.view(),
		count:       C.ustore_size_t(count),
		collections: lent.collections(inputs, collections),
		strings:     lent.strings(strings),
		presences:   &lent.result.presences,
		values:      &lent.result.values,
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
}

// Len reports how many scans or samples the call ran.
func (listing Listing) Len() uint { return listing.count }

// Count reports how many strings scan task found.
func (listing Listing) Count(task uint) uint {
	return uint(at(listing.offsets, task+1) - at(listing.offsets, task))
}

// Get borrows string index of scan task from the arena; index must be below [Listing.Count].
func (listing Listing) Get(task, index uint) []byte {
	return listing.strings.sized(uint(at(listing.offsets, listing.count))).Get(uint(at(listing.offsets, task)) + index)
}

// Scores borrows the BM25 score of every string scan task found, and nothing unless the patterns
// carried weights.
func (listing Listing) Scores(task uint) []float32 {
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
	var lent lent[Listing]
	return block(&lent, on.stringsScan(&lent, C.ustore_completion_blocking_k, collections, wanted, arena, config))
}

// StringsScan submits [DatabaseOn.StringsScan].
func (on deferredReads[Handle]) StringsScan(collections Collections, wanted uint, arena *Arena, config *StringsScanConfig) (*Deferred[Listing], error) {
	deferred := new(Deferred[Listing])
	return submitted(deferred, on.stringsScan(&deferred.lent, C.ustore_completion_deferred_k, collections, wanted, arena, config))
}

func (receiver storeReceiver[Handle]) stringsScan(lent *lent[Listing], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena, config *StringsScanConfig) error {
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
	arguments, inputs := stage[C.ustore_strings_scan_t](lent)
	lent.result.count = uint(count)
	progress, state := lent.progress(options.Progress)
	*arguments = C.ustore_strings_scan_t{
		database:       database,
		executor:       executor,
		outcome:        lent.outcome(),
		completion:     completion,
		view:           receiver.view(),
		count:          C.ustore_size_t(count),
		collections:    lent.collections(inputs, collections),
		prefixes:       lent.strings(options.Prefixes),
		start_strings:  lent.strings(options.StartStrings),
		end_strings:    lent.strings(options.EndStrings),
		direction:      C.ustore_scan_direction_t(options.Direction),
		patterns:       lent.patterns(options.Patterns),
		predicate:      lent.predicate(options.Predicate),
		offsets:        &lent.result.offsets,
		strings:        &lent.result.strings.raw,
		arena:          outputs,
		wanted:         C.ustore_size_t(wanted),
		mode:           C.ustore_scan_mode_t(options.Mode),
		progress:       progress,
		progress_state: state,
	}
	if len(options.Patterns.Weights) != 0 {
		arguments.scores = &lent.result.scores
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
	var lent lent[C.ustore_strings_cursor_t]
	arguments := open[C.ustore_strings_cursor_init_t](&lent)
	*arguments = C.ustore_strings_cursor_init_t{
		cursor:       lent.result,
		database:     database,
		executor:     executor,
		outcome:      lent.outcome(),
		view:         on.view(),
		collection:   rawCollection(collection),
		prefix:       lent.text(options.Prefix),
		start_string: lent.text(options.StartString),
		end_string:   lent.text(options.EndString),
		direction:    C.ustore_scan_direction_t(options.Direction),
		patterns:     lent.patterns(options.Patterns),
		predicate:    cursor.admits(options.Predicate),
	}
	C.ustore_strings_cursor_init(arguments)
	if cursor.handle, err = block(&lent, nil); err != nil {
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
}

// Len reports how many strings the pull handed out; fewer than wanted exhausts the walk for good.
func (page StringsPage) Len() uint { return uint(page.written) }

// Strings borrows the strings of the page from the arena.
func (page StringsPage) Strings() Strings { return page.strings.sized(uint(page.written)) }

// Values borrows the key every string maps onto, and is empty after a
// [StringsCursor.NextStrings] pull.
func (page StringsPage) Values() []Key { return keysAt(page.values, 0, uint64(page.written)) }

// NextStrings pulls up to wanted strings of the walk into arena.
func (on StringsCursorOn) NextStrings(wanted uint, arena *Arena) (StringsPage, error) {
	var lent lent[StringsPage]
	arguments, err := on.next(&lent, wanted, arena)
	if err != nil {
		return StringsPage{}, err
	}
	C.ustore_strings_cursor_next(arguments)
	return block(&lent, nil)
}

// NextPairs pulls up to wanted strings of the walk with the keys they map onto into arena.
func (on StringsCursorOn) NextPairs(wanted uint, arena *Arena) (StringsPage, error) {
	var lent lent[StringsPage]
	arguments, err := on.next(&lent, wanted, arena)
	if err != nil {
		return StringsPage{}, err
	}
	arguments.values = &lent.result.values
	C.ustore_strings_cursor_next(arguments)
	return block(&lent, nil)
}

// next opens a pull and fills every argument but the optional values.
func (on StringsCursorOn) next(lent *lent[StringsPage], wanted uint, arena *Arena) (*C.ustore_strings_cursor_next_t, error) {
	cursor, database, executor, outputs, err := on.lowerInto(arena)
	if err != nil {
		return nil, err
	}
	arguments := open[C.ustore_strings_cursor_next_t](lent)
	*arguments = C.ustore_strings_cursor_next_t{
		database: database,
		executor: executor,
		outcome:  lent.outcome(),
		cursor:   cursor,
		strings:  &lent.result.strings.raw,
		written:  &lent.result.written,
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
	var lent lent[Listing]
	return block(&lent, on.stringsSample(&lent, C.ustore_completion_blocking_k, collections, wanted, arena))
}

// StringsSample submits [DatabaseOn.StringsSample].
func (on deferredReads[Handle]) StringsSample(collections Collections, wanted uint, arena *Arena) (*Deferred[Listing], error) {
	deferred := new(Deferred[Listing])
	return submitted(deferred, on.stringsSample(&deferred.lent, C.ustore_completion_deferred_k, collections, wanted, arena))
}

func (receiver storeReceiver[Handle]) stringsSample(lent *lent[Listing], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	count := tasks(collections)
	arguments, inputs := stage[C.ustore_strings_sample_t](lent)
	lent.result.count = uint(count)
	*arguments = C.ustore_strings_sample_t{
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
		completion:  completion,
		view:        receiver.view(),
		count:       C.ustore_size_t(count),
		collections: lent.collections(inputs, collections),
		offsets:     &lent.result.offsets,
		strings:     &lent.result.strings.raw,
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
	var lent lent[Measurement]
	return block(&lent, on.stringsMeasure(&lent, C.ustore_completion_blocking_k, collections, arena, config))
}

// StringsMeasure submits [DatabaseOn.StringsMeasure].
func (on deferredReads[Handle]) StringsMeasure(collections Collections, arena *Arena, config *StringsMeasureConfig) (*Deferred[Measurement], error) {
	deferred := new(Deferred[Measurement])
	return submitted(deferred, on.stringsMeasure(&deferred.lent, C.ustore_completion_deferred_k, collections, arena, config))
}

func (receiver storeReceiver[Handle]) stringsMeasure(lent *lent[Measurement], completion C.ustore_completion_t, collections Collections, arena *Arena, config *StringsMeasureConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	count := tasks(collections)
	if err := shape(collections, count, int(options.StartStrings.Len()), int(options.EndStrings.Len())); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_strings_measure_t](lent)
	lent.result.count = uint(count)
	*arguments = C.ustore_strings_measure_t{
		database:          database,
		executor:          executor,
		outcome:           lent.outcome(),
		completion:        completion,
		view:              receiver.view(),
		count:             C.ustore_size_t(count),
		collections:       lent.collections(inputs, collections),
		start_strings:     lent.strings(options.StartStrings),
		end_strings:       lent.strings(options.EndStrings),
		direction:         C.ustore_scan_direction_t(options.Direction),
		min_cardinalities: &lent.result.minCardinalities,
		max_cardinalities: &lent.result.maxCardinalities,
		min_value_bytes:   &lent.result.minValueBytes,
		max_value_bytes:   &lent.result.maxValueBytes,
		min_space_usages:  &lent.result.minSpaceUsages,
		max_space_usages:  &lent.result.maxSpaceUsages,
		arena:             outputs,
	}
	C.ustore_strings_measure(arguments)
	return nil
}

// endregion: Samples and Measures
