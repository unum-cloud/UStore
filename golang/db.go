// Databases, snapshots, transactions, views, collections, change feeds and ingests, mirroring db.h.
//
// File: golang/db.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore

import (
	"encoding/binary"
	"encoding/hex"
	"runtime"
	"runtime/cgo"
	"unsafe"
)

/*
// Every verb here retains nothing it is handed past its outcome, so each claims noescape; those
// that take no callback claim nocallback too.
#cgo noescape ustore_database_init_ram
#cgo nocallback ustore_database_init_ram
#cgo noescape ustore_database_open_ram
#cgo nocallback ustore_database_open_ram
#cgo noescape ustore_database_open_rocksdb
#cgo nocallback ustore_database_open_rocksdb
#cgo noescape ustore_database_view_rocksdb
#cgo nocallback ustore_database_view_rocksdb
#cgo noescape ustore_database_open_leveldb
#cgo nocallback ustore_database_open_leveldb
#cgo noescape ustore_database_open_nvme
#cgo nocallback ustore_database_open_nvme
#cgo noescape ustore_database_describe
#cgo nocallback ustore_database_describe
#cgo noescape ustore_database_clear
#cgo nocallback ustore_database_clear
#cgo noescape ustore_database_compact
#cgo noescape ustore_database_compact_plan_finish
#cgo nocallback ustore_database_compact_plan_finish
#cgo noescape ustore_database_flush
#cgo nocallback ustore_database_flush
#cgo noescape ustore_snapshot_init
#cgo nocallback ustore_snapshot_init
#cgo noescape ustore_snapshot_save
#cgo noescape ustore_collection_list
#cgo nocallback ustore_collection_list
#cgo noescape ustore_collection_create
#cgo nocallback ustore_collection_create
#cgo noescape ustore_collection_find
#cgo nocallback ustore_collection_find
#cgo noescape ustore_collection_drop
#cgo nocallback ustore_collection_drop
#cgo noescape ustore_transaction_init
#cgo nocallback ustore_transaction_init
#cgo noescape ustore_transaction_stage
#cgo nocallback ustore_transaction_stage
#cgo noescape ustore_transaction_commit
#cgo nocallback ustore_transaction_commit
#cgo noescape ustore_transaction_rollback
#cgo nocallback ustore_transaction_rollback
#cgo noescape ustore_changes_cursor_init
#cgo nocallback ustore_changes_cursor_init
#cgo noescape ustore_changes_cursor_next
#cgo nocallback ustore_changes_cursor_next
#cgo noescape ustore_ingest_init
#cgo nocallback ustore_ingest_init
#cgo noescape ustore_ingest_append
#cgo nocallback ustore_ingest_append
#cgo noescape ustore_ingest_finish
#cgo nocallback ustore_ingest_finish
#cgo noescape ustore_durability_from_name
#cgo nocallback ustore_durability_from_name
#cgo noescape ustore_watch_from_name
#cgo nocallback ustore_watch_from_name
#cgo noescape ustore_scan_mode_from_name
#cgo nocallback ustore_scan_mode_from_name
#cgo noescape ustore_scan_direction_from_name
#cgo nocallback ustore_scan_direction_from_name
#cgo noescape ustore_drop_mode_from_name
#cgo nocallback ustore_drop_mode_from_name
#cgo noescape ustore_isolation_from_name
#cgo nocallback ustore_isolation_from_name
#cgo noescape ustore_engine_from_name
#cgo nocallback ustore_engine_from_name
#cgo noescape ustore_modality_from_name
#cgo nocallback ustore_modality_from_name
#cgo noescape ustore_change_kind_from_name
#cgo nocallback ustore_change_kind_from_name
#cgo noescape ustore_ingest_order_from_name
#cgo nocallback ustore_ingest_order_from_name

#include "shim.h"
*/
import "C"

// region: Keys

// Key identifies a value within one collection: sixteen bytes, ordered as their bytes. An integer
// spells as its 128-bit big-endian bytes, so keys made from integers order as the integers.
type Key [16]byte

// KeyUnknown is the key that stands for no key, every bit of it set, the start of a descending walk
// over a whole collection.
var KeyUnknown = Key{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}

// Keys and collections cross as the C structures, which hold the same sixteen bytes.
var _ = [1]struct{}{}[unsafe.Sizeof(Key{})-unsafe.Sizeof(C.ustore_key_t{})]
var _ = [1]struct{}{}[unsafe.Sizeof(CollectionID{})-unsafe.Sizeof(C.ustore_collection_id_t{})]

// KeyFromUint64 spells an integer as its 128-bit big-endian bytes.
func KeyFromUint64(value uint64) Key {
	var key Key
	binary.BigEndian.PutUint64(key[8:], value)
	return key
}

// String renders the key as a lowercase dashed UUID.
func (key Key) String() string { return dashed(key) }

// CollectionID identifies a collection: a UUIDv7 its create mints once and never reuses. The zero
// value names none.
type CollectionID [16]byte

// String renders the identifier as a lowercase dashed UUID.
func (collection CollectionID) String() string { return dashed(collection) }

// dashed renders sixteen bytes as a lowercase dashed UUID.
func dashed(bytes [16]byte) string {
	var rendered [36]byte
	hex.Encode(rendered[0:8], bytes[0:4])
	hex.Encode(rendered[9:13], bytes[4:6])
	hex.Encode(rendered[14:18], bytes[6:8])
	hex.Encode(rendered[19:23], bytes[8:10])
	hex.Encode(rendered[24:36], bytes[10:16])
	rendered[8], rendered[13], rendered[18], rendered[23] = '-', '-', '-', '-'
	return string(rendered[:])
}

// ReadStamp is the stamp a transaction or a snapshot reads at.
type ReadStamp uint64

// CommitStamp is a commit's position in commit order, which a read at any [ReadStamp] at or
// above it sees.
type CommitStamp uint64

// Collections names the collection of every task in a batch: a [CollectionID] the whole batch
// shares, or [Each], one per task.
type Collections interface{ collections() }

func (CollectionID) collections() {}

// Each names one collection per task of a batch.
type Each []CollectionID

func (Each) collections() {}

// tasks reports how many tasks collections address on their own: one per entry of Each, or one.
func tasks(collections Collections) int {
	if each, ok := collections.(Each); ok {
		return len(each)
	}
	return 1
}

// shape refuses runs the library would read past: Each of another length than the batch, or an
// optional run neither empty nor one entry per task.
func shape(collections Collections, count int, lengths ...int) error {
	if each, ok := collections.(Each); ok && len(each) != count {
		return fail(StatusInvalidArgument, "pass one collection per task")
	}
	for _, length := range lengths {
		if length != 0 && length != count {
			return fail(StatusInvalidArgument, "pass every run empty or with one entry per task")
		}
	}
	return nil
}

// endregion: Keys

// region: Enums

// Modality is what a collection holds, recorded beside its name so that a verb of another kind
// is refused.
type Modality int32

const (
	// ModalityBlobs holds binary values of blobs.h.
	ModalityBlobs Modality = C.ustore_modality_blobs_k
	// ModalityDocs holds JSON documents of docs.h.
	ModalityDocs Modality = C.ustore_modality_docs_k
	// ModalityGraph holds vertices and their edges of graph.h.
	ModalityGraph Modality = C.ustore_modality_graph_k
	// ModalityStrings holds values keyed by strings of strings.h.
	ModalityStrings Modality = C.ustore_modality_strings_k
	// ModalityPages holds structured pages of pages.h.
	ModalityPages Modality = C.ustore_modality_pages_k
)

// String renders the modality as the library spells it.
func (modality Modality) String() string {
	return C.GoString(C.ustore_modality_name(C.ustore_modality_t(modality)))
}

// ModalityFromName reads a modality back from the spelling [Modality.String] renders.
func ModalityFromName(name string) (Modality, error) {
	var modality C.ustore_modality_t
	err := named(C.ustore_modality_from_name(view(name), &modality))
	return Modality(modality), err
}

// Durability is how durable a write at the head is once the call finishes; a transaction's commit
// decides its own.
type Durability int32

const (
	// DurabilityBuffered is visible on return and durable at the next flush.
	DurabilityBuffered Durability = C.ustore_durability_buffered_k
	// DurabilityFlushed is durable on return.
	DurabilityFlushed Durability = C.ustore_durability_flushed_k
)

// String renders the durability as the library spells it.
func (durability Durability) String() string {
	return C.GoString(C.ustore_durability_name(C.ustore_durability_t(durability)))
}

// DurabilityFromName reads a durability back from the spelling [Durability.String] renders.
func DurabilityFromName(name string) (Durability, error) {
	var durability C.ustore_durability_t
	err := named(C.ustore_durability_from_name(view(name), &durability))
	return Durability(durability), err
}

// Watch is whether a transaction records the keys a call reads or writes, to refuse its commit when
// another writer changes them first.
type Watch int32

