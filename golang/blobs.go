// Binary value collections, mirroring blobs.h: writes, reads, scans, walks, samples and measures.
//
// File: golang/blobs.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore

import "unsafe"

/*
#cgo noescape ustore_blobs_write
#cgo nocallback ustore_blobs_write
#cgo noescape ustore_blobs_remove
#cgo nocallback ustore_blobs_remove
#cgo noescape ustore_blobs_read
#cgo nocallback ustore_blobs_read
#cgo noescape ustore_blobs_scan
#cgo noescape ustore_blobs_cursor_init
#cgo noescape ustore_blobs_cursor_next
#cgo noescape ustore_blobs_sample
#cgo nocallback ustore_blobs_sample
#cgo noescape ustore_blobs_measure
#cgo nocallback ustore_blobs_measure

#include "shim.h"
*/
import "C"

// region: Shared Shapes

// WriteConfig carries the optional arguments of a write that takes nothing else.
type WriteConfig struct {
	// Durability is how durable the writes are once the call finishes at the head.
	Durability Durability
}

// RemoveConfig carries the optional arguments of a removal by key or by range.
type RemoveConfig struct {
	// EndKeys turns every key into the inclusive start of a range ending before its end key, one per
	// task, or removes the keys alone when empty.
	EndKeys []Key
	// Durability is how durable the removals are once the call finishes at the head.
	Durability Durability
}

// Values is a batch of values a read wrote into its arena, in the order of its keys, readable until
// the arena serves another call.
type Values struct {
	presences *C.ustore_octet_t
	values    Strings
	count     uint
	arena     arenaWrite
}

// Len reports how many values the call read.
func (values Values) Len() uint { return values.count }

// Get views value index in the arena, and false for a missing key.
func (values Values) Get(index uint) ([]byte, bool) {
	values.arena.mustOpen()
	defer values.arena.close()
	if !present(values.presences, index) {
		return nil, false
	}
	return values.values.sized(values.count).Get(index), true
}

// Tape passes every value back as an input, a missing key as an empty value, so a read's tape is
// written again as it is.
func (read Values) Tape() Tape {
	read.arena.mustOpen()
	defer read.arena.close()
	return read.values.sized(read.count).Tape()
}

// Scanned is what a batch of scans or samples wrote into its arena, keys scan after scan, readable
// until the arena serves another call.
type Scanned struct {
	offsets *C.ustore_length_t
	keys    *C.ustore_key_t
	scores  *C.float
	count   uint
	arena   arenaWrite
}

// Len reports how many scans or samples the call ran.
func (scanned Scanned) Len() uint { return scanned.count }

// Keys views the keys scan task found in the arena.
func (scanned Scanned) Keys(task uint) []Key {
	scanned.arena.mustOpen()
	defer scanned.arena.close()
	return keysAt(scanned.keys, uint64(at(scanned.offsets, task)), uint64(at(scanned.offsets, task+1)))
}

// Scores views the BM25 score of every key scan task found, and nothing unless the patterns
// carried weights.
func (scanned Scanned) Scores(task uint) []float32 {
	scanned.arena.mustOpen()
	defer scanned.arena.close()
	begin, end := at(scanned.offsets, task), at(scanned.offsets, task+1)
	if scanned.scores == nil || begin == end {
		return nil
	}
	return unsafe.Slice((*float32)(unsafe.Add(unsafe.Pointer(scanned.scores), uintptr(begin)*4)), end-begin)
}

// MeasureConfig carries the optional arguments of a measure.
type MeasureConfig struct {
	// StartKeys are the inclusive starts of the ranges, one per task, or the first key in Direction
	// when empty.
	StartKeys []Key
	// EndKeys are the keys every range stops before, one per task, or no bound when empty.
	EndKeys []Key
	// Direction is which way every range goes from its start.
	Direction ScanDirection
}

// Measurement is the bounds a measure estimated, one per range, readable until the arena serves
// another call.
type Measurement struct {
	minCardinalities *C.ustore_size_t
	maxCardinalities *C.ustore_size_t
	minValueBytes    *C.ustore_size_t
	maxValueBytes    *C.ustore_size_t
	minSpaceUsages   *C.ustore_size_t
	maxSpaceUsages   *C.ustore_size_t
	count            uint
	arena            arenaWrite
}

