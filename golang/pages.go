// Structured page collections, mirroring pages.h: imports, positional edits, renderings and walks.
//
// File: golang/pages.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore

/*
#cgo noescape ustore_pages_import
#cgo noescape ustore_pages_insert
#cgo nocallback ustore_pages_insert
#cgo noescape ustore_pages_remove
#cgo nocallback ustore_pages_remove
#cgo noescape ustore_pages_move
#cgo nocallback ustore_pages_move
#cgo noescape ustore_pages_splice
#cgo nocallback ustore_pages_splice
#cgo noescape ustore_pages_set_attributes
#cgo nocallback ustore_pages_set_attributes
#cgo noescape ustore_pages_remove_attributes
#cgo nocallback ustore_pages_remove_attributes
#cgo noescape ustore_pages_read
#cgo nocallback ustore_pages_read
#cgo noescape ustore_pages_scan
#cgo noescape ustore_pages_cursor_init
#cgo noescape ustore_pages_cursor_next
#cgo noescape ustore_pages_sample
#cgo nocallback ustore_pages_sample
#cgo noescape ustore_pages_measure
#cgo nocallback ustore_pages_measure
#cgo noescape ustore_page_encoding_from_name
#cgo nocallback ustore_page_encoding_from_name

#include "shim.h"
*/
import "C"

// region: Encodings

// PageEncoding is the encoding a page or a fragment is imported from or rendered into.
type PageEncoding int32

const (
	// PageEncodingHTML is HTML, not yet imported.
	PageEncodingHTML PageEncoding = C.ustore_page_encoding_html_k
	// PageEncodingXML is XML, with every element and attribute kept as written.
	PageEncodingXML PageEncoding = C.ustore_page_encoding_xml_k
	// PageEncodingMarkdown is Markdown, not yet imported.
	PageEncodingMarkdown PageEncoding = C.ustore_page_encoding_markdown_k
	// PageEncodingText is the text of every run in document order, with structure dropped.
	PageEncodingText PageEncoding = C.ustore_page_encoding_text_k
	// PageEncodingJSON is the element tree as JSON, not yet imported.
	PageEncodingJSON PageEncoding = C.ustore_page_encoding_json_k
)

// String renders the encoding as the library spells it.
func (encoding PageEncoding) String() string {
	return C.GoString(C.ustore_page_encoding_name(C.ustore_page_encoding_t(encoding)))
}

// PageEncodingFromName reads an encoding back from the spelling [PageEncoding.String] renders.
func PageEncodingFromName(name string) (PageEncoding, error) {
	var encoding C.ustore_page_encoding_t
	err := named(C.ustore_page_encoding_from_name(view(name), &encoding))
	return PageEncoding(encoding), err
}

// endregion: Encodings

// region: Edits

// PagesImportConfig carries the optional arguments of [DatabaseOn.PagesImport].
type PagesImportConfig struct {
	// Encoding is the encoding of the markup.
	Encoding PageEncoding
	// Durability is how durable the edits are once the call finishes at the head.
	Durability Durability
	// Progress reports the pages parsed, and may cancel the call.
	Progress ProgressCallback
}

// PagesImport replaces whole pages, one per key, with parsed markup.
func (on writes[Handle]) PagesImport(collections Collections, keys []Key, values Tape, config *PagesImportConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.pagesImport(&call, C.ustore_completion_blocking_k, collections, keys, values, config))
	return err
}

// PagesImport submits [DatabaseOn.PagesImport].
func (on deferredWrites[Handle]) PagesImport(collections Collections, keys []Key, values Tape, config *PagesImportConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.pagesImport(&deferred.call, C.ustore_completion_deferred_k, collections, keys, values, config))
}

func (receiver storeReceiver[Handle]) pagesImport(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, keys []Key, values Tape, config *PagesImportConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	if err := shape(collections, len(keys), int(values.Len())); err != nil {
		return err
	}
	options := configOf(config)
	arguments, inputs := stage[C.ustore_pages_import_t](call)
	progress, state := call.progress(options.Progress)
	*arguments = C.ustore_pages_import_t{
		database:       database,
		executor:       executor,
		outcome:        call.outcome(),
		completion:     completion,
		transaction:    receiver.transaction(),
		count:          C.ustore_size_t(len(keys)),
		collections:    call.collections(inputs, collections),
		keys:           call.keys(keys),
		values:         call.strings(values),
		encoding:       C.ustore_page_encoding_t(options.Encoding),
		durability:     C.ustore_durability_t(options.Durability),
		watch:          C.ustore_watch_t(receiver.watch),
		progress:       progress,
		progress_state: state,
	}
	C.ustore_pages_import(arguments)
	return nil
}