const (
	// WatchKeys records the keys, so a concurrent change to any of them fails the commit.
	WatchKeys Watch = C.ustore_watch_keys_k
	// WatchNone records nothing, so concurrent changes to these keys never fail the commit.
	WatchNone Watch = C.ustore_watch_none_k
)

// String renders the watch as the library spells it.
func (watch Watch) String() string { return C.GoString(C.ustore_watch_name(C.ustore_watch_t(watch))) }

// WatchFromName reads a watch back from the spelling [Watch.String] renders.
func WatchFromName(name string) (Watch, error) {
	var watch C.ustore_watch_t
	err := named(C.ustore_watch_from_name(view(name), &watch))
	return Watch(watch), err
}

// ScanMode is in what order a scan hands its keys over.
type ScanMode int32

const (
	// ScanOrdered hands keys over in their order.
	ScanOrdered ScanMode = C.ustore_scan_ordered_k
	// ScanUnordered hands keys over in any order, as fast as the engine finds them.
	ScanUnordered ScanMode = C.ustore_scan_unordered_k
)

// String renders the scan mode as the library spells it.
func (mode ScanMode) String() string {
	return C.GoString(C.ustore_scan_mode_name(C.ustore_scan_mode_t(mode)))
}

// ScanModeFromName reads a scan mode back from the spelling [ScanMode.String] renders.
func ScanModeFromName(name string) (ScanMode, error) {
	var mode C.ustore_scan_mode_t
	err := named(C.ustore_scan_mode_from_name(view(name), &mode))
	return ScanMode(mode), err
}

// ScanDirection is which way a scan, a walk or a measure goes from its start key.
type ScanDirection int32

const (
	// ScanAscending walks up from the start key, the default.
	ScanAscending ScanDirection = C.ustore_scan_ascending_k
	// ScanDescending walks down from the start key.
	ScanDescending ScanDirection = C.ustore_scan_descending_k
)

// String renders the direction as the library spells it.
func (direction ScanDirection) String() string {
	return C.GoString(C.ustore_scan_direction_name(C.ustore_scan_direction_t(direction)))
}

// ScanDirectionFromName reads a direction back from the spelling [ScanDirection.String] renders.
func ScanDirectionFromName(name string) (ScanDirection, error) {
	var direction C.ustore_scan_direction_t
	err := named(C.ustore_scan_direction_from_name(view(name), &direction))
	return ScanDirection(direction), err
}

// DropMode is what a drop or a clear removes.
type DropMode int32

const (
	// DropCollection removes the collection with everything it holds.
	DropCollection DropMode = C.ustore_drop_collection_k
	// DropPairs removes every key and value, keeping the collection.
	DropPairs DropMode = C.ustore_drop_pairs_k
	// DropValues empties every value, keeping its key.
	DropValues DropMode = C.ustore_drop_values_k
)

// String renders the drop mode as the library spells it.
func (mode DropMode) String() string {
	return C.GoString(C.ustore_drop_mode_name(C.ustore_drop_mode_t(mode)))
}

// DropModeFromName reads a drop mode back from the spelling [DropMode.String] renders.
func DropModeFromName(name string) (DropMode, error) {
	var mode C.ustore_drop_mode_t
	err := named(C.ustore_drop_mode_from_name(view(name), &mode))
	return DropMode(mode), err
}

// Capabilities is the set of features an engine reports, one bit each.
type Capabilities int32

const (
	// CapabilitiesTransactions means the engine supports transactions.
	CapabilitiesTransactions Capabilities = C.ustore_capabilities_transactions_k
	// CapabilitiesSnapshots means the engine supports snapshots.
	CapabilitiesSnapshots Capabilities = C.ustore_capabilities_snapshots_k
	// CapabilitiesSavesSnapshots means the engine writes a snapshot out as a database of its own.
	CapabilitiesSavesSnapshots Capabilities = C.ustore_capabilities_saves_snapshots_k
)

// Isolation is how the transactions of the in-memory engine see each other's writes.
type Isolation int32

const (
	// IsolationStrictSerializable makes every transaction appear to run alone at one moment.
	IsolationStrictSerializable Isolation = C.ustore_isolation_strict_serializable_k
	// IsolationSnapshot makes every transaction read the state its start saw.
	IsolationSnapshot Isolation = C.ustore_isolation_snapshot_k
)

// String renders the isolation as the library spells it.
func (isolation Isolation) String() string {
	return C.GoString(C.ustore_isolation_name(C.ustore_isolation_t(isolation)))
}

// IsolationFromName reads an isolation back from the spelling [Isolation.String] renders.
func IsolationFromName(name string) (Isolation, error) {
	var isolation C.ustore_isolation_t
	err := named(C.ustore_isolation_from_name(view(name), &isolation))
	return Isolation(isolation), err
}

// Engine is the engine serving a database.
type Engine int32

const (
	// EngineRam keeps everything in memory, imaged into a directory on demand.
	EngineRam Engine = C.ustore_engine_ram_k
	// EngineRocksDB serves a RocksDB directory.
	EngineRocksDB Engine = C.ustore_engine_rocksdb_k
	// EngineLevelDB serves a LevelDB directory.
	EngineLevelDB Engine = C.ustore_engine_leveldb_k
	// EngineNvme is the in-tree NVMe engine.
	EngineNvme Engine = C.ustore_engine_nvme_k
)

// String renders the engine as the library spells it.
func (engine Engine) String() string {
	return C.GoString(C.ustore_engine_name(C.ustore_engine_t(engine)))
}

// EngineFromName reads an engine back from the spelling [Engine.String] renders.
func EngineFromName(name string) (Engine, error) {
	var engine C.ustore_engine_t
	err := named(C.ustore_engine_from_name(view(name), &engine))
	return Engine(engine), err
}

// ChangeKind is what one committed change did to its key.
type ChangeKind int32

const (
	// ChangeWrite wrote the whole value.
	ChangeWrite ChangeKind = C.ustore_change_write_k
	// ChangeRemoval removed the key.
	ChangeRemoval ChangeKind = C.ustore_change_removal_k
	// ChangeChange changed part of the value, as a document patch or a page edit does.
	ChangeChange ChangeKind = C.ustore_change_change_k
)

// String renders the change kind as the library spells it.
func (kind ChangeKind) String() string {
	return C.GoString(C.ustore_change_kind_name(C.ustore_change_kind_t(kind)))
}

// ChangeKindFromName reads a change kind back from the spelling [ChangeKind.String] renders.
func ChangeKindFromName(name string) (ChangeKind, error) {
	var kind C.ustore_change_kind_t
	err := named(C.ustore_change_kind_from_name(view(name), &kind))
	return ChangeKind(kind), err
}

// IngestOrder is in what order an ingest's rows arrive.
type IngestOrder int32

const (
	// IngestSorted takes keys strictly ascending across every append.
	IngestSorted IngestOrder = C.ustore_ingest_sorted_k
	// IngestUnsorted takes keys in any order, the last row of a key winning.
	IngestUnsorted IngestOrder = C.ustore_ingest_unsorted_k
)

// String renders the order as the library spells it.
func (order IngestOrder) String() string {
	return C.GoString(C.ustore_ingest_order_name(C.ustore_ingest_order_t(order)))
}

// IngestOrderFromName reads an ingest order back from the spelling [IngestOrder.String] renders.
func IngestOrderFromName(name string) (IngestOrder, error) {
	var order C.ustore_ingest_order_t
	err := named(C.ustore_ingest_order_from_name(view(name), &order))
	return IngestOrder(order), err
}

// endregion: Enums

// region: Predicates

// Admitter is the predicate a scan or a cursor asks about the keys it lists: a [Predicate], a
// [BatchPredicate] or a [CheapBatchPredicate], whose type tells the library what one call costs.
type Admitter interface {
	// admitter lends the handler through handle, which stays in place until the library is done with
	// it.
	admitter(handle *cgo.Handle) C.ustore_predicate_t
}

// Predicate admits one listed key into a scan or a walk, or passes over it, asked once per page
// before patterns or filters read any value. It runs on the host thread serving the call, never on
// two at once, and it must not panic.
type Predicate func(key Key) bool

// BatchPredicate writes an Admission for every key into admissions, which start as Reject, for
// handlers that pay per call: it is asked once per page about every key patterns and filters kept.
// Both slices live only until it returns. It runs on the host thread serving the call, never on
// two at once, and it must not panic.
type BatchPredicate func(keys []Key, admissions []Admission)

// CheapBatchPredicate is a [BatchPredicate] asked once per page before patterns or filters read any
// value, so it narrows those reads.
type CheapBatchPredicate func(keys []Key, admissions []Admission)

// Admission is whether a predicate keeps a listed key or passes over it.
type Admission int32

const (
	// Reject passes over the key.
	Reject Admission = C.ustore_reject_k
	// Admit keeps the key among the listed ones.
	Admit Admission = C.ustore_admit_k
)