// Measured is the bounds on the entries and the space one range holds.
type Measured struct {
	MinCardinality, MaxCardinality uint64
	MinValueBytes, MaxValueBytes   uint64
	MinSpaceUsage, MaxSpaceUsage   uint64
}

// Len reports how many ranges the call measured.
func (measurement Measurement) Len() uint { return measurement.count }

// Get reports the bounds of range index.
func (measurement Measurement) Get(index uint) Measured {
	measurement.arena.mustOpen()
	defer measurement.arena.close()
	return Measured{
		MinCardinality: uint64(at(measurement.minCardinalities, index)),
		MaxCardinality: uint64(at(measurement.maxCardinalities, index)),
		MinValueBytes:  uint64(at(measurement.minValueBytes, index)),
		MaxValueBytes:  uint64(at(measurement.maxValueBytes, index)),
		MinSpaceUsage:  uint64(at(measurement.minSpaceUsages, index)),
		MaxSpaceUsage:  uint64(at(measurement.maxSpaceUsages, index)),
	}
}

// endregion: Shared Shapes

// region: Writes

// BlobsWrite upserts one value per key, an empty value still being a value.
func (on writes[Handle]) BlobsWrite(collections Collections, keys []Key, values Tape, config *WriteConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.blobsWrite(&call, C.ustore_completion_blocking_k, collections, keys, values, config))
	return err
}

// BlobsWrite submits [DatabaseOn.BlobsWrite].
func (on deferredWrites[Handle]) BlobsWrite(collections Collections, keys []Key, values Tape, config *WriteConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.blobsWrite(&deferred.call, C.ustore_completion_deferred_k, collections, keys, values, config))
}

func (receiver storeReceiver[Handle]) blobsWrite(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, keys []Key, values Tape, config *WriteConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	if err := shape(collections, len(keys), int(values.Len())); err != nil {
		return err
	}
	options := configOf(config)
	arguments, inputs := stage[C.ustore_blobs_write_t](call)
	*arguments = C.ustore_blobs_write_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(len(keys)),
		collections: call.collections(inputs, collections),
		keys:        call.keys(keys),
		values:      call.strings(values),
		durability:  C.ustore_durability_t(options.Durability),
		watch:       C.ustore_watch_t(receiver.watch),
	}
	C.ustore_blobs_write(arguments)
	return nil
}

// BlobsRemove removes keys with their values, or every key of a range per task given end keys.
func (on writes[Handle]) BlobsRemove(collections Collections, keys []Key, config *RemoveConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.blobsRemove(&call, C.ustore_completion_blocking_k, collections, keys, config))
	return err
}

// BlobsRemove submits [DatabaseOn.BlobsRemove].
func (on deferredWrites[Handle]) BlobsRemove(collections Collections, keys []Key, config *RemoveConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.blobsRemove(&deferred.call, C.ustore_completion_deferred_k, collections, keys, config))
}

func (receiver storeReceiver[Handle]) blobsRemove(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, keys []Key, config *RemoveConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	options := configOf(config)
	if err := shape(collections, len(keys), len(options.EndKeys)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_blobs_remove_t](call)
	*arguments = C.ustore_blobs_remove_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(len(keys)),
		collections: call.collections(inputs, collections),
		keys:        call.keys(keys),
		end_keys:    call.keys(options.EndKeys),
		durability:  C.ustore_durability_t(options.Durability),
		watch:       C.ustore_watch_t(receiver.watch),
	}
	C.ustore_blobs_remove(arguments)
	return nil
}

// endregion: Writes

// region: Reads

// BlobsReadConfig carries the optional arguments of [DatabaseOn.BlobsRead].
type BlobsReadConfig struct {
	// Offsets are the byte offsets into every value the read starts at, one per key, or whole values
	// when empty.
	Offsets []uint64
	// Limits cap the bytes read of every value from its offset, one per key, or the rest when empty.
	Limits []uint64
}

// BlobsRead reads one value per key into arena, a missing key reading as absent.
func (on reads[Handle]) BlobsRead(collections Collections, keys []Key, arena *Arena, config *BlobsReadConfig) (Values, error) {
	var call callWithResult[Values]
	return block(&call, on.blobsRead(&call, C.ustore_completion_blocking_k, collections, keys, arena, config))
}

