// Collections of JSON documents: writes, reads, gists, gathers, scans and walks, mirroring docs.h.
//
// File: golang/docs.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore

import "unsafe"

/*
#cgo noescape ustore_docs_write
#cgo nocallback ustore_docs_write
#cgo noescape ustore_docs_read
#cgo nocallback ustore_docs_read
#cgo noescape ustore_docs_gist
#cgo nocallback ustore_docs_gist
#cgo noescape ustore_docs_gather
#cgo nocallback ustore_docs_gather
#cgo noescape ustore_docs_remove
#cgo nocallback ustore_docs_remove
#cgo noescape ustore_docs_scan
#cgo noescape ustore_docs_cursor_init
#cgo noescape ustore_docs_cursor_next
#cgo noescape ustore_docs_sample
#cgo nocallback ustore_docs_sample
#cgo noescape ustore_docs_measure
#cgo nocallback ustore_docs_measure
#cgo noescape ustore_docs_field_type_from_name
#cgo nocallback ustore_docs_field_type_from_name
#cgo noescape ustore_docs_modification_from_name
#cgo nocallback ustore_docs_modification_from_name

#include "shim.h"
*/
import "C"

// region: Enums

// DocsFieldType is the type of a document, or of a value extracted from one; every type but JSON
// text, strings and binary text is a fixed-width scalar.
type DocsFieldType int32

const (
	// DocsFieldTypeJSON is JSON text.
	DocsFieldTypeJSON DocsFieldType = C.ustore_docs_field_type_json_k
	// DocsFieldTypeNull is the JSON null.
	DocsFieldTypeNull DocsFieldType = C.ustore_docs_field_type_null_k
	// DocsFieldTypeBool is a boolean.
	DocsFieldTypeBool DocsFieldType = C.ustore_docs_field_type_bool_k
	// DocsFieldTypeUUID is a 128-bit UUID.
	DocsFieldTypeUUID DocsFieldType = C.ustore_docs_field_type_uuid_k
	// DocsFieldTypeF64 is a 64-bit float.
	DocsFieldTypeF64 DocsFieldType = C.ustore_docs_field_type_f64_k
	// DocsFieldTypeF32 is a 32-bit float.
	DocsFieldTypeF32 DocsFieldType = C.ustore_docs_field_type_f32_k
	// DocsFieldTypeF16 is a 16-bit float.
	DocsFieldTypeF16 DocsFieldType = C.ustore_docs_field_type_f16_k
	// DocsFieldTypeI64 is a signed 64-bit integer.
	DocsFieldTypeI64 DocsFieldType = C.ustore_docs_field_type_i64_k
	// DocsFieldTypeI32 is a signed 32-bit integer.
	DocsFieldTypeI32 DocsFieldType = C.ustore_docs_field_type_i32_k
	// DocsFieldTypeI16 is a signed 16-bit integer.
	DocsFieldTypeI16 DocsFieldType = C.ustore_docs_field_type_i16_k
	// DocsFieldTypeI8 is a signed 8-bit integer.
	DocsFieldTypeI8 DocsFieldType = C.ustore_docs_field_type_i8_k
	// DocsFieldTypeU64 is an unsigned 64-bit integer.
	DocsFieldTypeU64 DocsFieldType = C.ustore_docs_field_type_u64_k
	// DocsFieldTypeU32 is an unsigned 32-bit integer.
	DocsFieldTypeU32 DocsFieldType = C.ustore_docs_field_type_u32_k
	// DocsFieldTypeU16 is an unsigned 16-bit integer.
	DocsFieldTypeU16 DocsFieldType = C.ustore_docs_field_type_u16_k
	// DocsFieldTypeU8 is an unsigned 8-bit integer.
	DocsFieldTypeU8 DocsFieldType = C.ustore_docs_field_type_u8_k
	// DocsFieldTypeBinary is the text of a value without a null terminator.
	DocsFieldTypeBinary DocsFieldType = C.ustore_docs_field_type_binary_k
	// DocsFieldTypeString is a value printed as text.
	DocsFieldTypeString DocsFieldType = C.ustore_docs_field_type_string_k
)