//export ustoreAdmits
func ustoreAdmits(keys *C.ustore_key_t, admissions *C.ustore_admission_t, count C.ustore_size_t, state unsafe.Pointer) {
	asked := unsafe.Slice((*Key)(unsafe.Pointer(keys)), count)
	verdicts := unsafe.Slice((*Admission)(unsafe.Pointer(admissions)), count)
	switch predicate := (*cgo.Handle)(state).Value().(type) {
	case Predicate:
		for index, key := range asked {
			if predicate(key) {
				verdicts[index] = Admit
			}
		}
	case BatchPredicate:
		predicate(asked, verdicts)
	case CheapBatchPredicate:
		predicate(asked, verdicts)
	}
}

func (predicate Predicate) admitter(handle *cgo.Handle) C.ustore_predicate_t {
	if predicate == nil {
		return C.ustore_predicate_t{}
	}
	return admits(handle, predicate, C.ustore_predicate_cost_cheap_k)
}

func (predicate BatchPredicate) admitter(handle *cgo.Handle) C.ustore_predicate_t {
	if predicate == nil {
		return C.ustore_predicate_t{}
	}
	return admits(handle, predicate, C.ustore_predicate_cost_expensive_k)
}

func (predicate CheapBatchPredicate) admitter(handle *cgo.Handle) C.ustore_predicate_t {
	if predicate == nil {
		return C.ustore_predicate_t{}
	}
	return admits(handle, predicate, C.ustore_predicate_cost_cheap_k)
}

// admits lends handler to the C trampoline through handle, at the cost its type names.
func admits(handle *cgo.Handle, handler any, cost C.ustore_predicate_cost_t) C.ustore_predicate_t {
	*handle = cgo.NewHandle(handler)
	return C.ustore_predicate_t{callback: C.ustore_go_admits(), state: unsafe.Pointer(handle), cost: cost}
}

// predicate lends an optional predicate as the structure a filtering verb takes. It hands the
// handle slot rather than the call through the interface, so the call stays off the heap.
func (call *call) predicate(admitter Admitter) C.ustore_predicate_t {
	if admitter == nil {
		return C.ustore_predicate_t{}
	}
	return admitter.admitter(&call.slots.handles[0])
}

// endregion: Predicates

// region: Helpers

// keys lends a packed key slice as the strided run the library reads, zeroed when empty.
func (call *call) keys(keys []Key) C.ustore_keys_view_t {
	if len(keys) == 0 {
		return C.ustore_keys_view_t{}
	}
	return C.ustore_keys_view_t{data: (*C.ustore_key_t)(call.lend(unsafe.Pointer(unsafe.SliceData(keys)), len(keys))), stride: C.ustore_size_t(unsafe.Sizeof(Key{}))}
}

// key lends one key by address from the frame's inputs, or nil for none.
func (call *call) key(inputs *inputs, key *Key) *C.ustore_key_t {
	if key == nil {
		return nil
	}
	inputs.key = raw(*key)
	return &inputs.key
}

// lengths lends a run of lengths, zeroed when empty.
func (call *call) lengths(lengths []uint64) C.ustore_lengths_view_t {
	if len(lengths) == 0 {
		return C.ustore_lengths_view_t{}
	}
	return C.ustore_lengths_view_t{data: (*C.ustore_length_t)(call.lend(unsafe.Pointer(unsafe.SliceData(lengths)), len(lengths))), stride: 8}
}

// collections lends the collection every task shares from the frame's inputs, or one per task in
// place.
func (call *call) collections(inputs *inputs, collections Collections) C.ustore_collections_view_t {
	switch collections := collections.(type) {
	case CollectionID:
		inputs.collection = rawCollection(collections)
		return C.ustore_collections_view_t{data: &inputs.collection}
	case Each:
		return C.ustore_collections_view_t{
			data:   (*C.ustore_collection_id_t)(call.lend(unsafe.Pointer(unsafe.SliceData(collections)), len(collections))),
			stride: C.ustore_size_t(unsafe.Sizeof(CollectionID{})),
		}
	}
	return C.ustore_collections_view_t{}
}

// raw reinterprets a key as the C structure, which holds the same sixteen bytes.
func raw(key Key) C.ustore_key_t { return *(*C.ustore_key_t)(unsafe.Pointer(&key)) }

// rawCollection reinterprets a collection identifier as the C structure.
func rawCollection(collection CollectionID) C.ustore_collection_id_t {
	return *(*C.ustore_collection_id_t)(unsafe.Pointer(&collection))
}

// present reads bit index of a bitset a call wrote into its arena, least significant bit first.
func present(bits *C.ustore_octet_t, index uint) bool {
	if bits == nil {
		return false
	}
	return *(*byte)(unsafe.Add(unsafe.Pointer(bits), index/8))&(1<<(index%8)) != 0
}

// at reads element index of an array a call wrote into its arena.
func at[Value any](first *Value, index uint) Value {
	return *(*Value)(unsafe.Add(unsafe.Pointer(first), uintptr(index)*unsafe.Sizeof(*first)))
}

// keysAt borrows count keys a call wrote into its arena from position begin.
func keysAt(first *C.ustore_key_t, begin, end uint64) []Key {
	if first == nil || begin == end {
		return nil
	}
	return unsafe.Slice((*Key)(unsafe.Add(unsafe.Pointer(first), uintptr(begin)*16)), end-begin)
}

// endregion: Helpers

// region: Receivers

// storeReceiver is a parent's handle field, so a Close is seen, and the executor its verbs run on,
// beside the database every verb names and the watch its reads and writes record keys under.
type storeReceiver[Handle comparable] struct {
	receiver[Handle]
	database C.ustore_database_t
	watch    Watch
}

// lower reports the handles a verb hands across, refusing a closed parent before C dereferences it.
func (receiver storeReceiver[Handle]) lower() (C.ustore_database_t, C.ustore_executor_t, error) {
	_, executor, err := receiver.receiver.lower()
	return receiver.database, executor, err
}

// lowerInto is lower for a verb writing into arena, refusing a closed one too.
func (receiver storeReceiver[Handle]) lowerInto(arena *Arena) (C.ustore_database_t, C.ustore_executor_t, C.ustore_arena_t, error) {
	_, executor, outputs, err := receiver.receiver.lowerInto(arena)
	return receiver.database, executor, outputs, err
}

// view names what a read sees: the head, the transaction under its watch, or the snapshot.
func (receiver storeReceiver[Handle]) view() C.ustore_view_t {
	switch handle := any(*receiver.handle).(type) {
	case C.ustore_transaction_t:
		return C.ustore_transaction_view(handle, C.ustore_watch_t(receiver.watch))
	case C.ustore_snapshot_t:
		return C.ustore_snapshot_view(handle)
	}
	return nil
}

// transaction names the transaction a write joins, nil at the head.
func (receiver storeReceiver[Handle]) transaction() C.ustore_transaction_t {
	transaction, _ := any(*receiver.handle).(C.ustore_transaction_t)
	return transaction
}

// snapshot names the snapshot whose collections a lookup reads, nil elsewhere.
func (receiver storeReceiver[Handle]) snapshot() C.ustore_snapshot_t {
	snapshot, _ := any(*receiver.handle).(C.ustore_snapshot_t)
	return snapshot
}

// reads carries the blocking verbs every parent offers.
type reads[Handle comparable] struct{ storeReceiver[Handle] }

// deferredReads carries the deferred twins of reads.
type deferredReads[Handle comparable] struct{ storeReceiver[Handle] }

// writes carries the blocking verbs a database and a transaction offer, which a snapshot does not.
type writes[Handle comparable] struct{ reads[Handle] }

// deferredWrites carries the deferred twins of writes.
type deferredWrites[Handle comparable] struct{ deferredReads[Handle] }

// DatabaseOn is the receiver whose verbs block until their call is final, reading and writing the
// head: on the calling goroutine when embedded in a [Database], and on an executor when
// [DatabaseOn.On] makes it.
type DatabaseOn struct {
	writes[C.ustore_database_t]
}

// DatabaseDeferredOn is the receiver whose verbs submit their calls to an executor and answer a
// [Deferred], offering the verbs of [DatabaseOn] with the same arguments.
//
// The library reads a deferred call's inputs until the call is final, so the slices a verb is
// handed must not change until then; its arena is borrowed, as a blocking verb borrows it.
type DatabaseDeferredOn struct {
	deferredWrites[C.ustore_database_t]
}

// On is the receiver whose verbs block on executor.
func (on DatabaseOn) On(executor *Executor) DatabaseOn {
	on.executor = executor
	return on
}

// Deferred is the receiver whose verbs submit their calls to executor.
func (on DatabaseOn) Deferred(executor *Executor) DatabaseDeferredOn {
	on.executor = executor
	return DatabaseDeferredOn{deferredWrites[C.ustore_database_t]{deferredReads[C.ustore_database_t]{on.storeReceiver}}}
}

// TransactionOn is the receiver whose verbs block until their call is final, reading and writing
// inside a transaction under one [Watch].
type TransactionOn struct {
	writes[C.ustore_transaction_t]
}