// PagesInsertConfig carries the optional arguments of [DatabaseOn.PagesInsert].
type PagesInsertConfig struct {
	// Elements are the keys to give the inserted elements, one per task, or keys minted for each
	// when empty.
	Elements []Key
	// Parents are the elements the fragments land under, one per task, the zero key for the root, or
	// the root for every one when empty.
	Parents []Key
	// Anchors are the siblings after which the fragments land, one per task, the zero key for the
	// front, or the front for every one when empty.
	Anchors []Key
	// Encoding is the encoding of the fragments.
	Encoding PageEncoding
	// Durability is how durable the edits are once the call finishes at the head.
	Durability Durability
}

// Inserted is the key every inserted element ended up with, minted or given, readable until the
// arena serves another call.
type Inserted struct {
	keys  *C.ustore_key_t
	count uint
	arena arenaWrite
}

// Keys views the keys of the inserted elements in the arena, one per fragment.
func (inserted Inserted) Keys() []Key {
	inserted.arena.mustOpen()
	defer inserted.arena.close()
	return keysAt(inserted.keys, 0, uint64(inserted.count))
}

// PagesInsert parses one fragment per task into elements of the page keys names, reporting the key
// every element ended up with into arena.
func (on writes[Handle]) PagesInsert(collections Collections, keys []Key, values Tape, arena *Arena, config *PagesInsertConfig) (Inserted, error) {
	var call callWithResult[Inserted]
	return block(&call, on.pagesInsert(&call, C.ustore_completion_blocking_k, collections, keys, values, arena, config))
}

// PagesInsert submits [DatabaseOn.PagesInsert].
func (on deferredWrites[Handle]) PagesInsert(collections Collections, keys []Key, values Tape, arena *Arena, config *PagesInsertConfig) (*Deferred[Inserted], error) {
	deferred := new(Deferred[Inserted])
	return submitted(deferred, on.pagesInsert(&deferred.call, C.ustore_completion_deferred_k, collections, keys, values, arena, config))
}

func (receiver storeReceiver[Handle]) pagesInsert(call *callWithResult[Inserted], completion C.ustore_completion_t, collections Collections, keys []Key, values Tape, arena *Arena, config *PagesInsertConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	if err := shape(collections, len(keys), int(values.Len()), len(options.Elements), len(options.Parents), len(options.Anchors)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_pages_insert_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(len(keys))
	*arguments = C.ustore_pages_insert_t{
		database:       database,
		executor:       executor,
		outcome:        call.outcome(),
		completion:     completion,
		transaction:    receiver.transaction(),
		count:          C.ustore_size_t(len(keys)),
		collections:    call.collections(inputs, collections),
		keys:           call.keys(keys),
		elements:       call.keys(options.Elements),
		parents:        call.keys(options.Parents),
		anchors:        call.keys(options.Anchors),
		values:         call.strings(values),
		encoding:       C.ustore_page_encoding_t(options.Encoding),
		effective_keys: &call.result.keys,
		arena:          outputs,
		durability:     C.ustore_durability_t(options.Durability),
		watch:          C.ustore_watch_t(receiver.watch),
	}
	C.ustore_pages_insert(arguments)
	return nil
}

// PagesRemoveConfig carries the optional arguments of [DatabaseOn.PagesRemove].
type PagesRemoveConfig struct {
	// Elements are the elements to remove with their subtrees, one per task.
	Elements []Key
	// EndKeys turn every key into the inclusive start of a range of whole pages ending before its end
	// key, one per task, which leaves Elements empty.
	EndKeys []Key
	// Durability is how durable the edits are once the call finishes at the head.
	Durability Durability
}

// PagesRemove removes elements with their subtrees, or whole pages of a range per task given end
// keys.
func (on writes[Handle]) PagesRemove(collections Collections, keys []Key, config *PagesRemoveConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.pagesRemove(&call, C.ustore_completion_blocking_k, collections, keys, config))
	return err
}

// PagesRemove submits [DatabaseOn.PagesRemove].
func (on deferredWrites[Handle]) PagesRemove(collections Collections, keys []Key, config *PagesRemoveConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.pagesRemove(&deferred.call, C.ustore_completion_deferred_k, collections, keys, config))
}