// String renders the field type as the library spells it, such as "f64".
func (fieldType DocsFieldType) String() string {
	return C.GoString(C.ustore_docs_field_type_name(C.ustore_docs_field_type_t(fieldType)))
}

// DocsFieldTypeFromName reads a field type back from the spelling [DocsFieldType.String] renders.
func DocsFieldTypeFromName(name string) (DocsFieldType, error) {
	var fieldType C.ustore_docs_field_type_t
	err := named(C.ustore_docs_field_type_from_name(view(name), &fieldType))
	return DocsFieldType(fieldType), err
}

// DocsModification is the modification a document write applies.
type DocsModification int32

const (
	// DocsModifyUpsert writes whether or not the document exists.
	DocsModifyUpsert DocsModification = C.ustore_docs_modify_upsert_k
	// DocsModifyUpdate writes only documents that exist.
	DocsModifyUpdate DocsModification = C.ustore_docs_modify_update_k
	// DocsModifyInsert writes only documents that do not exist.
	DocsModifyInsert DocsModification = C.ustore_docs_modify_insert_k
	// DocsModifyPatch applies an RFC 6902 patch.
	DocsModifyPatch DocsModification = C.ustore_docs_modify_patch_k
	// DocsModifyMerge applies an RFC 7386 merge.
	DocsModifyMerge DocsModification = C.ustore_docs_modify_merge_k
)

// String renders the modification as the library spells it.
func (modification DocsModification) String() string {
	return C.GoString(C.ustore_docs_modification_name(C.ustore_docs_modification_t(modification)))
}

// DocsModificationFromName reads a modification back from the spelling [DocsModification.String]
// renders.
func DocsModificationFromName(name string) (DocsModification, error) {
	var modification C.ustore_docs_modification_t
	err := named(C.ustore_docs_modification_from_name(view(name), &modification))
	return DocsModification(modification), err
}

// endregion: Enums

// region: Writes

// DocsWriteConfig carries the optional arguments of [DatabaseOn.DocsWrite].
type DocsWriteConfig struct {
	// IDField is the field holding the key of every document when no keys are given, such as "_id".
	IDField string
	// Fields to modify inside the documents, one per task, or whole documents when empty.
	Fields Tape
	// Type is the format of the written values, JSON text by default.
	Type DocsFieldType
	// Modification is applied to every document, an upsert by default.
	Modification DocsModification
	// Durability is how durable the writes are once the call finishes at the head.
	Durability Durability
}

// DocsWrite writes one document or field per value, keyed by keys or, without them, by the
// [DocsWriteConfig] IDField of every document.
func (on writes[Handle]) DocsWrite(collections Collections, keys []Key, values Tape, config *DocsWriteConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.docsWrite(&call, C.ustore_completion_blocking_k, collections, keys, values, config))
	return err
}

// DocsWrite submits [DatabaseOn.DocsWrite].
func (on deferredWrites[Handle]) DocsWrite(collections Collections, keys []Key, values Tape, config *DocsWriteConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.docsWrite(&deferred.call, C.ustore_completion_deferred_k, collections, keys, values, config))
}

func (receiver storeReceiver[Handle]) docsWrite(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, keys []Key, values Tape, config *DocsWriteConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	options := configOf(config)
	count := int(values.Len())
	if err := shape(collections, count, len(keys), int(options.Fields.Len())); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_docs_write_t](call)
	*arguments = C.ustore_docs_write_t{
		database:     database,
		executor:     executor,
		outcome:      call.outcome(),
		completion:   completion,
		transaction:  receiver.transaction(),
		count:        C.ustore_size_t(count),
		collections:  call.collections(inputs, collections),
		keys:         call.keys(keys),
		id_field:     call.text(options.IDField),
		fields:       call.strings(options.Fields),
		values:       call.strings(values),
		_type:        C.ustore_docs_field_type_t(options.Type),
		modification: C.ustore_docs_modification_t(options.Modification),
		durability:   C.ustore_durability_t(options.Durability),
		watch:        C.ustore_watch_t(receiver.watch),
	}
	C.ustore_docs_write(arguments)
	return nil
}