// TransactionDeferredOn is the receiver whose verbs submit their calls to an executor, offering the
// verbs of [TransactionOn] with the same arguments.
type TransactionDeferredOn struct {
	deferredWrites[C.ustore_transaction_t]
}

// On is the receiver whose verbs block on executor, under the same watch.
func (on TransactionOn) On(executor *Executor) TransactionOn {
	on.executor = executor
	return on
}

// Deferred is the receiver whose verbs submit their calls to executor, under the same watch.
func (on TransactionOn) Deferred(executor *Executor) TransactionDeferredOn {
	on.executor = executor
	return TransactionDeferredOn{deferredWrites[C.ustore_transaction_t]{deferredReads[C.ustore_transaction_t]{on.storeReceiver}}}
}

// SnapshotOn is the receiver whose verbs block until their call is final, reading the state a
// snapshot pinned; nothing writes through it.
type SnapshotOn struct {
	reads[C.ustore_snapshot_t]
}

// SnapshotDeferredOn is the receiver whose verbs submit their calls to an executor, offering the
// verbs of [SnapshotOn] with the same arguments.
type SnapshotDeferredOn struct {
	deferredReads[C.ustore_snapshot_t]
}

// On is the receiver whose verbs block on executor.
func (on SnapshotOn) On(executor *Executor) SnapshotOn {
	on.executor = executor
	return on
}

// Deferred is the receiver whose verbs submit their calls to executor.
func (on SnapshotOn) Deferred(executor *Executor) SnapshotDeferredOn {
	on.executor = executor
	return SnapshotDeferredOn{deferredReads[C.ustore_snapshot_t]{on.storeReceiver}}
}

// endregion: Receivers

// region: Database

// Database is an open database, safe to share across goroutines from open until [Database.Close].
// Its own verbs read and write the head on the calling goroutine; [DatabaseOn.On] and
// [DatabaseOn.Deferred] pick an executor.
type Database struct {
	DatabaseOn
	handle C.ustore_database_t
}

// databaseOf wraps the handle an open wrote.
func databaseOf(handle C.ustore_database_t, err error) (*Database, error) {
	if err != nil {
		return nil, err
	}
	database := &Database{handle: handle}
	database.storeReceiver = storeReceiver[C.ustore_database_t]{receiver: receiver[C.ustore_database_t]{handle: &database.handle}, database: handle}
	return database, nil
}

// Close releases the database once every transaction, snapshot, cursor and ingest holding it is
// closed, waiting for every deferred call naming it. It is idempotent, and a closed database must
// not be used again.
func (database *Database) Close() error {
	if database == nil || database.handle == nil {
		return nil
	}
	C.ustore_database_free(database.handle)
	database.handle = nil
	return nil
}

// DatabaseInitRamConfig carries the optional arguments of [DatabaseInitRam] and [DatabaseOpenRam].
type DatabaseInitRamConfig struct {
	// Executor runs the open, nil the calling goroutine.
	Executor *Executor
	// Isolation is how transactions see each other's writes.
	Isolation Isolation
	// CPUClassCeiling is the newest class the database's kernels may run as; zero sets no ceiling.
	CPUClassCeiling CPUClass
}

// DatabaseInitRam starts an empty database in memory alone.
func DatabaseInitRam(config *DatabaseInitRamConfig) (*Database, error) {
	options := configOf(config)
	executor, err := options.Executor.lower()
	if err != nil {
		return nil, err
	}
	var lent lent[C.ustore_database_t]
	arguments := open[C.ustore_database_init_ram_t](&lent)
	*arguments = C.ustore_database_init_ram_t{
		database:          lent.result,
		executor:          executor,
		outcome:           lent.outcome(),
		isolation:         C.ustore_isolation_t(options.Isolation),
		cpu_class_ceiling: C.ustore_cpu_class_t(options.CPUClassCeiling),
	}
	C.ustore_database_init_ram(arguments)
	return databaseOf(block(&lent, nil))
}

// DatabaseOpenRam serves directory as one in-memory image, loading it if present.
func DatabaseOpenRam(directory string, config *DatabaseInitRamConfig) (*Database, error) {
	options := configOf(config)
	executor, err := options.Executor.lower()
	if err != nil {
		return nil, err
	}
	var lent lent[C.ustore_database_t]
	arguments := open[C.ustore_database_open_ram_t](&lent)
	*arguments = C.ustore_database_open_ram_t{
		database:          lent.result,
		executor:          executor,
		outcome:           lent.outcome(),
		directory:         lent.text(directory),
		isolation:         C.ustore_isolation_t(options.Isolation),
		cpu_class_ceiling: C.ustore_cpu_class_t(options.CPUClassCeiling),
	}
	C.ustore_database_open_ram(arguments)
	return databaseOf(block(&lent, nil))
}

// DataDirectory is a directory a database spreads its files over, with the bytes they may take
// in it.
type DataDirectory struct {
	// Path is the directory, which must exist.
	Path string
	// MaxBytes caps the bytes the files may take there, zero being unlimited.
	MaxBytes uint64
}

// directories lends the data directories of an open, each path borrowed rather than copied.
func (call *call) directories(directories []DataDirectory) (*C.ustore_data_directory_t, C.ustore_size_t) {
	if len(directories) == 0 {
		return nil, 0
	}
	lowered := make([]C.ustore_data_directory_t, len(directories))
	for index, directory := range directories {
		lowered[index] = C.ustore_data_directory_t{path: call.text(directory.Path), max_bytes: C.ustore_size_t(directory.MaxBytes)}
	}
	return (*C.ustore_data_directory_t)(call.lend(unsafe.Pointer(unsafe.SliceData(lowered)), len(lowered))), C.ustore_size_t(len(lowered))
}

// DatabaseOpenRocksdbConfig carries the optional arguments of [DatabaseOpenRocksdb] and
// [DatabaseViewRocksdb].
type DatabaseOpenRocksdbConfig struct {
	// Executor runs the open, nil the calling goroutine.
	Executor *Executor
	// DataDirectories spread the files beyond the directory.
	DataDirectories []DataDirectory
}

// DatabaseOpenRocksdb serves a RocksDB directory in place, creating it if absent.
func DatabaseOpenRocksdb(directory string, config *DatabaseOpenRocksdbConfig) (*Database, error) {
	options := configOf(config)
	executor, err := options.Executor.lower()
	if err != nil {
		return nil, err
	}
	var lent lent[C.ustore_database_t]
	arguments := open[C.ustore_database_open_rocksdb_t](&lent)
	directories, count := lent.directories(options.DataDirectories)
	*arguments = C.ustore_database_open_rocksdb_t{
		database:               lent.result,
		executor:               executor,
		outcome:                lent.outcome(),
		directory:              lent.text(directory),
		data_directories:       directories,
		data_directories_count: count,
	}
	C.ustore_database_open_rocksdb(arguments)
	return databaseOf(block(&lent, nil))
}

// DatabaseViewRocksdb serves a RocksDB directory read-only, refusing every change with
// [StatusReadOnly].
func DatabaseViewRocksdb(directory string, config *DatabaseOpenRocksdbConfig) (*Database, error) {
	options := configOf(config)
	executor, err := options.Executor.lower()
	if err != nil {
		return nil, err
	}
	var lent lent[C.ustore_database_t]
	arguments := open[C.ustore_database_view_rocksdb_t](&lent)
	directories, count := lent.directories(options.DataDirectories)
	*arguments = C.ustore_database_view_rocksdb_t{
		database:               lent.result,
		executor:               executor,
		outcome:                lent.outcome(),
		directory:              lent.text(directory),
		data_directories:       directories,
		data_directories_count: count,
	}
	C.ustore_database_view_rocksdb(arguments)
	return databaseOf(block(&lent, nil))
}

// DatabaseOpenLeveldb serves a LevelDB directory in place, creating it if absent, opened on
// executor or, for nil, the calling goroutine.
func DatabaseOpenLeveldb(directory string, executor *Executor) (*Database, error) {
	handle, err := executor.lower()
	if err != nil {
		return nil, err
	}
	var lent lent[C.ustore_database_t]
	arguments := open[C.ustore_database_open_leveldb_t](&lent)
	*arguments = C.ustore_database_open_leveldb_t{
		database:  lent.result,
		executor:  handle,
		outcome:   lent.outcome(),
		directory: lent.text(directory),
	}
	C.ustore_database_open_leveldb(arguments)
	return databaseOf(block(&lent, nil))
}