func (receiver storeReceiver[Handle]) pagesRemove(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, keys []Key, config *PagesRemoveConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	options := configOf(config)
	if err := shape(collections, len(keys), len(options.Elements), len(options.EndKeys)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_pages_remove_t](call)
	*arguments = C.ustore_pages_remove_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(len(keys)),
		collections: call.collections(inputs, collections),
		keys:        call.keys(keys),
		end_keys:    call.keys(options.EndKeys),
		elements:    call.keys(options.Elements),
		durability:  C.ustore_durability_t(options.Durability),
		watch:       C.ustore_watch_t(receiver.watch),
	}
	C.ustore_pages_remove(arguments)
	return nil
}

// PagesMoveConfig carries the optional arguments of [DatabaseOn.PagesMove].
type PagesMoveConfig struct {
	// Parents are the elements the moved elements land under, one per task, the zero key for the
	// root.
	Parents []Key
	// Anchors are the siblings after which the moved elements land, one per task, the zero key for
	// the front.
	Anchors []Key
	// Durability is how durable the edits are once the call finishes at the head.
	Durability Durability
}

// PagesMove moves elements with their subtrees under parents after anchors.
func (on writes[Handle]) PagesMove(collections Collections, keys, elements []Key, config *PagesMoveConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.pagesMove(&call, C.ustore_completion_blocking_k, collections, keys, elements, config))
	return err
}

// PagesMove submits [DatabaseOn.PagesMove].
func (on deferredWrites[Handle]) PagesMove(collections Collections, keys, elements []Key, config *PagesMoveConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.pagesMove(&deferred.call, C.ustore_completion_deferred_k, collections, keys, elements, config))
}

func (receiver storeReceiver[Handle]) pagesMove(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, keys, elements []Key, config *PagesMoveConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	options := configOf(config)
	if err := shape(collections, len(keys), len(elements), len(options.Parents), len(options.Anchors)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_pages_move_t](call)
	*arguments = C.ustore_pages_move_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(len(keys)),
		collections: call.collections(inputs, collections),
		keys:        call.keys(keys),
		elements:    call.keys(elements),
		parents:     call.keys(options.Parents),
		anchors:     call.keys(options.Anchors),
		durability:  C.ustore_durability_t(options.Durability),
		watch:       C.ustore_watch_t(receiver.watch),
	}
	C.ustore_pages_move(arguments)
	return nil
}

// PagesSplice replaces textLengths bytes of the text of every element from textStarts with values,
// where a zero length only inserts and an empty value only removes.
func (on writes[Handle]) PagesSplice(collections Collections, keys, elements []Key, textStarts, textLengths []uint64, values Tape, config *WriteConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.pagesSplice(&call, C.ustore_completion_blocking_k, collections, keys, elements, textStarts, textLengths, values, config))
	return err
}

// PagesSplice submits [DatabaseOn.PagesSplice].
func (on deferredWrites[Handle]) PagesSplice(collections Collections, keys, elements []Key, textStarts, textLengths []uint64, values Tape, config *WriteConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.pagesSplice(&deferred.call, C.ustore_completion_deferred_k, collections, keys, elements, textStarts, textLengths, values, config))
}

func (receiver storeReceiver[Handle]) pagesSplice(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, keys, elements []Key, textStarts, textLengths []uint64, values Tape, config *WriteConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	if err := shape(collections, len(keys), len(elements), len(textStarts), len(textLengths), int(values.Len())); err != nil {
		return err
	}
	options := configOf(config)
	arguments, inputs := stage[C.ustore_pages_splice_t](call)
	*arguments = C.ustore_pages_splice_t{
		database:     database,
		executor:     executor,
		outcome:      call.outcome(),
		completion:   completion,
		transaction:  receiver.transaction(),
		count:        C.ustore_size_t(len(keys)),
		collections:  call.collections(inputs, collections),
		keys:         call.keys(keys),
		elements:     call.keys(elements),
		text_starts:  call.lengths(textStarts),
		text_lengths: call.lengths(textLengths),
		values:       call.strings(values),
		durability:   C.ustore_durability_t(options.Durability),
		watch:        C.ustore_watch_t(receiver.watch),
	}
	C.ustore_pages_splice(arguments)
	return nil
}

// PagesSetAttributes sets one attribute of an element per task.
func (on writes[Handle]) PagesSetAttributes(collections Collections, keys, elements []Key, fields, values Tape, config *WriteConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.pagesSetAttributes(&call, C.ustore_completion_blocking_k, collections, keys, elements, fields, values, config))
	return err
}