// DocsRemove removes whole documents, or every document of a range per task given end keys.
func (on writes[Handle]) DocsRemove(collections Collections, keys []Key, config *RemoveConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.docsRemove(&call, C.ustore_completion_blocking_k, collections, keys, config))
	return err
}

// DocsRemove submits [DatabaseOn.DocsRemove].
func (on deferredWrites[Handle]) DocsRemove(collections Collections, keys []Key, config *RemoveConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.docsRemove(&deferred.call, C.ustore_completion_deferred_k, collections, keys, config))
}

func (receiver storeReceiver[Handle]) docsRemove(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, keys []Key, config *RemoveConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	options := configOf(config)
	if err := shape(collections, len(keys), len(options.EndKeys)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_docs_remove_t](call)
	*arguments = C.ustore_docs_remove_t{
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
	C.ustore_docs_remove(arguments)
	return nil
}

// endregion: Writes

// region: Reads

// DocsReadConfig carries the optional arguments of [DatabaseOn.DocsRead].
type DocsReadConfig struct {
	// Fields to read inside the documents, one per key, top-level keys or RFC 6901 pointers, or whole
	// documents when empty.
	Fields Tape
	// OutputsType is what the values are converted into, JSON text by default or a string.
	OutputsType DocsFieldType
}

// DocsRead reads whole documents or fields inside them into arena, a missing one reading as absent.
func (on reads[Handle]) DocsRead(collections Collections, keys []Key, arena *Arena, config *DocsReadConfig) (Values, error) {
	var call callWithResult[Values]
	return block(&call, on.docsRead(&call, C.ustore_completion_blocking_k, collections, keys, arena, config))
}

// DocsRead submits [DatabaseOn.DocsRead].
func (on deferredReads[Handle]) DocsRead(collections Collections, keys []Key, arena *Arena, config *DocsReadConfig) (*Deferred[Values], error) {
	deferred := new(Deferred[Values])
	return submitted(deferred, on.docsRead(&deferred.call, C.ustore_completion_deferred_k, collections, keys, arena, config))
}

func (receiver storeReceiver[Handle]) docsRead(call *callWithResult[Values], completion C.ustore_completion_t, collections Collections, keys []Key, arena *Arena, config *DocsReadConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	if err := shape(collections, len(keys), int(options.Fields.Len())); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_docs_read_t](call)
	call.result.count = uint(len(keys))
	*arguments = C.ustore_docs_read_t{
		database:     database,
		executor:     executor,
		outcome:      call.outcome(),
		completion:   completion,
		view:         receiver.view(),
		count:        C.ustore_size_t(len(keys)),
		collections:  call.collections(inputs, collections),
		keys:         call.keys(keys),
		fields:       call.strings(options.Fields),
		presences:    &call.result.presences,
		values:       &call.result.values.raw,
		arena:        outputs,
		outputs_type: C.ustore_docs_field_type_t(options.OutputsType),
	}
	C.ustore_docs_read(arguments)
	return nil
}

// Paths is every unique JSON Pointer to a leaf that a gist found, readable until the arena
// serves another call.
type Paths struct {
	fields  Strings
	written C.ustore_size_t
}

// Len reports how many unique paths the call found.
func (paths Paths) Len() uint { return uint(paths.written) }

// Get views path index in the arena.
func (paths Paths) Get(index uint) []byte { return paths.fields.sized(uint(paths.written)).Get(index) }

// DocsGist lists the unique paths to the leaves of the documents keys name into arena.
func (on reads[Handle]) DocsGist(collections Collections, keys []Key, arena *Arena) (Paths, error) {
	var call callWithResult[Paths]
	return block(&call, on.docsGist(&call, C.ustore_completion_blocking_k, collections, keys, arena))
}

// DocsGist submits [DatabaseOn.DocsGist].
func (on deferredReads[Handle]) DocsGist(collections Collections, keys []Key, arena *Arena) (*Deferred[Paths], error) {
	deferred := new(Deferred[Paths])
	return submitted(deferred, on.docsGist(&deferred.call, C.ustore_completion_deferred_k, collections, keys, arena))
}