// DatabaseOpenNvmeConfig carries the optional arguments of [DatabaseOpenNvme], where every zero
// size or count keeps the engine's default.
type DatabaseOpenNvmeConfig struct {
	// Executor runs the open, nil the calling goroutine; an NVMe executor moves the bytes.
	Executor *Executor
	// DataDirectories spread the volumes beyond the directory.
	DataDirectories []DataDirectory
	// Compactions is the executor merges run on.
	Compactions *Executor
	// MemoryLimit caps the bytes the engine keeps in memory.
	MemoryLimit uint64
	// TransactionMaxBytes caps the bytes one transaction buffers.
	TransactionMaxBytes uint64
	// TransactionMaxDeltas caps the changes one transaction buffers.
	TransactionMaxDeltas uint64
	// CollectionConcurrencyLimit caps the calls one collection serves at once.
	CollectionConcurrencyLimit uint64
	// CollectionCacheLimit caps the bytes one collection caches.
	CollectionCacheLimit uint64
	// CollectionMemoryRunsMaxBytes caps the bytes of one collection's memory runs.
	CollectionMemoryRunsMaxBytes uint64
	// CollectionMemoryRunsMaxDeltas caps the changes of one collection's memory runs.
	CollectionMemoryRunsMaxDeltas uint64
	// CollectionLevelEnlargeFactor is how much larger each level grows than the one above it.
	CollectionLevelEnlargeFactor uint64
	// CollectionValueSize is the value size the collections are tuned for.
	CollectionValueSize uint64
	// CollectionValueMaxSize caps the size of one value.
	CollectionValueMaxSize uint64
	// CPUClassCeiling is the newest class the database's kernels may run as; zero sets no ceiling.
	CPUClassCeiling CPUClass
}

// DatabaseOpenNvme serves a directory through the in-tree NVMe engine.
func DatabaseOpenNvme(directory string, config *DatabaseOpenNvmeConfig) (*Database, error) {
	options := configOf(config)
	executor, err := options.Executor.lower()
	if err != nil {
		return nil, err
	}
	compactions, err := options.Compactions.lower()
	if err != nil {
		return nil, err
	}
	var lent lent[C.ustore_database_t]
	arguments := open[C.ustore_database_open_nvme_t](&lent)
	directories, count := lent.directories(options.DataDirectories)
	*arguments = C.ustore_database_open_nvme_t{
		database:                          lent.result,
		executor:                          executor,
		outcome:                           lent.outcome(),
		directory:                         lent.text(directory),
		data_directories:                  directories,
		data_directories_count:            count,
		compactions:                       compactions,
		memory_limit:                      C.ustore_size_t(options.MemoryLimit),
		transaction_max_bytes:             C.ustore_size_t(options.TransactionMaxBytes),
		transaction_max_deltas:            C.ustore_size_t(options.TransactionMaxDeltas),
		collection_concurrency_limit:      C.ustore_size_t(options.CollectionConcurrencyLimit),
		collection_cache_limit:            C.ustore_size_t(options.CollectionCacheLimit),
		collection_memory_runs_max_bytes:  C.ustore_size_t(options.CollectionMemoryRunsMaxBytes),
		collection_memory_runs_max_deltas: C.ustore_size_t(options.CollectionMemoryRunsMaxDeltas),
		collection_level_enlarge_factor:   C.ustore_size_t(options.CollectionLevelEnlargeFactor),
		collection_value_size:             C.ustore_size_t(options.CollectionValueSize),
		collection_value_max_size:         C.ustore_size_t(options.CollectionValueMaxSize),
		cpu_class_ceiling:                 C.ustore_cpu_class_t(options.CPUClassCeiling),
	}
	C.ustore_database_open_nvme(arguments)
	return databaseOf(block(&lent, nil))
}

// DatabaseDescription is what [DatabaseOn.Describe] reports, laid out as
// ustore_database_description_t.
type DatabaseDescription struct {
	// Capabilities are the features the engine supports.
	Capabilities Capabilities
	// Engine is the engine serving the database.
	Engine Engine
	// Isolation is how transactions see each other.
	Isolation Isolation
	// CPUClass is the class the engine's calls run as, picked at open.
	CPUClass CPUClass
	// Keys counts the keys across every collection.
	Keys uint64
	// MemoryBytes is the memory the engine holds for the data.
	MemoryBytes uint64
	// DiskBytes is what the files of the database take on disk.
	DiskBytes uint64
	// PinRefusedBytes is the NVMe engine's io_uring arena the locked-memory cap refused to pin.
	PinRefusedBytes uint64
	// CrossingRequests counts the requests the NVMe engine sent unpinned while its arena was pinned.
	CrossingRequests uint64
}

// The library writes the description in place, so the two layouts must agree.
var _ = [1]struct{}{}[unsafe.Sizeof(DatabaseDescription{})-unsafe.Sizeof(C.ustore_database_description_t{})]

// Describe reports the engine's capabilities, isolation, CPU class and the sizes it estimates.
func (on DatabaseOn) Describe() (DatabaseDescription, error) {
	var lent lent[DatabaseDescription]
	return block(&lent, on.describe(&lent, C.ustore_completion_blocking_k))
}

// Describe submits [DatabaseOn.Describe].
func (on DatabaseDeferredOn) Describe() (*Deferred[DatabaseDescription], error) {
	deferred := new(Deferred[DatabaseDescription])
	return submitted(deferred, on.describe(&deferred.lent, C.ustore_completion_deferred_k))
}

func (receiver storeReceiver[Handle]) describe(lent *lent[DatabaseDescription], completion C.ustore_completion_t) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	arguments := open[C.ustore_database_describe_t](lent)
	*arguments = C.ustore_database_describe_t{
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
		completion:  completion,
		description: (*C.ustore_database_description_t)(unsafe.Pointer(lent.result)),
	}
	C.ustore_database_describe(arguments)
	return nil
}

// Clear empties every collection at once, as mode says.
func (on DatabaseOn) Clear(mode DropMode) error {
	var lent lent[struct{}]
	_, err := block(&lent, on.clear(&lent, C.ustore_completion_blocking_k, mode))
	return err
}

// Clear submits [DatabaseOn.Clear].
func (on DatabaseDeferredOn) Clear(mode DropMode) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.clear(&deferred.lent, C.ustore_completion_deferred_k, mode))
}

func (receiver storeReceiver[Handle]) clear(lent *lent[struct{}], completion C.ustore_completion_t, mode DropMode) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	arguments := open[C.ustore_database_clear_t](lent)
	*arguments = C.ustore_database_clear_t{
		database:   database,
		executor:   executor,
		outcome:    lent.outcome(),
		completion: completion,
		mode:       C.ustore_drop_mode_t(mode),
	}
	C.ustore_database_clear(arguments)
	return nil
}

// CompactPlan is a compaction kept live across calls: the handle the library opens on the first
// call naming it, and where the last such call stopped.
type CompactPlan struct {
	// Phase is where the last call naming the plan stopped, [PhaseDone] once the compaction ended.
	Phase  PhaseProgress
	handle C.ustore_database_compact_plan_t
}

// CompactConfig carries the optional arguments of [DatabaseOn.Compact].
type CompactConfig struct {
	// Plan keeps a compaction live across calls on an executor; nil runs it to its end.
	Plan *CompactPlan
	// Progress is called at every barrier of every phase, and may cancel at that barrier.
	Progress ProgressCallback
}

// Compact reclaims the space that overwritten and removed data still takes, through the phases
// its engine runs.
func (on DatabaseOn) Compact(config *CompactConfig) error {
	var lent lent[struct{}]
	_, err := block(&lent, on.compact(&lent, C.ustore_completion_blocking_k, config))
	return err
}

// Compact submits [DatabaseOn.Compact].
func (on DatabaseDeferredOn) Compact(config *CompactConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.compact(&deferred.lent, C.ustore_completion_deferred_k, config))
}

func (receiver storeReceiver[Handle]) compact(lent *lent[struct{}], completion C.ustore_completion_t, config *CompactConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	options := configOf(config)
	arguments := open[C.ustore_database_compact_t](lent)
	progress, state := lent.progress(options.Progress)
	*arguments = C.ustore_database_compact_t{
		database:       database,
		executor:       executor,
		outcome:        lent.outcome(),
		completion:     completion,
		progress:       progress,
		progress_state: state,
	}
	if options.Plan != nil {
		plan := (*CompactPlan)(lent.pin(unsafe.Pointer(options.Plan)))
		arguments.plan = &plan.handle
		arguments.phase = (*C.ustore_phase_progress_t)(unsafe.Pointer(&plan.Phase))
	}
	C.ustore_database_compact(arguments)
	return nil
}

// CompactPlanFinish frees a plan whatever the call answers, so another compaction may open on the
// database, leaving the plan fresh.
func (on DatabaseOn) CompactPlanFinish(plan *CompactPlan) error {
	var lent lent[struct{}]
	_, err := block(&lent, on.compactPlanFinish(&lent, C.ustore_completion_blocking_k, plan))
	return err
}

// CompactPlanFinish submits [DatabaseOn.CompactPlanFinish].
func (on DatabaseDeferredOn) CompactPlanFinish(plan *CompactPlan) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.compactPlanFinish(&deferred.lent, C.ustore_completion_deferred_k, plan))
}