// PagesSetAttributes submits [DatabaseOn.PagesSetAttributes].
func (on deferredWrites[Handle]) PagesSetAttributes(collections Collections, keys, elements []Key, fields, values Tape, config *WriteConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.pagesSetAttributes(&deferred.call, C.ustore_completion_deferred_k, collections, keys, elements, fields, values, config))
}

func (receiver storeReceiver[Handle]) pagesSetAttributes(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, keys, elements []Key, fields, values Tape, config *WriteConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	if err := shape(collections, len(keys), len(elements), int(fields.Len()), int(values.Len())); err != nil {
		return err
	}
	options := configOf(config)
	arguments, inputs := stage[C.ustore_pages_set_attributes_t](call)
	*arguments = C.ustore_pages_set_attributes_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(len(keys)),
		collections: call.collections(inputs, collections),
		keys:        call.keys(keys),
		elements:    call.keys(elements),
		fields:      call.strings(fields),
		values:      call.strings(values),
		durability:  C.ustore_durability_t(options.Durability),
		watch:       C.ustore_watch_t(receiver.watch),
	}
	C.ustore_pages_set_attributes(arguments)
	return nil
}

// PagesRemoveAttributes removes one attribute of an element per task.
func (on writes[Handle]) PagesRemoveAttributes(collections Collections, keys, elements []Key, fields Tape, config *WriteConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.pagesRemoveAttributes(&call, C.ustore_completion_blocking_k, collections, keys, elements, fields, config))
	return err
}

// PagesRemoveAttributes submits [DatabaseOn.PagesRemoveAttributes].
func (on deferredWrites[Handle]) PagesRemoveAttributes(collections Collections, keys, elements []Key, fields Tape, config *WriteConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.pagesRemoveAttributes(&deferred.call, C.ustore_completion_deferred_k, collections, keys, elements, fields, config))
}

func (receiver storeReceiver[Handle]) pagesRemoveAttributes(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, keys, elements []Key, fields Tape, config *WriteConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	if err := shape(collections, len(keys), len(elements), int(fields.Len())); err != nil {
		return err
	}
	options := configOf(config)
	arguments, inputs := stage[C.ustore_pages_remove_attributes_t](call)
	*arguments = C.ustore_pages_remove_attributes_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(len(keys)),
		collections: call.collections(inputs, collections),
		keys:        call.keys(keys),
		elements:    call.keys(elements),
		fields:      call.strings(fields),
		durability:  C.ustore_durability_t(options.Durability),
		watch:       C.ustore_watch_t(receiver.watch),
	}
	C.ustore_pages_remove_attributes(arguments)
	return nil
}

// endregion: Edits

// region: Reads

// PagesReadConfig carries the optional arguments of [DatabaseOn.PagesRead].
type PagesReadConfig struct {
	// Elements are the elements to render with their subtrees, one per key, the zero key for the
	// whole page, or whole pages when empty.
	Elements []Key
	// OutputsEncoding is the encoding the renderings are converted into.
	OutputsEncoding PageEncoding
}

// PagesRead renders whole pages or subtrees into arena, a missing one reading as absent.
func (on reads[Handle]) PagesRead(collections Collections, keys []Key, arena *Arena, config *PagesReadConfig) (Values, error) {
	var call callWithResult[Values]
	return block(&call, on.pagesRead(&call, C.ustore_completion_blocking_k, collections, keys, arena, config))
}

// PagesRead submits [DatabaseOn.PagesRead].
func (on deferredReads[Handle]) PagesRead(collections Collections, keys []Key, arena *Arena, config *PagesReadConfig) (*Deferred[Values], error) {
	deferred := new(Deferred[Values])
	return submitted(deferred, on.pagesRead(&deferred.call, C.ustore_completion_deferred_k, collections, keys, arena, config))
}