// BlobsRead submits [DatabaseOn.BlobsRead].
func (on deferredReads[Handle]) BlobsRead(collections Collections, keys []Key, arena *Arena, config *BlobsReadConfig) (*Deferred[Values], error) {
	deferred := new(Deferred[Values])
	return submitted(deferred, on.blobsRead(&deferred.call, C.ustore_completion_deferred_k, collections, keys, arena, config))
}

func (receiver storeReceiver[Handle]) blobsRead(call *callWithResult[Values], completion C.ustore_completion_t, collections Collections, keys []Key, arena *Arena, config *BlobsReadConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	if err := shape(collections, len(keys), len(options.Offsets), len(options.Limits)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_blobs_read_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(len(keys))
	*arguments = C.ustore_blobs_read_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		view:        receiver.view(),
		count:       C.ustore_size_t(len(keys)),
		collections: call.collections(inputs, collections),
		keys:        call.keys(keys),
		offsets:     call.lengths(options.Offsets),
		limits:      call.lengths(options.Limits),
		presences:   &call.result.presences,
		values:      &call.result.values.raw,
		arena:       outputs,
	}
	C.ustore_blobs_read(arguments)
	return nil
}

// endregion: Reads

// region: Scans

// BlobsScanConfig carries the optional arguments of [DatabaseOn.BlobsScan].
type BlobsScanConfig struct {
	// StartKeys are the inclusive starts of the scans, one per task, or the first key in Direction
	// when empty.
	StartKeys []Key
	// EndKeys are the keys every scan stops before, one per task, or no bound when empty.
	EndKeys []Key
	// Direction is which way every scan goes from its start.
	Direction ScanDirection
	// Patterns keep the keys whose values match, shared by every scan.
	Patterns Patterns
	// Predicate admits or passes over the listed keys.
	Predicate Admitter
	// Mode is in what order the keys come.
	Mode ScanMode
	// Progress reports the scans finished, and may cancel the call.
	Progress ProgressCallback
}

// BlobsScan lists up to wanted keys of a range per task into arena, one task per collection
// collections names.
func (on reads[Handle]) BlobsScan(collections Collections, wanted uint, arena *Arena, config *BlobsScanConfig) (Scanned, error) {
	var call callWithResult[Scanned]
	return block(&call, on.blobsScan(&call, C.ustore_completion_blocking_k, collections, wanted, arena, config))
}

// BlobsScan submits [DatabaseOn.BlobsScan].
func (on deferredReads[Handle]) BlobsScan(collections Collections, wanted uint, arena *Arena, config *BlobsScanConfig) (*Deferred[Scanned], error) {
	deferred := new(Deferred[Scanned])
	return submitted(deferred, on.blobsScan(&deferred.call, C.ustore_completion_deferred_k, collections, wanted, arena, config))
}

func (receiver storeReceiver[Handle]) blobsScan(call *callWithResult[Scanned], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena, config *BlobsScanConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	count := tasks(collections)
	if err := shape(collections, count, len(options.StartKeys), len(options.EndKeys)); err != nil {
		return err
	}
	if err := options.Patterns.check(); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_blobs_scan_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	progress, state := call.progress(options.Progress)
	*arguments = C.ustore_blobs_scan_t{
		database:       database,
		executor:       executor,
		outcome:        call.outcome(),
		completion:     completion,
		view:           receiver.view(),
		count:          C.ustore_size_t(count),
		collections:    call.collections(inputs, collections),
		start_keys:     call.keys(options.StartKeys),
		end_keys:       call.keys(options.EndKeys),
		direction:      C.ustore_scan_direction_t(options.Direction),
		patterns:       call.patterns(options.Patterns),
		predicate:      call.predicate(options.Predicate),
		offsets:        &call.result.offsets,
		keys:           &call.result.keys,
		arena:          outputs,
		wanted:         C.ustore_size_t(wanted),
		mode:           C.ustore_scan_mode_t(options.Mode),
		progress:       progress,
		progress_state: state,
	}
	if len(options.Patterns.Weights) != 0 {
		arguments.scores = &call.result.scores
	}
	C.ustore_blobs_scan(arguments)
	return nil
}