func (receiver storeReceiver[Handle]) docsGist(call *callWithResult[Paths], completion C.ustore_completion_t, collections Collections, keys []Key, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	if err := shape(collections, len(keys)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_docs_gist_t](call)
	*arguments = C.ustore_docs_gist_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		view:        receiver.view(),
		count:       C.ustore_size_t(len(keys)),
		collections: call.collections(inputs, collections),
		keys:        call.keys(keys),
		fields:      &call.result.fields.raw,
		written:     &call.result.written,
		arena:       outputs,
	}
	C.ustore_docs_gist(arguments)
	return nil
}

// endregion: Reads

// region: Gathers

// Columns is one typed column per field a gather converted, one row per document, readable until
// the arena serves another call.
type Columns struct {
	columns *C.ustore_docs_column_view_t
	fields  uint
	rows    uint
}

// Len reports how many columns the call gathered, one per field.
func (columns Columns) Len() uint { return columns.fields }

// Column reports the column of field index.
func (columns Columns) Column(index uint) DocsColumn {
	column := (*C.ustore_docs_column_view_t)(unsafe.Add(unsafe.Pointer(columns.columns), uintptr(index)*unsafe.Sizeof(*columns.columns)))
	return DocsColumn{raw: column, rows: columns.rows}
}

// DocsColumn is the rows of one field as an Apache Arrow array of its type, beside the bitmaps of
// the rows found and converted, converted with a change of semantics, and colliding with a value of
// an inconvertible type.
type DocsColumn struct {
	raw  *C.ustore_docs_column_view_t
	rows uint
}

// Len reports how many rows the column holds.
func (column DocsColumn) Len() uint { return column.rows }

// Valid reports whether row's value was found and converted.
func (column DocsColumn) Valid(row uint) bool { return present(column.raw.validities, row) }

// Converted reports whether row's conversion changed its semantics, such as a string parsed into an
// integer.
func (column DocsColumn) Converted(row uint) bool { return present(column.raw.conversions, row) }

// Colliding reports whether a value of an inconvertible type sits under row's field.
func (column DocsColumn) Colliding(row uint) bool { return present(column.raw.collisions, row) }

// Strings views the rows of a string, binary or JSON column in the arena, and nothing for a
// scalar column.
func (column DocsColumn) Strings() Strings {
	return Strings{raw: column.raw.strings}.sized(column.rows)
}

// DocsScalar constrains the Go types a scalar column reads as, one per fixed-width
// [DocsFieldType]: a 16-bit float as its bits, and a UUID as a [Key].
type DocsScalar interface {
	~bool | ~int8 | ~int16 | ~int32 | ~int64 | ~uint8 | ~uint16 | ~uint32 | ~uint64 | ~float32 | ~float64 | ~[16]byte
}

// DocsColumnScalars views the scalars of a column gathered as the type Value spells, in place; a
// Value of another width than the column's type reads garbage, as the column does not record it.
func DocsColumnScalars[Value DocsScalar](column DocsColumn) []Value {
	if column.raw.scalars == nil || column.rows == 0 {
		return nil
	}
	return unsafe.Slice((*Value)(unsafe.Pointer(column.raw.scalars)), column.rows)
}

// DocsGather reads, type-checks and converts one column per field of the documents keys name into
// arena, converting field i into outputsTypes[i].
func (on reads[Handle]) DocsGather(collections Collections, keys []Key, fields Tape, outputsTypes []DocsFieldType, arena *Arena) (Columns, error) {
	var call callWithResult[Columns]
	return block(&call, on.docsGather(&call, C.ustore_completion_blocking_k, collections, keys, fields, outputsTypes, arena))
}

// DocsGather submits [DatabaseOn.DocsGather].
func (on deferredReads[Handle]) DocsGather(collections Collections, keys []Key, fields Tape, outputsTypes []DocsFieldType, arena *Arena) (*Deferred[Columns], error) {
	deferred := new(Deferred[Columns])
	return submitted(deferred, on.docsGather(&deferred.call, C.ustore_completion_deferred_k, collections, keys, fields, outputsTypes, arena))
}