func (receiver storeReceiver[Handle]) compactPlanFinish(lent *lent[struct{}], completion C.ustore_completion_t, plan *CompactPlan) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	if plan == nil {
		return fail(StatusInvalidArgument, "pass the plan to finish")
	}
	arguments := open[C.ustore_database_compact_plan_finish_t](lent)
	*arguments = C.ustore_database_compact_plan_finish_t{
		database:   database,
		executor:   executor,
		outcome:    lent.outcome(),
		completion: completion,
		plan:       plan.handle,
	}
	C.ustore_database_compact_plan_finish(arguments)
	*plan = CompactPlan{}
	return nil
}

// Flush makes every buffered write durable.
func (on DatabaseOn) Flush() error {
	var lent lent[struct{}]
	_, err := block(&lent, on.flush(&lent, C.ustore_completion_blocking_k))
	return err
}

// Flush submits [DatabaseOn.Flush].
func (on DatabaseDeferredOn) Flush() (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.flush(&deferred.lent, C.ustore_completion_deferred_k))
}

func (receiver storeReceiver[Handle]) flush(lent *lent[struct{}], completion C.ustore_completion_t) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	arguments := open[C.ustore_database_flush_t](lent)
	*arguments = C.ustore_database_flush_t{
		database:   database,
		executor:   executor,
		outcome:    lent.outcome(),
		completion: completion,
	}
	C.ustore_database_flush(arguments)
	return nil
}

// endregion: Database

// region: Snapshots

// Snapshot pins the state it was taken at until [Snapshot.Close], and its own verbs read that state
// on the calling goroutine; nothing writes through it.
type Snapshot struct {
	SnapshotOn
	handle    C.ustore_snapshot_t
	readStamp ReadStamp
}

// snapshotInit is what a snapshot's init writes: the handle and the stamp it reads at.
type snapshotInit struct {
	handle C.ustore_snapshot_t
	stamp  C.ustore_stamp_t
}

// SnapshotInit pins the head as it stands, blocking whatever receiver it is called on.
func (on DatabaseOn) SnapshotInit() (*Snapshot, error) {
	database, executor, err := on.lower()
	if err != nil {
		return nil, err
	}
	var lent lent[snapshotInit]
	arguments := open[C.ustore_snapshot_init_t](&lent)
	*arguments = C.ustore_snapshot_init_t{
		snapshot:   &lent.result.handle,
		database:   database,
		executor:   executor,
		outcome:    lent.outcome(),
		read_stamp: &lent.result.stamp,
	}
	C.ustore_snapshot_init(arguments)
	initialized, err := block(&lent, nil)
	if err != nil {
		return nil, err
	}
	snapshot := &Snapshot{handle: initialized.handle, readStamp: ReadStamp(initialized.stamp)}
	snapshot.storeReceiver = storeReceiver[C.ustore_snapshot_t]{receiver: receiver[C.ustore_snapshot_t]{handle: &snapshot.handle}, database: database}
	return snapshot, nil
}

// ReadStamp reports the stamp the snapshot reads at, which every commit at or below it shows.
func (snapshot *Snapshot) ReadStamp() ReadStamp { return snapshot.readStamp }

// Close releases the snapshot once every deferred call naming it is final, unpinning the versions
// only it kept. It is idempotent.
func (snapshot *Snapshot) Close() error {
	if snapshot == nil || snapshot.handle == nil {
		return nil
	}
	C.ustore_snapshot_free(snapshot.handle)
	snapshot.handle = nil
	return nil
}

// Save writes the snapshot out as a new database of the same engine at path, holding exactly the
// collections and names it records; a cancelled save removes what it wrote.
func (on SnapshotOn) Save(path string, progress ProgressCallback) error {
	var lent lent[struct{}]
	_, err := block(&lent, on.save(&lent, C.ustore_completion_blocking_k, path, progress))
	return err
}

// Save submits [SnapshotOn.Save].
func (on SnapshotDeferredOn) Save(path string, progress ProgressCallback) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.save(&deferred.lent, C.ustore_completion_deferred_k, path, progress))
}

func (receiver storeReceiver[Handle]) save(lent *lent[struct{}], completion C.ustore_completion_t, path string, progress ProgressCallback) error {
	_, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	arguments := open[C.ustore_snapshot_save_t](lent)
	callback, state := lent.progress(progress)
	*arguments = C.ustore_snapshot_save_t{
		snapshot:       receiver.snapshot(),
		executor:       executor,
		outcome:        lent.outcome(),
		completion:     completion,
		path:           lent.text(path),
		progress:       callback,
		progress_state: state,
	}
	C.ustore_snapshot_save(arguments)
	return nil
}

// endregion: Snapshots

// region: Collections

// CollectionCreate creates a collection under name holding modality; a name starting with an
// underscore is refused, as the engines keep their own collections there.
func (on DatabaseOn) CollectionCreate(name string, modality Modality) (CollectionID, error) {
	var lent lent[CollectionID]
	return block(&lent, on.collectionCreate(&lent, C.ustore_completion_blocking_k, name, modality))
}

// CollectionCreate submits [DatabaseOn.CollectionCreate].
func (on DatabaseDeferredOn) CollectionCreate(name string, modality Modality) (*Deferred[CollectionID], error) {
	deferred := new(Deferred[CollectionID])
	return submitted(deferred, on.collectionCreate(&deferred.lent, C.ustore_completion_deferred_k, name, modality))
}

func (receiver storeReceiver[Handle]) collectionCreate(lent *lent[CollectionID], completion C.ustore_completion_t, name string, modality Modality) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	arguments := open[C.ustore_collection_create_t](lent)
	*arguments = C.ustore_collection_create_t{
		database:   database,
		executor:   executor,
		outcome:    lent.outcome(),
		completion: completion,
		name:       lent.text(name),
		modality:   C.ustore_modality_t(modality),
		collection: (*C.ustore_collection_id_t)(unsafe.Pointer(lent.result)),
	}
	C.ustore_collection_create(arguments)
	return nil
}

// CollectionDrop empties a collection of its pairs or values, or drops it whole, as mode says.
func (on DatabaseOn) CollectionDrop(collection CollectionID, mode DropMode) error {
	var lent lent[struct{}]
	_, err := block(&lent, on.collectionDrop(&lent, C.ustore_completion_blocking_k, collection, mode))
	return err
}

// CollectionDrop submits [DatabaseOn.CollectionDrop].
func (on DatabaseDeferredOn) CollectionDrop(collection CollectionID, mode DropMode) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.collectionDrop(&deferred.lent, C.ustore_completion_deferred_k, collection, mode))
}

func (receiver storeReceiver[Handle]) collectionDrop(lent *lent[struct{}], completion C.ustore_completion_t, collection CollectionID, mode DropMode) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	arguments := open[C.ustore_collection_drop_t](lent)
	*arguments = C.ustore_collection_drop_t{
		database:   database,
		executor:   executor,
		outcome:    lent.outcome(),
		completion: completion,
		collection: rawCollection(collection),
		mode:       C.ustore_drop_mode_t(mode),
	}
	C.ustore_collection_drop(arguments)
	return nil
}

// CollectionList is every collection a call listed into its arena, readable until the arena serves
// another call.
type CollectionList struct {
	collections *C.ustore_collection_id_t
	modalities  *C.ustore_modality_t
	names       Strings
	written     C.ustore_size_t
}

// Len reports how many collections the call listed.
func (list CollectionList) Len() uint { return uint(list.written) }

// ID reports the identifier of collection index.
func (list CollectionList) ID(index uint) CollectionID {
	return at((*CollectionID)(unsafe.Pointer(list.collections)), index)
}

// Modality reports what collection index holds.
func (list CollectionList) Modality(index uint) Modality { return Modality(at(list.modalities, index)) }

// Name borrows the name of collection index from the arena.
func (list CollectionList) Name(index uint) []byte {
	return list.names.sized(uint(list.written)).Get(index)
}

// CollectionList lists every collection the receiver sees, with its modality and name, into arena.
func (on reads[Handle]) CollectionList(arena *Arena) (CollectionList, error) {
	var lent lent[CollectionList]
	return block(&lent, on.collectionList(&lent, C.ustore_completion_blocking_k, arena))
}

// CollectionList submits [DatabaseOn.CollectionList].
func (on deferredReads[Handle]) CollectionList(arena *Arena) (*Deferred[CollectionList], error) {
	deferred := new(Deferred[CollectionList])
	return submitted(deferred, on.collectionList(&deferred.lent, C.ustore_completion_deferred_k, arena))
}

func (receiver storeReceiver[Handle]) collectionList(lent *lent[CollectionList], completion C.ustore_completion_t, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	arguments := open[C.ustore_collection_list_t](lent)
	*arguments = C.ustore_collection_list_t{
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
		completion:  completion,
		snapshot:    receiver.snapshot(),
		collections: &lent.result.collections,
		modalities:  &lent.result.modalities,
		names:       &lent.result.names.raw,
		written:     &lent.result.written,
		arena:       outputs,
	}
	C.ustore_collection_list(arguments)
	return nil
}