// endregion: Scans

// region: Cursors

// BlobsCursorConfig carries the optional arguments of [DatabaseOn.BlobsCursorInit].
type BlobsCursorConfig struct {
	// EndKey is the key the walk stops before, or nil for no bound.
	EndKey *Key
	// Direction is which way the walk goes from its start.
	Direction ScanDirection
	// Patterns keep the keys whose values match, compiled once.
	Patterns Patterns
	// Predicate admits or passes over the keys of every page.
	Predicate Admitter
}

// BlobsCursor walks the keys of one collection page by page, every pull resuming past the last key
// the pull before it handed out. It holds its database open, and the transaction or snapshot it
// reads through, until [BlobsCursor.Close].
type BlobsCursor struct {
	BlobsCursorOn
	walk
	handle C.ustore_blobs_cursor_t
}

// BlobsCursorOn is the receiver whose pulls block until their call is final: on the calling
// goroutine when embedded in a [BlobsCursor], and on an executor when [BlobsCursorOn.On] makes it.
type BlobsCursorOn struct {
	childReceiver[C.ustore_blobs_cursor_t, C.ustore_database_t]
}

// On is the receiver whose pulls block on executor.
func (on BlobsCursorOn) On(executor *Executor) BlobsCursorOn {
	on.executor = executor
	return on
}

// BlobsCursorInit opens a walk over collection from startKey, reading nothing until its first pull
// and blocking whatever receiver it is called on; every pull names its own executor through On.
func (on reads[Handle]) BlobsCursorInit(collection CollectionID, startKey Key, config *BlobsCursorConfig) (*BlobsCursor, error) {
	database, executor, err := on.lower()
	if err != nil {
		return nil, err
	}
	options := configOf(config)
	if err := options.Patterns.check(); err != nil {
		return nil, err
	}
	cursor := new(BlobsCursor)
	var call callWithResult[C.ustore_blobs_cursor_t]
	arguments, inputs := stage[C.ustore_blobs_cursor_init_t](&call)
	*arguments = C.ustore_blobs_cursor_init_t{
		cursor:     call.result,
		database:   database,
		executor:   executor,
		outcome:    call.outcome(),
		view:       on.view(),
		collection: rawCollection(collection),
		start_key:  raw(startKey),
		end_key:    call.key(inputs, options.EndKey),
		direction:  C.ustore_scan_direction_t(options.Direction),
		patterns:   call.patterns(options.Patterns),
		predicate:  cursor.admits(options.Predicate),
	}
	C.ustore_blobs_cursor_init(arguments)
	if cursor.handle, err = block(&call, nil); err != nil {
		cursor.end()
		return nil, err
	}
	cursor.database = database
	cursor.childReceiver = childReceiver[C.ustore_blobs_cursor_t, C.ustore_database_t]{receiver: receiver[C.ustore_blobs_cursor_t]{handle: &cursor.handle}, parent: &cursor.database}
	return cursor, nil
}

// NextKeys pulls up to wanted keys of the walk into arena.
func (on BlobsCursorOn) NextKeys(wanted uint, arena *Arena) (Page, error) {
	var call callWithResult[Page]
	arguments, err := on.next(&call, wanted, arena)
	if err != nil {
		return Page{}, err
	}
	C.ustore_blobs_cursor_next(arguments)
	return block(&call, nil)
}

// NextPairs pulls up to wanted keys of the walk with their values into arena, in one call.
func (on BlobsCursorOn) NextPairs(wanted uint, arena *Arena) (Page, error) {
	var call callWithResult[Page]
	arguments, err := on.next(&call, wanted, arena)
	if err != nil {
		return Page{}, err
	}
	arguments.values = &call.result.values.raw
	C.ustore_blobs_cursor_next(arguments)
	return block(&call, nil)
}