func (receiver storeReceiver[Handle]) docsGather(call *callWithResult[Columns], completion C.ustore_completion_t, collections Collections, keys []Key, fields Tape, outputsTypes []DocsFieldType, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	if err := shape(collections, len(keys)); err != nil {
		return err
	}
	if uint(len(outputsTypes)) != fields.Len() {
		return fail(StatusInvalidArgument, "pass one output type per field")
	}
	arguments, inputs := stage[C.ustore_docs_gather_t](call)
	call.result.fields, call.result.rows = fields.Len(), uint(len(keys))
	*arguments = C.ustore_docs_gather_t{
		database:      database,
		executor:      executor,
		outcome:       call.outcome(),
		completion:    completion,
		view:          receiver.view(),
		count:         C.ustore_size_t(len(keys)),
		collections:   call.collections(inputs, collections),
		keys:          call.keys(keys),
		fields_count:  C.ustore_size_t(fields.Len()),
		fields:        call.strings(fields),
		columns:       &call.result.columns,
		arena:         outputs,
		outputs_types: (*C.ustore_docs_field_type_t)(call.pinSlice(unsafe.Pointer(unsafe.SliceData(outputsTypes)), len(outputsTypes))),
	}
	C.ustore_docs_gather(arguments)
	return nil
}

// endregion: Gathers

// region: Scans

// DocsScanConfig carries the optional arguments of [DatabaseOn.DocsScan].
type DocsScanConfig struct {
	// StartKeys are the inclusive starts of the scans, one per task, or the first key in Direction
	// when empty.
	StartKeys []Key
	// EndKeys are the keys every scan stops before, one per task, or no bound when empty.
	EndKeys []Key
	// Direction is which way every scan goes from its start.
	Direction ScanDirection
	// Filter is a JSONPath, or a bare root filter such as ?@.age>30, every listed document matches.
	Filter string
	// Predicate admits or passes over the listed keys.
	Predicate Admitter
	// Mode is in what order the keys come.
	Mode ScanMode
	// Progress reports the scans finished, and may cancel the call.
	Progress ProgressCallback
}

// DocsScan lists up to wanted document keys of a range per task into arena, one task per collection
// collections names.
func (on reads[Handle]) DocsScan(collections Collections, wanted uint, arena *Arena, config *DocsScanConfig) (Scanned, error) {
	var call callWithResult[Scanned]
	return block(&call, on.docsScan(&call, C.ustore_completion_blocking_k, collections, wanted, arena, config))
}

// DocsScan submits [DatabaseOn.DocsScan].
func (on deferredReads[Handle]) DocsScan(collections Collections, wanted uint, arena *Arena, config *DocsScanConfig) (*Deferred[Scanned], error) {
	deferred := new(Deferred[Scanned])
	return submitted(deferred, on.docsScan(&deferred.call, C.ustore_completion_deferred_k, collections, wanted, arena, config))
}

func (receiver storeReceiver[Handle]) docsScan(call *callWithResult[Scanned], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena, config *DocsScanConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	count := tasks(collections)
	if err := shape(collections, count, len(options.StartKeys), len(options.EndKeys)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_docs_scan_t](call)
	call.result.count = uint(count)
	progress, state := call.progress(options.Progress)
	*arguments = C.ustore_docs_scan_t{
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
		filter:         call.text(options.Filter),
		predicate:      call.predicate(options.Predicate),
		offsets:        &call.result.offsets,
		keys:           &call.result.keys,
		arena:          outputs,
		wanted:         C.ustore_size_t(wanted),
		mode:           C.ustore_scan_mode_t(options.Mode),
		progress:       progress,
		progress_state: state,
	}
	C.ustore_docs_scan(arguments)
	return nil
}

// endregion: Scans

// region: Cursors