func (receiver storeReceiver[Handle]) pagesRead(call *callWithResult[Values], completion C.ustore_completion_t, collections Collections, keys []Key, arena *Arena, config *PagesReadConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	if err := shape(collections, len(keys), len(options.Elements)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_pages_read_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(len(keys))
	*arguments = C.ustore_pages_read_t{
		database:         database,
		executor:         executor,
		outcome:          call.outcome(),
		completion:       completion,
		view:             receiver.view(),
		count:            C.ustore_size_t(len(keys)),
		collections:      call.collections(inputs, collections),
		keys:             call.keys(keys),
		elements:         call.keys(options.Elements),
		presences:        &call.result.presences,
		values:           &call.result.values.raw,
		arena:            outputs,
		outputs_encoding: C.ustore_page_encoding_t(options.OutputsEncoding),
	}
	C.ustore_pages_read(arguments)
	return nil
}

// endregion: Reads

// region: Scans

// PagesScanConfig carries the optional arguments of [DatabaseOn.PagesScan].
type PagesScanConfig struct {
	// StartKeys are the inclusive starts of the scans, one per task, or the first page in Direction
	// when empty.
	StartKeys []Key
	// EndKeys are the keys every scan stops before, one per task, or no bound when empty.
	EndKeys []Key
	// Direction is which way every scan goes from its start.
	Direction ScanDirection
	// Filter is the expression every listed page matches, shared by every scan.
	Filter string
	// Predicate admits or passes over the listed keys.
	Predicate Admitter
	// Mode is in what order the keys come.
	Mode ScanMode
	// Progress reports the scans finished, and may cancel the call.
	Progress ProgressCallback
}

// PagesScan lists up to wanted page keys of a range per task into arena, one task per collection
// collections names.
func (on reads[Handle]) PagesScan(collections Collections, wanted uint, arena *Arena, config *PagesScanConfig) (Scanned, error) {
	var call callWithResult[Scanned]
	return block(&call, on.pagesScan(&call, C.ustore_completion_blocking_k, collections, wanted, arena, config))
}

// PagesScan submits [DatabaseOn.PagesScan].
func (on deferredReads[Handle]) PagesScan(collections Collections, wanted uint, arena *Arena, config *PagesScanConfig) (*Deferred[Scanned], error) {
	deferred := new(Deferred[Scanned])
	return submitted(deferred, on.pagesScan(&deferred.call, C.ustore_completion_deferred_k, collections, wanted, arena, config))
}

func (receiver storeReceiver[Handle]) pagesScan(call *callWithResult[Scanned], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena, config *PagesScanConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	count := tasks(collections)
	if err := shape(collections, count, len(options.StartKeys), len(options.EndKeys)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_pages_scan_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	progress, state := call.progress(options.Progress)
	*arguments = C.ustore_pages_scan_t{
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
	C.ustore_pages_scan(arguments)
	return nil
}

// endregion: Scans

// region: Cursors

// PagesCursorConfig carries the optional arguments of [DatabaseOn.PagesCursorInit].
type PagesCursorConfig struct {
	// EndKey is the key the walk stops before, or nil for no bound.
	EndKey *Key
	// Direction is which way the walk goes from its start.
	Direction ScanDirection
	// Filter is the expression every page the walk hands out matches, compiled once.
	Filter string
	// Predicate admits or passes over the keys of every page.
	Predicate Admitter
}

// PagesCursor walks the pages of one collection page by page, every pull resuming past the last key
// the pull before it handed out.
type PagesCursor struct {
	PagesCursorOn
	walk
	handle C.ustore_pages_cursor_t
}

// PagesCursorOn is the receiver whose pulls block until their call is final: on the calling
// goroutine when embedded in a [PagesCursor], and on an executor when [PagesCursorOn.On] makes it.
type PagesCursorOn struct {
	childReceiver[C.ustore_pages_cursor_t, C.ustore_database_t]
}

// On is the receiver whose pulls block on executor.
func (on PagesCursorOn) On(executor *Executor) PagesCursorOn {
	on.executor = executor
	return on
}

// PagesCursorInit opens a walk over collection from startKey, reading nothing until its first pull
// and blocking whatever receiver it is called on.
func (on reads[Handle]) PagesCursorInit(collection CollectionID, startKey Key, config *PagesCursorConfig) (*PagesCursor, error) {
	database, executor, err := on.lower()
	if err != nil {
		return nil, err
	}
	options := configOf(config)
	cursor := new(PagesCursor)
	var call callWithResult[C.ustore_pages_cursor_t]
	arguments, inputs := stage[C.ustore_pages_cursor_init_t](&call)
	*arguments = C.ustore_pages_cursor_init_t{
		cursor:     call.result,
		database:   database,
		executor:   executor,
		outcome:    call.outcome(),
		view:       on.view(),
		collection: rawCollection(collection),
		start_key:  raw(startKey),
		end_key:    call.key(inputs, options.EndKey),
		direction:  C.ustore_scan_direction_t(options.Direction),
		filter:     call.text(options.Filter),
		predicate:  cursor.admits(options.Predicate),
	}
	C.ustore_pages_cursor_init(arguments)
	if cursor.handle, err = block(&call, nil); err != nil {
		cursor.end()
		return nil, err
	}
	cursor.database = database
	cursor.childReceiver = childReceiver[C.ustore_pages_cursor_t, C.ustore_database_t]{receiver: receiver[C.ustore_pages_cursor_t]{handle: &cursor.handle}, parent: &cursor.database}
	return cursor, nil
}

// NextKeys pulls up to wanted page keys of the walk into arena.
func (on PagesCursorOn) NextKeys(wanted uint, arena *Arena) (Page, error) {
	var call callWithResult[Page]
	arguments, err := on.next(&call, wanted, arena)
	if err != nil {
		return Page{}, err
	}
	C.ustore_pages_cursor_next(arguments)
	return block(&call, nil)
}

// NextPairs pulls up to wanted page keys of the walk with their pages rendered as outputsEncoding
// into arena.
func (on PagesCursorOn) NextPairs(wanted uint, outputsEncoding PageEncoding, arena *Arena) (Page, error) {
	var call callWithResult[Page]
	arguments, err := on.next(&call, wanted, arena)
	if err != nil {
		return Page{}, err
	}
	arguments.values = &call.result.values.raw
	arguments.outputs_encoding = C.ustore_page_encoding_t(outputsEncoding)
	C.ustore_pages_cursor_next(arguments)
	return block(&call, nil)
}

// next opens a pull and fills every argument but the optional renderings.
func (on PagesCursorOn) next(call *callWithResult[Page], wanted uint, arena *Arena) (*C.ustore_pages_cursor_next_t, error) {
	cursor, database, executor, outputs, err := on.lowerInto(arena)
	if err != nil {
		return nil, err
	}
	arguments := open[C.ustore_pages_cursor_next_t](call)
	call.result.arena = call.stamp(arena, outputs)
	*arguments = C.ustore_pages_cursor_next_t{
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
func (cursor *PagesCursor) Close() error {
	if cursor == nil || cursor.handle == nil {
		return nil
	}
	C.ustore_pages_cursor_free(cursor.handle)
	cursor.handle = nil
	cursor.end()
	return nil
}

// endregion: Cursors

// region: Samples and Measures

// PagesSample draws up to wanted page keys uniformly at random into arena, one task per collection
// collections names.
func (on reads[Handle]) PagesSample(collections Collections, wanted uint, arena *Arena) (Scanned, error) {
	var call callWithResult[Scanned]
	return block(&call, on.pagesSample(&call, C.ustore_completion_blocking_k, collections, wanted, arena))
}

// PagesSample submits [DatabaseOn.PagesSample].
func (on deferredReads[Handle]) PagesSample(collections Collections, wanted uint, arena *Arena) (*Deferred[Scanned], error) {
	deferred := new(Deferred[Scanned])
	return submitted(deferred, on.pagesSample(&deferred.call, C.ustore_completion_deferred_k, collections, wanted, arena))
}

func (receiver storeReceiver[Handle]) pagesSample(call *callWithResult[Scanned], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	count := tasks(collections)
	arguments, inputs := stage[C.ustore_pages_sample_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	*arguments = C.ustore_pages_sample_t{
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
	C.ustore_pages_sample(arguments)
	return nil
}

// PagesMeasure estimates the pages and the space inside a range per task into arena, one task per
// collection collections names.
func (on reads[Handle]) PagesMeasure(collections Collections, arena *Arena, config *MeasureConfig) (Measurement, error) {
	var call callWithResult[Measurement]
	return block(&call, on.pagesMeasure(&call, C.ustore_completion_blocking_k, collections, arena, config))
}

// PagesMeasure submits [DatabaseOn.PagesMeasure].
func (on deferredReads[Handle]) PagesMeasure(collections Collections, arena *Arena, config *MeasureConfig) (*Deferred[Measurement], error) {
	deferred := new(Deferred[Measurement])
	return submitted(deferred, on.pagesMeasure(&deferred.call, C.ustore_completion_deferred_k, collections, arena, config))
}

func (receiver storeReceiver[Handle]) pagesMeasure(call *callWithResult[Measurement], completion C.ustore_completion_t, collections Collections, arena *Arena, config *MeasureConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	count := tasks(collections)
	if err := shape(collections, count, len(options.StartKeys), len(options.EndKeys)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_pages_measure_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	*arguments = C.ustore_pages_measure_t{
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
	C.ustore_pages_measure(arguments)
	return nil
}

// endregion: Samples and Measures