// next opens a pull and fills every argument but the optional values.
func (on BlobsCursorOn) next(call *callWithResult[Page], wanted uint, arena *Arena) (*C.ustore_blobs_cursor_next_t, error) {
	cursor, database, executor, outputs, err := on.lowerInto(arena)
	if err != nil {
		return nil, err
	}
	arguments := open[C.ustore_blobs_cursor_next_t](call)
	call.result.arena = call.stamp(arena, outputs)
	*arguments = C.ustore_blobs_cursor_next_t{
		database: database,
		executor: executor,
		outcome:  call.outcome(),
		cursor:   cursor,
		keys:     &call.result.keys,
		written:  &call.result.written,
		arena:    outputs,
		wanted:   C.ustore_size_t(wanted),
	}
	return arguments, nil
}

// Close frees the walk, giving back the holds it kept. It is idempotent.
func (cursor *BlobsCursor) Close() error {
	if cursor == nil || cursor.handle == nil {
		return nil
	}
	C.ustore_blobs_cursor_free(cursor.handle)
	cursor.handle = nil
	cursor.end()
	return nil
}

// endregion: Cursors

// region: Samples and Measures

// BlobsSample draws up to wanted keys uniformly at random into arena, one task per collection
// collections names.
func (on reads[Handle]) BlobsSample(collections Collections, wanted uint, arena *Arena) (Scanned, error) {
	var call callWithResult[Scanned]
	return block(&call, on.blobsSample(&call, C.ustore_completion_blocking_k, collections, wanted, arena))
}

// BlobsSample submits [DatabaseOn.BlobsSample].
func (on deferredReads[Handle]) BlobsSample(collections Collections, wanted uint, arena *Arena) (*Deferred[Scanned], error) {
	deferred := new(Deferred[Scanned])
	return submitted(deferred, on.blobsSample(&deferred.call, C.ustore_completion_deferred_k, collections, wanted, arena))
}

func (receiver storeReceiver[Handle]) blobsSample(call *callWithResult[Scanned], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	count := tasks(collections)
	arguments, inputs := stage[C.ustore_blobs_sample_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	*arguments = C.ustore_blobs_sample_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		view:        receiver.view(),
		count:       C.ustore_size_t(count),
		collections: call.collections(inputs, collections),
		offsets:     &call.result.offsets,
		keys:        &call.result.keys,
		arena:       outputs,
		wanted:      C.ustore_size_t(wanted),
	}
	C.ustore_blobs_sample(arguments)
	return nil
}

// BlobsMeasure estimates the entries and the space inside a range per task into arena, one task per
// collection collections names, without walking them.
func (on reads[Handle]) BlobsMeasure(collections Collections, arena *Arena, config *MeasureConfig) (Measurement, error) {
	var call callWithResult[Measurement]
	return block(&call, on.blobsMeasure(&call, C.ustore_completion_blocking_k, collections, arena, config))
}

// BlobsMeasure submits [DatabaseOn.BlobsMeasure].
func (on deferredReads[Handle]) BlobsMeasure(collections Collections, arena *Arena, config *MeasureConfig) (*Deferred[Measurement], error) {
	deferred := new(Deferred[Measurement])
	return submitted(deferred, on.blobsMeasure(&deferred.call, C.ustore_completion_deferred_k, collections, arena, config))
}

func (receiver storeReceiver[Handle]) blobsMeasure(call *callWithResult[Measurement], completion C.ustore_completion_t, collections Collections, arena *Arena, config *MeasureConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	count := tasks(collections)
	if err := shape(collections, count, len(options.StartKeys), len(options.EndKeys)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_blobs_measure_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	*arguments = C.ustore_blobs_measure_t{
		database:          database,
		executor:          executor,
		outcome:           call.outcome(),
		completion:        completion,
		view:              receiver.view(),
		count:             C.ustore_size_t(count),
		collections:       call.collections(inputs, collections),
		start_keys:        call.keys(options.StartKeys),
		end_keys:          call.keys(options.EndKeys),
		direction:         C.ustore_scan_direction_t(options.Direction),
		min_cardinalities: &call.result.minCardinalities,
		max_cardinalities: &call.result.maxCardinalities,
		min_value_bytes:   &call.result.minValueBytes,
		max_value_bytes:   &call.result.maxValueBytes,
		min_space_usages:  &call.result.minSpaceUsages,
		max_space_usages:  &call.result.maxSpaceUsages,
		arena:             outputs,
	}
	C.ustore_blobs_measure(arguments)
	return nil
}

// endregion: Samples and Measures