// DocsCursorConfig carries the optional arguments of [DatabaseOn.DocsCursorInit].
type DocsCursorConfig struct {
	// EndKey is the key the walk stops before, or nil for no bound.
	EndKey *Key
	// Direction is which way the walk goes from its start.
	Direction ScanDirection
	// Filter is a JSONPath every document the walk hands out matches, compiled once.
	Filter string
	// Predicate admits or passes over the keys of every page.
	Predicate Admitter
	// Fields are the fields every pull gathers into the columns of its batches, the empty field
	// being the whole document, or none for keys alone.
	Fields Tape
	// ColumnsTypes holds the type every field converts into, one per field.
	ColumnsTypes []DocsFieldType
}

// DocsCursor walks the documents of one collection in batches of rows, every pull resuming past
// the last key the pull before it handed out.
type DocsCursor struct {
	DocsCursorOn
	walk
	handle C.ustore_docs_cursor_t
}

// DocsCursorOn is the receiver whose pulls block until their call is final: on the calling
// goroutine when embedded in a [DocsCursor], and on an executor when [DocsCursorOn.On] makes it.
type DocsCursorOn struct {
	childReceiver[C.ustore_docs_cursor_t, C.ustore_database_t]
	fields uint
}

// On is the receiver whose pulls block on executor.
func (on DocsCursorOn) On(executor *Executor) DocsCursorOn {
	on.executor = executor
	return on
}

// DocsCursorInit opens a walk over collection from startKey, reading nothing until its first pull
// and blocking whatever receiver it is called on.
func (on reads[Handle]) DocsCursorInit(collection CollectionID, startKey Key, config *DocsCursorConfig) (*DocsCursor, error) {
	database, executor, err := on.lower()
	if err != nil {
		return nil, err
	}
	options := configOf(config)
	if uint(len(options.ColumnsTypes)) != options.Fields.Len() {
		return nil, fail(StatusInvalidArgument, "pass one column type per field")
	}
	cursor := new(DocsCursor)
	var call callWithResult[C.ustore_docs_cursor_t]
	arguments, inputs := stage[C.ustore_docs_cursor_init_t](&call)
	*arguments = C.ustore_docs_cursor_init_t{
		cursor:        call.result,
		database:      database,
		executor:      executor,
		outcome:       call.outcome(),
		view:          on.view(),
		collection:    rawCollection(collection),
		start_key:     raw(startKey),
		end_key:       call.key(inputs, options.EndKey),
		direction:     C.ustore_scan_direction_t(options.Direction),
		filter:        call.text(options.Filter),
		predicate:     cursor.admits(options.Predicate),
		fields_count:  C.ustore_size_t(options.Fields.Len()),
		fields:        call.strings(options.Fields),
		columns_types: (*C.ustore_docs_field_type_t)(call.pinSlice(unsafe.Pointer(unsafe.SliceData(options.ColumnsTypes)), len(options.ColumnsTypes))),
	}
	C.ustore_docs_cursor_init(arguments)
	if cursor.handle, err = block(&call, nil); err != nil {
		cursor.end()
		return nil, err
	}
	cursor.database = database
	cursor.childReceiver = childReceiver[C.ustore_docs_cursor_t, C.ustore_database_t]{receiver: receiver[C.ustore_docs_cursor_t]{handle: &cursor.handle}, parent: &cursor.database}
	cursor.fields = options.Fields.Len()
	return cursor, nil
}

// DocsBatches is what one pull of a [DocsCursor] wrote into its arena in walk order, one batch per
// executor task that found rows, readable until the arena serves another call.
type DocsBatches struct {
	batches *C.ustore_docs_batch_view_t
	count   C.ustore_size_t
	fields  uint
}

// Len reports how many batches the pull handed out, none once the walk is exhausted.
func (batches DocsBatches) Len() uint { return uint(batches.count) }

// Keys views the keys of batch index in the arena, one per row.
func (batches DocsBatches) Keys(index uint) []Key {
	batch := batches.at(index)
	return keysAt(batch.keys, 0, uint64(batch.count))
}

// Columns views the columns of batch index in the arena, one per field the cursor gathers.
func (batches DocsBatches) Columns(index uint) Columns {
	batch := batches.at(index)
	return Columns{columns: batch.columns, fields: batches.fields, rows: uint(batch.count)}
}