// FoundCollections is what a lookup of names found, in the order of the names, readable until the
// arena serves another call.
type FoundCollections struct {
	presences   *C.ustore_octet_t
	collections *C.ustore_collection_id_t
	modalities  *C.ustore_modality_t
	count       uint
}

// Len reports how many names the call looked up.
func (found FoundCollections) Len() uint { return found.count }

// Get reports the collection name index addresses and what it holds, and false when no collection
// carries the name.
func (found FoundCollections) Get(index uint) (CollectionID, Modality, bool) {
	if !present(found.presences, index) {
		return CollectionID{}, 0, false
	}
	return at((*CollectionID)(unsafe.Pointer(found.collections)), index), Modality(at(found.modalities, index)), true
}

// CollectionFind resolves a batch of names into the collections they address, into arena.
func (on reads[Handle]) CollectionFind(names Tape, arena *Arena) (FoundCollections, error) {
	var lent lent[FoundCollections]
	return block(&lent, on.collectionFind(&lent, C.ustore_completion_blocking_k, names, arena))
}

// CollectionFind submits [DatabaseOn.CollectionFind].
func (on deferredReads[Handle]) CollectionFind(names Tape, arena *Arena) (*Deferred[FoundCollections], error) {
	deferred := new(Deferred[FoundCollections])
	return submitted(deferred, on.collectionFind(&deferred.lent, C.ustore_completion_deferred_k, names, arena))
}

func (receiver storeReceiver[Handle]) collectionFind(lent *lent[FoundCollections], completion C.ustore_completion_t, names Tape, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	arguments := open[C.ustore_collection_find_t](lent)
	lent.result.count = names.Len()
	*arguments = C.ustore_collection_find_t{
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
		completion:  completion,
		snapshot:    receiver.snapshot(),
		count:       C.ustore_size_t(names.Len()),
		names:       lent.strings(names),
		presences:   &lent.result.presences,
		collections: &lent.result.collections,
		modalities:  &lent.result.modalities,
		arena:       outputs,
	}
	C.ustore_collection_find(arguments)
	return nil
}

// endregion: Collections

// region: Transactions

// Transaction groups reads and writes across collections and modalities, and commits all of them or
// none. Its own verbs run on the calling goroutine and watch the keys they touch, while
// [Transaction.View] picks another watch. It serves one call at a time and holds its database open
// until [Transaction.Close].
type Transaction struct {
	TransactionOn
	handle C.ustore_transaction_t
}

// TransactionInit begins a transaction on the head, blocking whatever receiver it is called on.
func (on DatabaseOn) TransactionInit() (*Transaction, error) {
	database, executor, err := on.lower()
	if err != nil {
		return nil, err
	}
	var lent lent[C.ustore_transaction_t]
	arguments := open[C.ustore_transaction_init_t](&lent)
	*arguments = C.ustore_transaction_init_t{
		transaction: lent.result,
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
	}
	C.ustore_transaction_init(arguments)
	handle, err := block(&lent, nil)
	if err != nil {
		return nil, err
	}
	transaction := &Transaction{handle: handle}
	transaction.storeReceiver = storeReceiver[C.ustore_transaction_t]{receiver: receiver[C.ustore_transaction_t]{handle: &transaction.handle}, database: database}
	return transaction, nil
}

// View is the receiver whose reads and writes, on the calling goroutine, record their keys under
// watch.
func (transaction *Transaction) View(watch Watch) TransactionOn {
	on := transaction.TransactionOn
	on.watch = watch
	return on
}

// Close releases the transaction once every deferred call naming it is final, dropping whatever it
// did not commit. It is idempotent.
func (transaction *Transaction) Close() error {
	if transaction == nil || transaction.handle == nil {
		return nil
	}
	C.ustore_transaction_free(transaction.handle)
	transaction.handle = nil
	return nil
}

// Reset reopens the transaction on the newest state for another try, dropping its contents.
func (on TransactionOn) Reset() error {
	database, executor, err := on.lower()
	if err != nil {
		return err
	}
	var lent lent[C.ustore_transaction_t]
	arguments := open[C.ustore_transaction_init_t](&lent)
	*lent.result = *on.handle
	*arguments = C.ustore_transaction_init_t{
		transaction: lent.result,
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
	}
	C.ustore_transaction_init(arguments)
	handle, err := block(&lent, nil)
	if err != nil {
		return err
	}
	*on.handle = handle
	return nil
}

// Stage validates the transaction and holds its keys, answering the stamp it reads at and its
// commit validates against.
func (on TransactionOn) Stage() (ReadStamp, error) {
	var lent lent[ReadStamp]
	return block(&lent, on.stage(&lent, C.ustore_completion_blocking_k))
}

// Stage submits [TransactionOn.Stage].
func (on TransactionDeferredOn) Stage() (*Deferred[ReadStamp], error) {
	deferred := new(Deferred[ReadStamp])
	return submitted(deferred, on.stage(&deferred.lent, C.ustore_completion_deferred_k))
}

func (receiver storeReceiver[Handle]) stage(lent *lent[ReadStamp], completion C.ustore_completion_t) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	arguments := open[C.ustore_transaction_stage_t](lent)
	*arguments = C.ustore_transaction_stage_t{
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		read_stamp:  (*C.ustore_stamp_t)(unsafe.Pointer(lent.result)),
	}
	C.ustore_transaction_stage(arguments)
	return nil
}

// Commit lands every change of the transaction at once, or none of them with a conflict status,
// answering its position in commit order.
func (on TransactionOn) Commit(durability Durability) (CommitStamp, error) {
	var lent lent[CommitStamp]
	return block(&lent, on.commit(&lent, C.ustore_completion_blocking_k, durability))
}

// Commit submits [TransactionOn.Commit].
func (on TransactionDeferredOn) Commit(durability Durability) (*Deferred[CommitStamp], error) {
	deferred := new(Deferred[CommitStamp])
	return submitted(deferred, on.commit(&deferred.lent, C.ustore_completion_deferred_k, durability))
}

func (receiver storeReceiver[Handle]) commit(lent *lent[CommitStamp], completion C.ustore_completion_t, durability Durability) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	arguments := open[C.ustore_transaction_commit_t](lent)
	*arguments = C.ustore_transaction_commit_t{
		database:     database,
		executor:     executor,
		outcome:      lent.outcome(),
		completion:   completion,
		transaction:  receiver.transaction(),
		commit_stamp: (*C.ustore_stamp_t)(unsafe.Pointer(lent.result)),
		durability:   C.ustore_durability_t(durability),
	}
	C.ustore_transaction_commit(arguments)
	return nil
}

// Rollback withdraws a stage while keeping the reads and writes, so that the transaction can
// stage again.
func (on TransactionOn) Rollback() error {
	var lent lent[struct{}]
	_, err := block(&lent, on.rollback(&lent, C.ustore_completion_blocking_k))
	return err
}

// Rollback submits [TransactionOn.Rollback].
func (on TransactionDeferredOn) Rollback() (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.rollback(&deferred.lent, C.ustore_completion_deferred_k))
}

func (receiver storeReceiver[Handle]) rollback(lent *lent[struct{}], completion C.ustore_completion_t) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	arguments := open[C.ustore_transaction_rollback_t](lent)
	*arguments = C.ustore_transaction_rollback_t{
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
	}
	C.ustore_transaction_rollback(arguments)
	return nil
}

// endregion: Transactions

// region: Cursors

// walk is what every cursor keeps between its pulls: the database it holds open, and the predicate
// it asks, pinned in place for as long as the cursor lives. It keeps no executor, since every pull
// names its own.
type walk struct {
	database  C.ustore_database_t
	predicate cgo.Handle
	pinner    runtime.Pinner
}

// admits lends the walk's own predicate, which outlives the init into every pull.
func (walk *walk) admits(admitter Admitter) C.ustore_predicate_t {
	if admitter == nil {
		return C.ustore_predicate_t{}
	}
	walk.pinner.Pin(&walk.predicate)
	return admitter.admitter(&walk.predicate)
}

// end gives back the predicate and the pins a cursor kept.
func (walk *walk) end() {
	walk.pinner.Unpin()
	if walk.predicate != 0 {
		walk.predicate.Delete()
		walk.predicate = 0
	}
}

// Page is one pull of a walk, written into its arena and readable until the arena serves another
// call.
type Page struct {
	keys    *C.ustore_key_t
	values  Strings
	written C.ustore_size_t
}

// Len reports how many keys the pull handed out; fewer than wanted exhausts the walk for good.
func (page Page) Len() uint { return uint(page.written) }

// Keys borrows the keys of the page from the arena.
func (page Page) Keys() []Key { return keysAt(page.keys, 0, uint64(page.written)) }

// Values borrows the values of the page from the arena, one per key, empty for a keys-only pull.
func (page Page) Values() Strings { return page.values.sized(uint(page.written)) }

// endregion: Cursors

// region: Changes