// at addresses batch index of the pull.
func (batches DocsBatches) at(index uint) *C.ustore_docs_batch_view_t {
	return (*C.ustore_docs_batch_view_t)(unsafe.Add(unsafe.Pointer(batches.batches), uintptr(index)*unsafe.Sizeof(*batches.batches)))
}

// Next pulls batches of up to wanted rows in all into arena, each row a key and a cell of every
// column the cursor gathers.
func (on DocsCursorOn) Next(wanted uint, arena *Arena) (DocsBatches, error) {
	cursor, database, executor, outputs, err := on.lowerInto(arena)
	if err != nil {
		return DocsBatches{}, err
	}
	var call callWithResult[DocsBatches]
	arguments := open[C.ustore_docs_cursor_next_t](&call)
	call.result.fields = on.fields
	*arguments = C.ustore_docs_cursor_next_t{
		database:      database,
		executor:      executor,
		outcome:       call.outcome(),
		cursor:        cursor,
		batches:       &call.result.batches,
		batches_count: &call.result.count,
		arena:         outputs,
		wanted:        C.ustore_size_t(wanted),
	}
	C.ustore_docs_cursor_next(arguments)
	return block(&call, nil)
}

// Close frees the walk, giving back the holds it kept. It is idempotent.
func (cursor *DocsCursor) Close() error {
	if cursor == nil || cursor.handle == nil {
		return nil
	}
	C.ustore_docs_cursor_free(cursor.handle)
	cursor.handle = nil
	cursor.end()
	return nil
}

// endregion: Cursors

// region: Samples and Measures

// DocsSample draws up to wanted document keys uniformly at random into arena, one task per
// collection collections names.
func (on reads[Handle]) DocsSample(collections Collections, wanted uint, arena *Arena) (Scanned, error) {
	var call callWithResult[Scanned]
	return block(&call, on.docsSample(&call, C.ustore_completion_blocking_k, collections, wanted, arena))
}

// DocsSample submits [DatabaseOn.DocsSample].
func (on deferredReads[Handle]) DocsSample(collections Collections, wanted uint, arena *Arena) (*Deferred[Scanned], error) {
	deferred := new(Deferred[Scanned])
	return submitted(deferred, on.docsSample(&deferred.call, C.ustore_completion_deferred_k, collections, wanted, arena))
}

func (receiver storeReceiver[Handle]) docsSample(call *callWithResult[Scanned], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	count := tasks(collections)
	arguments, inputs := stage[C.ustore_docs_sample_t](call)
	call.result.count = uint(count)
	*arguments = C.ustore_docs_sample_t{
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
	C.ustore_docs_sample(arguments)
	return nil
}

// DocsMeasure estimates the documents and the space inside a range per task into arena, one task
// per collection collections names.
func (on reads[Handle]) DocsMeasure(collections Collections, arena *Arena, config *MeasureConfig) (Measurement, error) {
	var call callWithResult[Measurement]
	return block(&call, on.docsMeasure(&call, C.ustore_completion_blocking_k, collections, arena, config))
}

// DocsMeasure submits [DatabaseOn.DocsMeasure].
func (on deferredReads[Handle]) DocsMeasure(collections Collections, arena *Arena, config *MeasureConfig) (*Deferred[Measurement], error) {
	deferred := new(Deferred[Measurement])
	return submitted(deferred, on.docsMeasure(&deferred.call, C.ustore_completion_deferred_k, collections, arena, config))
}

func (receiver storeReceiver[Handle]) docsMeasure(call *callWithResult[Measurement], completion C.ustore_completion_t, collections Collections, arena *Arena, config *MeasureConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	count := tasks(collections)
	if err := shape(collections, count, len(options.StartKeys), len(options.EndKeys)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_docs_measure_t](call)
	call.result.count = uint(count)
	*arguments = C.ustore_docs_measure_t{
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
	C.ustore_docs_measure(arguments)
	return nil
}

// endregion: Samples and Measures