// ChangesCursor is a feed of the changes committed to one collection after a stamp, oldest first,
// pinning that stamp as a snapshot does until [ChangesCursor.Close].
type ChangesCursor struct {
	ChangesCursorOn
	walk
	handle C.ustore_changes_cursor_t
}

// ChangesCursorOn is the receiver whose pulls block until their call is final: on the calling
// goroutine when embedded in a [ChangesCursor], or on an executor that [ChangesCursorOn.On]
// picks.
type ChangesCursorOn struct {
	childReceiver[C.ustore_changes_cursor_t, C.ustore_database_t]
}

// On is the receiver whose pulls block on executor.
func (on ChangesCursorOn) On(executor *Executor) ChangesCursorOn {
	on.executor = executor
	return on
}

// ChangesCursorInit opens a feed of collection's changes committed after since, reading nothing yet
// and blocking whatever receiver it is called on; every pull names its own executor through On.
func (on DatabaseOn) ChangesCursorInit(collection CollectionID, since CommitStamp) (*ChangesCursor, error) {
	database, executor, err := on.lower()
	if err != nil {
		return nil, err
	}
	var lent lent[C.ustore_changes_cursor_t]
	arguments := open[C.ustore_changes_cursor_init_t](&lent)
	*arguments = C.ustore_changes_cursor_init_t{
		database:    database,
		executor:    executor,
		outcome:     lent.outcome(),
		collection:  rawCollection(collection),
		since_stamp: C.ustore_stamp_t(since),
		cursor:      lent.result,
	}
	C.ustore_changes_cursor_init(arguments)
	cursor := new(ChangesCursor)
	if cursor.handle, err = block(&lent, nil); err != nil {
		return nil, err
	}
	cursor.database = database
	cursor.childReceiver = childReceiver[C.ustore_changes_cursor_t, C.ustore_database_t]{receiver: receiver[C.ustore_changes_cursor_t]{handle: &cursor.handle}, parent: &cursor.database}
	return cursor, nil
}

// Changes is one pull of a feed, written into its arena and readable until the arena serves another
// call.
type Changes struct {
	keys   *C.ustore_key_t
	stamps *C.ustore_stamp_t
	kinds  *C.ustore_change_kind_t
	values Strings
	count  C.ustore_size_t
}

// Len reports how many changes the pull yielded, zero at the newest commit.
func (changes Changes) Len() uint { return uint(changes.count) }

// Key reports the key change index touched.
func (changes Changes) Key(index uint) Key {
	return at((*Key)(unsafe.Pointer(changes.keys)), index)
}

// Stamp reports the commit change index landed at.
func (changes Changes) Stamp(index uint) CommitStamp { return CommitStamp(at(changes.stamps, index)) }

// Kind reports what change index did to its key.
func (changes Changes) Kind(index uint) ChangeKind { return ChangeKind(at(changes.kinds, index)) }

// Value borrows the bytes change index carries from the arena, empty for a removal and for a pull
// through [ChangesCursor.NextKeys].
func (changes Changes) Value(index uint) []byte {
	values := changes.values.sized(uint(changes.count))
	if values.Len() == 0 {
		return nil
	}
	return values.Get(index)
}

// NextKeys pulls up to wanted changes into arena: their keys, stamps and kinds.
func (on ChangesCursorOn) NextKeys(wanted uint, arena *Arena) (Changes, error) {
	var lent lent[Changes]
	arguments, err := on.next(&lent, wanted, arena)
	if err != nil {
		return Changes{}, err
	}
	C.ustore_changes_cursor_next(arguments)
	return block(&lent, nil)
}

// NextPairs pulls up to wanted changes into arena with the bytes every change carries.
func (on ChangesCursorOn) NextPairs(wanted uint, arena *Arena) (Changes, error) {
	var lent lent[Changes]
	arguments, err := on.next(&lent, wanted, arena)
	if err != nil {
		return Changes{}, err
	}
	arguments.values = &lent.result.values.raw
	C.ustore_changes_cursor_next(arguments)
	return block(&lent, nil)
}

// next opens a pull and fills every argument but the optional values.
func (on ChangesCursorOn) next(lent *lent[Changes], wanted uint, arena *Arena) (*C.ustore_changes_cursor_next_t, error) {
	cursor, _, executor, outputs, err := on.lowerInto(arena)
	if err != nil {
		return nil, err
	}
	arguments := open[C.ustore_changes_cursor_next_t](lent)
	*arguments = C.ustore_changes_cursor_next_t{
		cursor:   cursor,
		executor: executor,
		outcome:  lent.outcome(),
		wanted:   C.ustore_size_t(wanted),
		count:    &lent.result.count,
		keys:     &lent.result.keys,
		stamps:   &lent.result.stamps,
		kinds:    &lent.result.kinds,
		arena:    outputs,
	}
	return arguments, nil
}

// Close releases the feed and the stamp it pins. It is idempotent.
func (cursor *ChangesCursor) Close() error {
	if cursor == nil || cursor.handle == nil {
		return nil
	}
	C.ustore_changes_cursor_free(cursor.handle)
	cursor.handle = nil
	cursor.end()
	return nil
}

// endregion: Changes

// region: Ingests

// Ingest lands every row appended to it into one collection as one commit of its own, joining no
// transaction; its calls run on the calling goroutine, one at a time.
type Ingest struct {
	handle C.ustore_ingest_t
}

// IngestInit opens an ingest into collection, whose rows arrive in order.
func (on DatabaseOn) IngestInit(collection CollectionID, order IngestOrder) (*Ingest, error) {
	database, _, err := on.lower()
	if err != nil {
		return nil, err
	}
	var lent lent[C.ustore_ingest_t]
	arguments := open[C.ustore_ingest_init_t](&lent)
	*arguments = C.ustore_ingest_init_t{
		ingest:     lent.result,
		database:   database,
		outcome:    lent.outcome(),
		collection: rawCollection(collection),
		order:      C.ustore_ingest_order_t(order),
	}
	C.ustore_ingest_init(arguments)
	handle, err := block(&lent, nil)
	if err != nil {
		return nil, err
	}
	return &Ingest{handle: handle}, nil
}

// IngestRows is one batch of rows: keys and values for blobs, documents and pages, values as the
// strings and keys as what they map onto for strings, and sources, targets and optional edge
// identifiers for graphs.
type IngestRows struct {
	// Keys are the keys of blobs, documents and pages, or the keys strings map onto.
	Keys []Key
	// Values are the values of blobs, JSON documents and XML pages, or the strings of a strings
	// collection.
	Values Tape
	// Sources are the sources of edges.
	Sources []Key
	// Targets are the targets of edges.
	Targets []Key
	// Edges are the identifiers of edges, or nothing for the default identifier of each.
	Edges []Key
}

// Append hands the ingest one batch of rows, copied or written before it returns.
func (ingest *Ingest) Append(rows IngestRows) error {
	if ingest == nil || ingest.handle == nil {
		return fail(StatusUninitialized, "the ingest is closed or was never opened")
	}
	count := max(len(rows.Keys), int(rows.Values.Len()), len(rows.Sources))
	if err := shape(nil, count, len(rows.Keys), int(rows.Values.Len()), len(rows.Sources), len(rows.Targets), len(rows.Edges)); err != nil {
		return err
	}
	var lent lent[struct{}]
	arguments := open[C.ustore_ingest_append_t](&lent)
	*arguments = C.ustore_ingest_append_t{
		ingest:  ingest.handle,
		outcome: lent.outcome(),
		count:   C.ustore_size_t(count),
		keys:    lent.keys(rows.Keys),
		values:  lent.strings(rows.Values),
		sources: lent.keys(rows.Sources),
		targets: lent.keys(rows.Targets),
		edges:   lent.keys(rows.Edges),
	}
	C.ustore_ingest_append(arguments)
	_, err := block(&lent, nil)
	return err
}

// Finish lands every row appended as one commit and ends the ingest, answering the one stamp every
// row landed at.
func (ingest *Ingest) Finish(durability Durability) (CommitStamp, error) {
	if ingest == nil || ingest.handle == nil {
		return 0, fail(StatusUninitialized, "the ingest is closed or was never opened")
	}
	var lent lent[CommitStamp]
	arguments := open[C.ustore_ingest_finish_t](&lent)
	*arguments = C.ustore_ingest_finish_t{
		ingest:       ingest.handle,
		outcome:      lent.outcome(),
		durability:   C.ustore_durability_t(durability),
		commit_stamp: (*C.ustore_stamp_t)(unsafe.Pointer(lent.result)),
	}
	C.ustore_ingest_finish(arguments)
	return block(&lent, nil)
}

// Close frees the ingest, dropping every row of an unfinished one. It is idempotent.
func (ingest *Ingest) Close() error {
	if ingest == nil || ingest.handle == nil {
		return nil
	}
	C.ustore_ingest_free(ingest.handle)
	ingest.handle = nil
	return nil
}

// endregion: Ingests
