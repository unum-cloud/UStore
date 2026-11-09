"""The loaded shared object and the one place a raw symbol address is ever touched.

The engine is reached by `dlopen` rather than by linking, which is what lets a consumer build
without a single linker flag. Mojo's `@extern` cannot serve here: it resolves at link time, so a
symbol from a library that is only opened at run time is an undefined reference under both the JIT
and `lld`, and using it would put a link flag in every consumer's build.

Every entry point is resolved once, when the library loads, into a typed `thin abi("C")` function
pointer. `OwnedDLHandle.get_function` would run `dlsym` on every call and parameterises only on the
return type, leaving arguments unchecked.

File: mojo/ustore/library.mojo
Author: Ash Vardanian
Date: November 8, 2026
"""

from std.ffi import OwnedDLHandle, __fn_type_is_cabi, c_int, c_size_t
from std.memory import ArcPointer, stack_allocation
from std.sys.info import CompilationTarget

from ustore.blobs import (
    BlobsCursorInitArguments,
    BlobsReadArguments,
    BlobsRemoveArguments,
    BlobsScanArguments,
    BlobsWriteArguments,
    CursorNextArguments,
    MeasureArguments,
    SampleArguments,
)
from ustore.docs import (
    DocsGatherArguments,
    DocsGistArguments,
    DocsReadArguments,
    DocsRemoveArguments,
    DocsWriteArguments,
    FieldType,
    FilteredCursorInitArguments,
    FilteredScanArguments,
    Modification,
)
from ustore.graph import (
    GraphEdgesArguments,
    GraphReadEdgesArguments,
    GraphRemoveVerticesArguments,
    GraphScanVerticesArguments,
    GraphVerticesCursorInitArguments,
    GraphVerticesCursorNextArguments,
    GraphWriteVerticesArguments,
    VertexRole,
)
from ustore.pages import (
    PageEncoding,
    PagesCursorNextArguments,
    PagesImportArguments,
    PagesInsertArguments,
    PagesMoveArguments,
    PagesReadArguments,
    PagesRemoveArguments,
    PagesRemoveAttributesArguments,
    PagesSetAttributesArguments,
    PagesSpliceArguments,
)
from ustore.strings import (
    StringsCursorInitArguments,
    StringsCursorNextArguments,
    StringsMeasureArguments,
    StringsReadArguments,
    StringsRemoveArguments,
    StringsSampleArguments,
    StringsScanArguments,
    StringsWriteArguments,
)
from ustore.db import (
    ChangeKind,
    ChangesCursorInitArguments,
    ChangesCursorNextArguments,
    CollectionCreateArguments,
    CollectionDropArguments,
    CollectionFindArguments,
    CollectionListArguments,
    DatabaseClearArguments,
    DatabaseCompactArguments,
    DatabaseDescribeArguments,
    DatabaseFlushArguments,
    DatabaseInitRamArguments,
    DatabaseOpenLeveldbArguments,
    DatabaseOpenNvmeArguments,
    DatabaseOpenRamArguments,
    DatabaseOpenRocksdbArguments,
    DropMode,
    Durability,
    Engine,
    IngestAppendArguments,
    IngestFinishArguments,
    IngestInitArguments,
    IngestOrder,
    Isolation,
    Modality,
    PredicateCost,
    ScanDirection,
    ScanMode,
    SnapshotInitArguments,
    SnapshotSaveArguments,
    TransactionCommitArguments,
    TransactionInitArguments,
    TransactionRollbackArguments,
    TransactionStageArguments,
    Watch,
)
from ustore.types import (
    ArenaDescribe,
    ArenaInit,
    BytesView,
    CaseSensitivity,
    CPUClass,
    CString,
    Error,
    ExecutorDrain,
    ExecutorForNvme,
    ExecutorForRam,
    ExecutorForVram,
    ExecutorOnPool,
    FromName,
    Gds,
    GPUClass,
    GPUDevice,
    GPUDevices,
    GPUDevicesArguments,
    GPUVendor,
    Mood,
    NameOf,
    Outcome,
    OutcomeStatus,
    PatternKind,
    Release,
    Status,
    Transport,
    Verb,
    ViewOf,
    WatchedViewOf,
    text_at,
)

comptime LIBRARY_FILE = StaticString(
    "libustore_shared.dylib" if CompilationTarget.is_macos() else "libustore_shared.so"
)
"""The loader already searches the environment's library directory, because `mojo build` bakes it
into the binary's runpath, so an unqualified name is enough for a library installed there."""

comptime BINDING_MAJOR = 1
"""The major version this binding was written against; a mismatch is refused at load."""

# region Signatures

comptime StaticText = def() thin abi("C") -> CString
"""`ustore_version_name`: a static C string returned by value."""

comptime VersionOf = def(
    Pointer[c_int, MutAnyOrigin], Pointer[c_int, MutAnyOrigin], Pointer[c_int, MutAnyOrigin]
) thin abi("C") -> None
"""`ustore_version`: the linked library's major, minor and patch, written through three `int` pointers.

Typed pointers rather than `Address`: an `alloca` whose address crosses only as an integer argument
was measured reading back the value stored before the call."""

# endregion Signatures


@inline(.always)
def _resolve[
    Signature: TrivialRegisterPassable
](ref handle: OwnedDLHandle, name: StaticString) raises Error -> Signature:
    """One `dlsym`, checked once, typed from here on.

    `get_symbol` answers `Optional` where `get_function` aborts the process, which is what lets a
    stale library name the symbol it is missing instead of dying without a message.
    """
    comptime assert __fn_type_is_cabi[Signature](), 'the signature must carry abi("C")'
    if case .Some(address) = handle.get_symbol[NoneType](name):
        return Pointer(to=address).unsafe_bitcast[Signature]()[]
    raise Error(Status.SYMBOL_MISSING, name)


def _version(version: VersionOf) -> Tuple[Int, Int, Int]:
    """The linked library's major, minor and patch, read through `ustore_version`."""
    var parts = stack_allocation[3, c_int]().unsafe_origin_cast[MutAnyOrigin]()
    for part in range(3):
        parts[unsafe_offset=part] = c_int(0)
    version(parts, parts.unsafe_offset(1), parts.unsafe_offset(2))
    return (Int(parts[unsafe_offset=0]), Int(parts[unsafe_offset=1]), Int(parts[unsafe_offset=2]))


struct Symbols:
    """Every entry point the C ABI exports outside its Arrow header, resolved once when the library loads."""

    var version: VersionOf
    var version_name: StaticText
    var outcome_wait: OutcomeStatus
    var outcome_status: OutcomeStatus
    var status_name: NameOf
    var status_from_name: FromName
    var pattern_kind_name: NameOf
    var pattern_kind_from_name: FromName
    var case_sensitivity_name: NameOf
    var case_sensitivity_from_name: FromName
    var mood_name: NameOf
    var mood_from_name: FromName
    var cpu_class_name: NameOf
    var cpu_class_from_name: FromName
    var transport_name: NameOf
    var transport_from_name: FromName
    var gds_name: NameOf
    var gds_from_name: FromName
    var gpu_vendor_name: NameOf
    var gpu_vendor_from_name: FromName
    var gpu_class_name: NameOf
    var gpu_class_from_name: FromName
    var gpu_devices: GPUDevices
    var executor_for_ram: ExecutorForRam
    var executor_on_pool: ExecutorOnPool
    var executor_for_nvme: ExecutorForNvme
    var executor_for_vram: ExecutorForVram
    var executor_drain: ExecutorDrain
    var executor_free: Release
    var arena_init: ArenaInit
    var arena_describe: ArenaDescribe
    var arena_free: Release

    var predicate_cost_name: NameOf
    var predicate_cost_from_name: FromName
    var durability_name: NameOf
    var durability_from_name: FromName
    var watch_name: NameOf
    var watch_from_name: FromName
    var scan_mode_name: NameOf
    var scan_mode_from_name: FromName
    var scan_direction_name: NameOf
    var scan_direction_from_name: FromName
    var drop_mode_name: NameOf
    var drop_mode_from_name: FromName
    var isolation_name: NameOf
    var isolation_from_name: FromName
    var engine_name: NameOf
    var engine_from_name: FromName
    var modality_name: NameOf
    var modality_from_name: FromName
    var change_kind_name: NameOf
    var change_kind_from_name: FromName
    var ingest_order_name: NameOf
    var ingest_order_from_name: FromName
    var database_init_ram: Verb[DatabaseInitRamArguments]
    var database_open_ram: Verb[DatabaseOpenRamArguments]
    var database_open_rocksdb: Verb[DatabaseOpenRocksdbArguments]
    var database_view_rocksdb: Verb[DatabaseOpenRocksdbArguments]
    var database_open_leveldb: Verb[DatabaseOpenLeveldbArguments]
    var database_open_nvme: Verb[DatabaseOpenNvmeArguments]
    var database_describe: Verb[DatabaseDescribeArguments]
    var database_clear: Verb[DatabaseClearArguments]
    var database_compact: Verb[DatabaseCompactArguments]
    var database_flush: Verb[DatabaseFlushArguments]
    var database_free: Release
    var snapshot_init: Verb[SnapshotInitArguments]
    var snapshot_save: Verb[SnapshotSaveArguments]
    var snapshot_view: ViewOf
    var snapshot_free: Release
    var collection_list: Verb[CollectionListArguments]
    var collection_create: Verb[CollectionCreateArguments]
    var collection_find: Verb[CollectionFindArguments]
    var collection_drop: Verb[CollectionDropArguments]
    var transaction_init: Verb[TransactionInitArguments]
    var transaction_view: WatchedViewOf
    var transaction_stage: Verb[TransactionStageArguments]
    var transaction_commit: Verb[TransactionCommitArguments]
    var transaction_rollback: Verb[TransactionRollbackArguments]
    var transaction_free: Release
    var changes_cursor_init: Verb[ChangesCursorInitArguments]
    var changes_cursor_next: Verb[ChangesCursorNextArguments]
    var changes_cursor_free: Release
    var ingest_init: Verb[IngestInitArguments]
    var ingest_append: Verb[IngestAppendArguments]
    var ingest_finish: Verb[IngestFinishArguments]
    var ingest_free: Release

    var blobs_write: Verb[BlobsWriteArguments]
    var blobs_remove: Verb[BlobsRemoveArguments]
    var blobs_read: Verb[BlobsReadArguments]
    var blobs_scan: Verb[BlobsScanArguments]
    var blobs_cursor_init: Verb[BlobsCursorInitArguments]
    var blobs_cursor_next: Verb[CursorNextArguments["ustore_blobs_cursor_next"]]
    var blobs_cursor_free: Release
    var blobs_sample: Verb[SampleArguments["ustore_blobs_sample"]]
    var blobs_measure: Verb[MeasureArguments["ustore_blobs_measure"]]

    var strings_write: Verb[StringsWriteArguments]
    var strings_remove: Verb[StringsRemoveArguments]
    var strings_read: Verb[StringsReadArguments]
    var strings_scan: Verb[StringsScanArguments]
    var strings_cursor_init: Verb[StringsCursorInitArguments]
    var strings_cursor_next: Verb[StringsCursorNextArguments]
    var strings_cursor_free: Release
    var strings_sample: Verb[StringsSampleArguments]
    var strings_measure: Verb[StringsMeasureArguments]

    var docs_field_type_name: NameOf
    var docs_field_type_from_name: FromName
    var docs_modification_name: NameOf
    var docs_modification_from_name: FromName
    var docs_write: Verb[DocsWriteArguments]
    var docs_read: Verb[DocsReadArguments]
    var docs_gist: Verb[DocsGistArguments]
    var docs_gather: Verb[DocsGatherArguments]
    var docs_remove: Verb[DocsRemoveArguments]
    var docs_scan: Verb[FilteredScanArguments["ustore_docs_scan"]]
    var docs_cursor_init: Verb[FilteredCursorInitArguments]
    var docs_cursor_next: Verb[CursorNextArguments["ustore_docs_cursor_next"]]
    var docs_cursor_free: Release
    var docs_sample: Verb[SampleArguments["ustore_docs_sample"]]
    var docs_measure: Verb[MeasureArguments["ustore_docs_measure"]]

    var vertex_role_name: NameOf
    var vertex_role_from_name: FromName
    var graph_read_edges: Verb[GraphReadEdgesArguments]
    var graph_write_edges: Verb[GraphEdgesArguments["ustore_graph_write_edges"]]
    var graph_remove_edges: Verb[GraphEdgesArguments["ustore_graph_remove_edges"]]
    var graph_write_vertices: Verb[GraphWriteVerticesArguments]
    var graph_remove_vertices: Verb[GraphRemoveVerticesArguments]
    var graph_scan_vertices: Verb[GraphScanVerticesArguments]
    var graph_vertices_cursor_init: Verb[GraphVerticesCursorInitArguments]
    var graph_vertices_cursor_next: Verb[GraphVerticesCursorNextArguments]
    var graph_vertices_cursor_free: Release
    var graph_sample_vertices: Verb[SampleArguments["ustore_graph_sample_vertices"]]
    var graph_measure_vertices: Verb[MeasureArguments["ustore_graph_measure_vertices"]]

    var page_encoding_name: NameOf
    var page_encoding_from_name: FromName
    var pages_import: Verb[PagesImportArguments]
    var pages_insert: Verb[PagesInsertArguments]
    var pages_remove: Verb[PagesRemoveArguments]
    var pages_move: Verb[PagesMoveArguments]
    var pages_splice: Verb[PagesSpliceArguments]
    var pages_set_attributes: Verb[PagesSetAttributesArguments]
    var pages_remove_attributes: Verb[PagesRemoveAttributesArguments]
    var pages_read: Verb[PagesReadArguments]
    var pages_scan: Verb[FilteredScanArguments["ustore_pages_scan"]]
    var pages_cursor_init: Verb[FilteredCursorInitArguments]
    var pages_cursor_next: Verb[PagesCursorNextArguments]
    var pages_cursor_free: Release
    var pages_sample: Verb[SampleArguments["ustore_pages_sample"]]
    var pages_measure: Verb[MeasureArguments["ustore_pages_measure"]]

    def __init__(out self, ref handle: OwnedDLHandle) raises Error:
        """Resolves every entry point at once, so no call ever pays for a `dlsym`.

        Raises:
            With `SYMBOL_MISSING`, on the first symbol the loaded library does not export.
        """
        self.version = _resolve[VersionOf](handle, "ustore_version")
        self.version_name = _resolve[StaticText](handle, "ustore_version_name")
        self.outcome_wait = _resolve[OutcomeStatus](handle, "ustore_outcome_wait")
        self.outcome_status = _resolve[OutcomeStatus](handle, "ustore_outcome_status")
        self.status_name = _resolve[NameOf](handle, "ustore_status_name")
        self.status_from_name = _resolve[FromName](handle, "ustore_status_from_name")
        self.pattern_kind_name = _resolve[NameOf](handle, "ustore_pattern_kind_name")
        self.pattern_kind_from_name = _resolve[FromName](handle, "ustore_pattern_kind_from_name")
        self.case_sensitivity_name = _resolve[NameOf](handle, "ustore_case_sensitivity_name")
        self.case_sensitivity_from_name = _resolve[FromName](handle, "ustore_case_sensitivity_from_name")
        self.mood_name = _resolve[NameOf](handle, "ustore_mood_name")
        self.mood_from_name = _resolve[FromName](handle, "ustore_mood_from_name")
        self.cpu_class_name = _resolve[NameOf](handle, "ustore_cpu_class_name")
        self.cpu_class_from_name = _resolve[FromName](handle, "ustore_cpu_class_from_name")
        self.transport_name = _resolve[NameOf](handle, "ustore_transport_name")
        self.transport_from_name = _resolve[FromName](handle, "ustore_transport_from_name")
        self.gds_name = _resolve[NameOf](handle, "ustore_gds_name")
        self.gds_from_name = _resolve[FromName](handle, "ustore_gds_from_name")
        self.gpu_vendor_name = _resolve[NameOf](handle, "ustore_gpu_vendor_name")
        self.gpu_vendor_from_name = _resolve[FromName](handle, "ustore_gpu_vendor_from_name")
        self.gpu_class_name = _resolve[NameOf](handle, "ustore_gpu_class_name")
        self.gpu_class_from_name = _resolve[FromName](handle, "ustore_gpu_class_from_name")
        self.gpu_devices = _resolve[GPUDevices](handle, "ustore_gpu_devices")
        self.executor_for_ram = _resolve[ExecutorForRam](handle, "ustore_executor_for_ram")
        self.executor_on_pool = _resolve[ExecutorOnPool](handle, "ustore_executor_on_pool")
        self.executor_for_nvme = _resolve[ExecutorForNvme](handle, "ustore_executor_for_nvme")
        self.executor_for_vram = _resolve[ExecutorForVram](handle, "ustore_executor_for_vram")
        self.executor_drain = _resolve[ExecutorDrain](handle, "ustore_executor_drain")
        self.executor_free = _resolve[Release](handle, "ustore_executor_free")
        self.arena_init = _resolve[ArenaInit](handle, "ustore_arena_init")
        self.arena_describe = _resolve[ArenaDescribe](handle, "ustore_arena_describe")
        self.arena_free = _resolve[Release](handle, "ustore_arena_free")

        self.predicate_cost_name = _resolve[NameOf](handle, "ustore_predicate_cost_name")
        self.predicate_cost_from_name = _resolve[FromName](handle, "ustore_predicate_cost_from_name")
        self.durability_name = _resolve[NameOf](handle, "ustore_durability_name")
        self.durability_from_name = _resolve[FromName](handle, "ustore_durability_from_name")
        self.watch_name = _resolve[NameOf](handle, "ustore_watch_name")
        self.watch_from_name = _resolve[FromName](handle, "ustore_watch_from_name")
        self.scan_mode_name = _resolve[NameOf](handle, "ustore_scan_mode_name")
        self.scan_mode_from_name = _resolve[FromName](handle, "ustore_scan_mode_from_name")
        self.scan_direction_name = _resolve[NameOf](handle, "ustore_scan_direction_name")
        self.scan_direction_from_name = _resolve[FromName](handle, "ustore_scan_direction_from_name")
        self.drop_mode_name = _resolve[NameOf](handle, "ustore_drop_mode_name")
        self.drop_mode_from_name = _resolve[FromName](handle, "ustore_drop_mode_from_name")
        self.isolation_name = _resolve[NameOf](handle, "ustore_isolation_name")
        self.isolation_from_name = _resolve[FromName](handle, "ustore_isolation_from_name")
        self.engine_name = _resolve[NameOf](handle, "ustore_engine_name")
        self.engine_from_name = _resolve[FromName](handle, "ustore_engine_from_name")
        self.modality_name = _resolve[NameOf](handle, "ustore_modality_name")
        self.modality_from_name = _resolve[FromName](handle, "ustore_modality_from_name")
        self.change_kind_name = _resolve[NameOf](handle, "ustore_change_kind_name")
        self.change_kind_from_name = _resolve[FromName](handle, "ustore_change_kind_from_name")
        self.ingest_order_name = _resolve[NameOf](handle, "ustore_ingest_order_name")
        self.ingest_order_from_name = _resolve[FromName](handle, "ustore_ingest_order_from_name")
        self.database_init_ram = _resolve[Verb[DatabaseInitRamArguments]](handle, "ustore_database_init_ram")
        self.database_open_ram = _resolve[Verb[DatabaseOpenRamArguments]](handle, "ustore_database_open_ram")
        self.database_open_rocksdb = _resolve[Verb[DatabaseOpenRocksdbArguments]](handle, "ustore_database_open_rocksdb")
        self.database_view_rocksdb = _resolve[Verb[DatabaseOpenRocksdbArguments]](handle, "ustore_database_view_rocksdb")
        self.database_open_leveldb = _resolve[Verb[DatabaseOpenLeveldbArguments]](handle, "ustore_database_open_leveldb")
        self.database_open_nvme = _resolve[Verb[DatabaseOpenNvmeArguments]](handle, "ustore_database_open_nvme")
        self.database_describe = _resolve[Verb[DatabaseDescribeArguments]](handle, "ustore_database_describe")
        self.database_clear = _resolve[Verb[DatabaseClearArguments]](handle, "ustore_database_clear")
        self.database_compact = _resolve[Verb[DatabaseCompactArguments]](handle, "ustore_database_compact")
        self.database_flush = _resolve[Verb[DatabaseFlushArguments]](handle, "ustore_database_flush")
        self.database_free = _resolve[Release](handle, "ustore_database_free")
        self.snapshot_init = _resolve[Verb[SnapshotInitArguments]](handle, "ustore_snapshot_init")
        self.snapshot_save = _resolve[Verb[SnapshotSaveArguments]](handle, "ustore_snapshot_save")
        self.snapshot_view = _resolve[ViewOf](handle, "ustore_snapshot_view")
        self.snapshot_free = _resolve[Release](handle, "ustore_snapshot_free")
        self.collection_list = _resolve[Verb[CollectionListArguments]](handle, "ustore_collection_list")
        self.collection_create = _resolve[Verb[CollectionCreateArguments]](handle, "ustore_collection_create")
        self.collection_find = _resolve[Verb[CollectionFindArguments]](handle, "ustore_collection_find")
        self.collection_drop = _resolve[Verb[CollectionDropArguments]](handle, "ustore_collection_drop")
        self.transaction_init = _resolve[Verb[TransactionInitArguments]](handle, "ustore_transaction_init")
        self.transaction_view = _resolve[WatchedViewOf](handle, "ustore_transaction_view")
        self.transaction_stage = _resolve[Verb[TransactionStageArguments]](handle, "ustore_transaction_stage")
        self.transaction_commit = _resolve[Verb[TransactionCommitArguments]](handle, "ustore_transaction_commit")
        self.transaction_rollback = _resolve[Verb[TransactionRollbackArguments]](handle, "ustore_transaction_rollback")
        self.transaction_free = _resolve[Release](handle, "ustore_transaction_free")
        self.changes_cursor_init = _resolve[Verb[ChangesCursorInitArguments]](handle, "ustore_changes_cursor_init")
        self.changes_cursor_next = _resolve[Verb[ChangesCursorNextArguments]](handle, "ustore_changes_cursor_next")
        self.changes_cursor_free = _resolve[Release](handle, "ustore_changes_cursor_free")
        self.ingest_init = _resolve[Verb[IngestInitArguments]](handle, "ustore_ingest_init")
        self.ingest_append = _resolve[Verb[IngestAppendArguments]](handle, "ustore_ingest_append")
        self.ingest_finish = _resolve[Verb[IngestFinishArguments]](handle, "ustore_ingest_finish")
        self.ingest_free = _resolve[Release](handle, "ustore_ingest_free")

        self.blobs_write = _resolve[Verb[BlobsWriteArguments]](handle, "ustore_blobs_write")
        self.blobs_remove = _resolve[Verb[BlobsRemoveArguments]](handle, "ustore_blobs_remove")
        self.blobs_read = _resolve[Verb[BlobsReadArguments]](handle, "ustore_blobs_read")
        self.blobs_scan = _resolve[Verb[BlobsScanArguments]](handle, "ustore_blobs_scan")
        self.blobs_cursor_init = _resolve[Verb[BlobsCursorInitArguments]](handle, "ustore_blobs_cursor_init")
        self.blobs_cursor_next = _resolve[Verb[CursorNextArguments["ustore_blobs_cursor_next"]]](handle, "ustore_blobs_cursor_next")
        self.blobs_cursor_free = _resolve[Release](handle, "ustore_blobs_cursor_free")
        self.blobs_sample = _resolve[Verb[SampleArguments["ustore_blobs_sample"]]](handle, "ustore_blobs_sample")
        self.blobs_measure = _resolve[Verb[MeasureArguments["ustore_blobs_measure"]]](handle, "ustore_blobs_measure")

        self.strings_write = _resolve[Verb[StringsWriteArguments]](handle, "ustore_strings_write")
        self.strings_remove = _resolve[Verb[StringsRemoveArguments]](handle, "ustore_strings_remove")
        self.strings_read = _resolve[Verb[StringsReadArguments]](handle, "ustore_strings_read")
        self.strings_scan = _resolve[Verb[StringsScanArguments]](handle, "ustore_strings_scan")
        self.strings_cursor_init = _resolve[Verb[StringsCursorInitArguments]](handle, "ustore_strings_cursor_init")
        self.strings_cursor_next = _resolve[Verb[StringsCursorNextArguments]](handle, "ustore_strings_cursor_next")
        self.strings_cursor_free = _resolve[Release](handle, "ustore_strings_cursor_free")
        self.strings_sample = _resolve[Verb[StringsSampleArguments]](handle, "ustore_strings_sample")
        self.strings_measure = _resolve[Verb[StringsMeasureArguments]](handle, "ustore_strings_measure")

        self.docs_field_type_name = _resolve[NameOf](handle, "ustore_docs_field_type_name")
        self.docs_field_type_from_name = _resolve[FromName](handle, "ustore_docs_field_type_from_name")
        self.docs_modification_name = _resolve[NameOf](handle, "ustore_docs_modification_name")
        self.docs_modification_from_name = _resolve[FromName](handle, "ustore_docs_modification_from_name")
        self.docs_write = _resolve[Verb[DocsWriteArguments]](handle, "ustore_docs_write")
        self.docs_read = _resolve[Verb[DocsReadArguments]](handle, "ustore_docs_read")
        self.docs_gist = _resolve[Verb[DocsGistArguments]](handle, "ustore_docs_gist")
        self.docs_gather = _resolve[Verb[DocsGatherArguments]](handle, "ustore_docs_gather")
        self.docs_remove = _resolve[Verb[DocsRemoveArguments]](handle, "ustore_docs_remove")
        self.docs_scan = _resolve[Verb[FilteredScanArguments["ustore_docs_scan"]]](handle, "ustore_docs_scan")
        self.docs_cursor_init = _resolve[Verb[FilteredCursorInitArguments]](handle, "ustore_docs_cursor_init")
        self.docs_cursor_next = _resolve[Verb[CursorNextArguments["ustore_docs_cursor_next"]]](handle, "ustore_docs_cursor_next")
        self.docs_cursor_free = _resolve[Release](handle, "ustore_docs_cursor_free")
        self.docs_sample = _resolve[Verb[SampleArguments["ustore_docs_sample"]]](handle, "ustore_docs_sample")
        self.docs_measure = _resolve[Verb[MeasureArguments["ustore_docs_measure"]]](handle, "ustore_docs_measure")

        self.vertex_role_name = _resolve[NameOf](handle, "ustore_vertex_role_name")
        self.vertex_role_from_name = _resolve[FromName](handle, "ustore_vertex_role_from_name")
        self.graph_read_edges = _resolve[Verb[GraphReadEdgesArguments]](handle, "ustore_graph_read_edges")
        self.graph_write_edges = _resolve[Verb[GraphEdgesArguments["ustore_graph_write_edges"]]](handle, "ustore_graph_write_edges")
        self.graph_remove_edges = _resolve[Verb[GraphEdgesArguments["ustore_graph_remove_edges"]]](handle, "ustore_graph_remove_edges")
        self.graph_write_vertices = _resolve[Verb[GraphWriteVerticesArguments]](handle, "ustore_graph_write_vertices")
        self.graph_remove_vertices = _resolve[Verb[GraphRemoveVerticesArguments]](handle, "ustore_graph_remove_vertices")
        self.graph_scan_vertices = _resolve[Verb[GraphScanVerticesArguments]](handle, "ustore_graph_scan_vertices")
        self.graph_vertices_cursor_init = _resolve[Verb[GraphVerticesCursorInitArguments]](handle, "ustore_graph_vertices_cursor_init")
        self.graph_vertices_cursor_next = _resolve[Verb[GraphVerticesCursorNextArguments]](handle, "ustore_graph_vertices_cursor_next")
        self.graph_vertices_cursor_free = _resolve[Release](handle, "ustore_graph_vertices_cursor_free")
        self.graph_sample_vertices = _resolve[Verb[SampleArguments["ustore_graph_sample_vertices"]]](handle, "ustore_graph_sample_vertices")
        self.graph_measure_vertices = _resolve[Verb[MeasureArguments["ustore_graph_measure_vertices"]]](handle, "ustore_graph_measure_vertices")

        self.page_encoding_name = _resolve[NameOf](handle, "ustore_page_encoding_name")
        self.page_encoding_from_name = _resolve[FromName](handle, "ustore_page_encoding_from_name")
        self.pages_import = _resolve[Verb[PagesImportArguments]](handle, "ustore_pages_import")
        self.pages_insert = _resolve[Verb[PagesInsertArguments]](handle, "ustore_pages_insert")
        self.pages_remove = _resolve[Verb[PagesRemoveArguments]](handle, "ustore_pages_remove")
        self.pages_move = _resolve[Verb[PagesMoveArguments]](handle, "ustore_pages_move")
        self.pages_splice = _resolve[Verb[PagesSpliceArguments]](handle, "ustore_pages_splice")
        self.pages_set_attributes = _resolve[Verb[PagesSetAttributesArguments]](handle, "ustore_pages_set_attributes")
        self.pages_remove_attributes = _resolve[Verb[PagesRemoveAttributesArguments]](handle, "ustore_pages_remove_attributes")
        self.pages_read = _resolve[Verb[PagesReadArguments]](handle, "ustore_pages_read")
        self.pages_scan = _resolve[Verb[FilteredScanArguments["ustore_pages_scan"]]](handle, "ustore_pages_scan")
        self.pages_cursor_init = _resolve[Verb[FilteredCursorInitArguments]](handle, "ustore_pages_cursor_init")
        self.pages_cursor_next = _resolve[Verb[PagesCursorNextArguments]](handle, "ustore_pages_cursor_next")
        self.pages_cursor_free = _resolve[Release](handle, "ustore_pages_cursor_free")
        self.pages_sample = _resolve[Verb[SampleArguments["ustore_pages_sample"]]](handle, "ustore_pages_sample")
        self.pages_measure = _resolve[Verb[MeasureArguments["ustore_pages_measure"]]](handle, "ustore_pages_measure")


struct _Loaded:
    """The handle and its symbols, kept together so neither outlives the other."""

    var handle: OwnedDLHandle
    var symbols: Symbols

    def __init__(out self) raises Error:
        try:
            self.handle = OwnedDLHandle(String(LIBRARY_FILE))
        except:
            raise Error(Status.LIBRARY_MISSING, LIBRARY_FILE)
        # Checked first, so a library of another major reports that rather than a missing verb.
        if _version(_resolve[VersionOf](self.handle, "ustore_version"))[0] != BINDING_MAJOR:
            raise Error(Status.VERSION_MISMATCH, "ustore_version")
        self.symbols = Symbols(self.handle)


struct Library(ImplicitlyCopyable):
    """A reference to the loaded engine, cheap to copy and shared by everything it creates.

    Every handle carries one, which is what keeps the shared object mapped for as long as anything
    can still call into it.
    """

    var shared: ArcPointer[_Loaded]
    """The handle and its symbols, refcounted so the last holder unmaps the object."""

    def __init__(out self) raises Error:
        """Opens the shared object and resolves it, which every handle then shares.

        Raises:
            With `LIBRARY_MISSING`, when nothing is on the loader path, with `VERSION_MISMATCH`
            when what loaded is another major version, or with `SYMBOL_MISSING` when it does not
            export an entry point the binding needs.
        """
        self.shared = ArcPointer(_Loaded())

    @inline(.always)
    def symbols(self) -> ref[origin_of(self.shared[].symbols)] Symbols:
        """The resolved entry points, borrowed rather than copied on every call."""
        return self.shared[].symbols

    def version(self) -> Tuple[Int, Int, Int]:
        """The loaded engine's major, minor and patch. Calls `ustore_version`."""
        return _version(self.symbols().version)

    def version_name(self) -> String:
        """The loaded engine's own version, like "1.0.0". Calls `ustore_version_name`."""
        return text_at(self.symbols().version_name())

    def gpu_devices[devices_origin: MutOrigin, //](self, devices: Span[GPUDevice, devices_origin]) raises Error -> Int:
        """Fills `devices` with every GPU this process can drive, in the ordinal order a VRAM executor
        names them by. Calls `ustore_gpu_devices`.

        Args:
            devices: Where the listing lands, as many entries as fit; may be empty to only count.

        Returns:
            How many devices this process can drive, which may exceed `len(devices)`.
        """
        var count = stack_allocation[1, c_size_t]().unsafe_origin_cast[MutUntrackedOrigin]()
        count[] = 0
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        var listed: Optional[Pointer[GPUDevice, MutUntrackedOrigin]] = None
        if len(devices):
            listed = Pointer[GPUDevice, MutUntrackedOrigin](unsafe_from_address=Int(devices.unsafe_ptr()))
        var arguments = stack_allocation[1, GPUDevicesArguments]()
        arguments[unsafe_offset=0] = GPUDevicesArguments(outcome, listed, c_size_t(len(devices)), count)
        self.symbols().gpu_devices(arguments.unsafe_origin_cast[MutAnyOrigin]())
        self._check(outcome.unsafe_origin_cast[MutAnyOrigin]()[], "ustore_gpu_devices")
        return Int(count.unsafe_origin_cast[MutAnyOrigin]()[])

    def status_name(self, status: Status) -> String:
        """The engine's spelling of a status, "unrecognized" for a code it does not name. Calls `ustore_status_name`."""
        return text_at(self.symbols().status_name(c_int(status.code)))

    def status_from_name(self, name: StringSlice) raises Error -> Status:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_status_from_name`."""
        return Status.of(self._read_name(self.symbols().status_from_name, name, "ustore_status_from_name"))

    def pattern_kind_name(self, kind: PatternKind) -> String:
        """The engine's spelling of a pattern kind. Calls `ustore_pattern_kind_name`."""
        return text_at(self.symbols().pattern_kind_name(kind.code))

    def pattern_kind_from_name(self, name: StringSlice) raises Error -> PatternKind:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_pattern_kind_from_name`."""
        return PatternKind(
            self._read_name(self.symbols().pattern_kind_from_name, name, "ustore_pattern_kind_from_name")
        )

    def case_sensitivity_name(self, sensitivity: CaseSensitivity) -> String:
        """The engine's spelling of a case sensitivity. Calls `ustore_case_sensitivity_name`."""
        return text_at(self.symbols().case_sensitivity_name(sensitivity.code))

    def case_sensitivity_from_name(self, name: StringSlice) raises Error -> CaseSensitivity:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_case_sensitivity_from_name`."""
        return CaseSensitivity(
            self._read_name(self.symbols().case_sensitivity_from_name, name, "ustore_case_sensitivity_from_name")
        )

    def mood_name(self, mood: Mood) -> String:
        """The engine's spelling of a mood. Calls `ustore_mood_name`."""
        return text_at(self.symbols().mood_name(mood.code))

    def mood_from_name(self, name: StringSlice) raises Error -> Mood:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_mood_from_name`."""
        return Mood(self._read_name(self.symbols().mood_from_name, name, "ustore_mood_from_name"))

    def cpu_class_name(self, cpu_class: CPUClass) -> String:
        """The engine's spelling of a CPU class, "automatic" for `UNKNOWN`. Calls `ustore_cpu_class_name`."""
        return text_at(self.symbols().cpu_class_name(cpu_class.code))

    def cpu_class_from_name(self, name: StringSlice) raises Error -> CPUClass:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_cpu_class_from_name`."""
        return CPUClass(self._read_name(self.symbols().cpu_class_from_name, name, "ustore_cpu_class_from_name"))

    def transport_name(self, transport: Transport) -> String:
        """The engine's spelling of a transport. Calls `ustore_transport_name`."""
        return text_at(self.symbols().transport_name(transport.code))

    def transport_from_name(self, name: StringSlice) raises Error -> Transport:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_transport_from_name`."""
        return Transport(self._read_name(self.symbols().transport_from_name, name, "ustore_transport_from_name"))

    def gds_name(self, gds: Gds) -> String:
        """The engine's spelling of a GPUDirect Storage mode. Calls `ustore_gds_name`."""
        return text_at(self.symbols().gds_name(gds.code))

    def gds_from_name(self, name: StringSlice) raises Error -> Gds:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_gds_from_name`."""
        return Gds(self._read_name(self.symbols().gds_from_name, name, "ustore_gds_from_name"))

    def gpu_vendor_name(self, vendor: GPUVendor) -> String:
        """The engine's spelling of a GPU vendor. Calls `ustore_gpu_vendor_name`."""
        return text_at(self.symbols().gpu_vendor_name(vendor.code))

    def gpu_vendor_from_name(self, name: StringSlice) raises Error -> GPUVendor:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_gpu_vendor_from_name`."""
        return GPUVendor(self._read_name(self.symbols().gpu_vendor_from_name, name, "ustore_gpu_vendor_from_name"))

    def gpu_class_name(self, gpu_class: GPUClass) -> String:
        """The engine's spelling of a GPU class. Calls `ustore_gpu_class_name`."""
        return text_at(self.symbols().gpu_class_name(gpu_class.code))

    def gpu_class_from_name(self, name: StringSlice) raises Error -> GPUClass:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_gpu_class_from_name`."""
        return GPUClass(self._read_name(self.symbols().gpu_class_from_name, name, "ustore_gpu_class_from_name"))

    def predicate_cost_name(self, cost: PredicateCost) -> String:
        """The engine's spelling of a predicate cost. Calls `ustore_predicate_cost_name`."""
        return text_at(self.symbols().predicate_cost_name(cost.code))

    def predicate_cost_from_name(self, name: StringSlice) raises Error -> PredicateCost:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_predicate_cost_from_name`."""
        return PredicateCost(
            self._read_name(self.symbols().predicate_cost_from_name, name, "ustore_predicate_cost_from_name")
        )

    def durability_name(self, durability: Durability) -> String:
        """The engine's spelling of a durability. Calls `ustore_durability_name`."""
        return text_at(self.symbols().durability_name(durability.code))

    def durability_from_name(self, name: StringSlice) raises Error -> Durability:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_durability_from_name`."""
        return Durability(self._read_name(self.symbols().durability_from_name, name, "ustore_durability_from_name"))

    def watch_name(self, watch: Watch) -> String:
        """The engine's spelling of a watch. Calls `ustore_watch_name`."""
        return text_at(self.symbols().watch_name(watch.code))

    def watch_from_name(self, name: StringSlice) raises Error -> Watch:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_watch_from_name`."""
        return Watch(self._read_name(self.symbols().watch_from_name, name, "ustore_watch_from_name"))

    def scan_mode_name(self, mode: ScanMode) -> String:
        """The engine's spelling of a scan mode. Calls `ustore_scan_mode_name`."""
        return text_at(self.symbols().scan_mode_name(mode.code))

    def scan_mode_from_name(self, name: StringSlice) raises Error -> ScanMode:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_scan_mode_from_name`."""
        return ScanMode(self._read_name(self.symbols().scan_mode_from_name, name, "ustore_scan_mode_from_name"))

    def scan_direction_name(self, direction: ScanDirection) -> String:
        """The engine's spelling of a scan direction. Calls `ustore_scan_direction_name`."""
        return text_at(self.symbols().scan_direction_name(direction.code))

    def scan_direction_from_name(self, name: StringSlice) raises Error -> ScanDirection:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_scan_direction_from_name`."""
        return ScanDirection(
            self._read_name(self.symbols().scan_direction_from_name, name, "ustore_scan_direction_from_name")
        )

    def drop_mode_name(self, mode: DropMode) -> String:
        """The engine's spelling of a drop mode. Calls `ustore_drop_mode_name`."""
        return text_at(self.symbols().drop_mode_name(mode.code))

    def drop_mode_from_name(self, name: StringSlice) raises Error -> DropMode:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_drop_mode_from_name`."""
        return DropMode(self._read_name(self.symbols().drop_mode_from_name, name, "ustore_drop_mode_from_name"))

    def isolation_name(self, isolation: Isolation) -> String:
        """The engine's spelling of an isolation. Calls `ustore_isolation_name`."""
        return text_at(self.symbols().isolation_name(isolation.code))

    def isolation_from_name(self, name: StringSlice) raises Error -> Isolation:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_isolation_from_name`."""
        return Isolation(self._read_name(self.symbols().isolation_from_name, name, "ustore_isolation_from_name"))

    def engine_name(self, engine: Engine) -> String:
        """The engine's spelling of an engine. Calls `ustore_engine_name`."""
        return text_at(self.symbols().engine_name(engine.code))

    def engine_from_name(self, name: StringSlice) raises Error -> Engine:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_engine_from_name`."""
        return Engine(self._read_name(self.symbols().engine_from_name, name, "ustore_engine_from_name"))

    def modality_name(self, modality: Modality) -> String:
        """The engine's spelling of a modality. Calls `ustore_modality_name`."""
        return text_at(self.symbols().modality_name(modality.code))

    def modality_from_name(self, name: StringSlice) raises Error -> Modality:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_modality_from_name`."""
        return Modality(self._read_name(self.symbols().modality_from_name, name, "ustore_modality_from_name"))

    def change_kind_name(self, kind: ChangeKind) -> String:
        """The engine's spelling of a change kind. Calls `ustore_change_kind_name`."""
        return text_at(self.symbols().change_kind_name(kind.code))

    def change_kind_from_name(self, name: StringSlice) raises Error -> ChangeKind:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_change_kind_from_name`."""
        return ChangeKind(self._read_name(self.symbols().change_kind_from_name, name, "ustore_change_kind_from_name"))

    def ingest_order_name(self, order: IngestOrder) -> String:
        """The engine's spelling of an ingest order. Calls `ustore_ingest_order_name`."""
        return text_at(self.symbols().ingest_order_name(order.code))

    def ingest_order_from_name(self, name: StringSlice) raises Error -> IngestOrder:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_ingest_order_from_name`."""
        return IngestOrder(
            self._read_name(self.symbols().ingest_order_from_name, name, "ustore_ingest_order_from_name")
        )

    def docs_field_type_name(self, field_type: FieldType) -> String:
        """The engine's spelling of a field type, as `F64` spells "f64". Calls `ustore_docs_field_type_name`."""
        return text_at(self.symbols().docs_field_type_name(field_type.code))

    def docs_field_type_from_name(self, name: StringSlice) raises Error -> FieldType:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_docs_field_type_from_name`."""
        return FieldType(
            self._read_name(self.symbols().docs_field_type_from_name, name, "ustore_docs_field_type_from_name")
        )

    def docs_modification_name(self, modification: Modification) -> String:
        """The engine's spelling of a modification. Calls `ustore_docs_modification_name`."""
        return text_at(self.symbols().docs_modification_name(modification.code))

    def docs_modification_from_name(self, name: StringSlice) raises Error -> Modification:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_docs_modification_from_name`."""
        return Modification(
            self._read_name(self.symbols().docs_modification_from_name, name, "ustore_docs_modification_from_name")
        )

    def vertex_role_name(self, role: VertexRole) -> String:
        """The engine's spelling of a vertex role. Calls `ustore_vertex_role_name`."""
        return text_at(self.symbols().vertex_role_name(role.code))

    def vertex_role_from_name(self, name: StringSlice) raises Error -> VertexRole:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_vertex_role_from_name`."""
        return VertexRole(self._read_name(self.symbols().vertex_role_from_name, name, "ustore_vertex_role_from_name"))

    def page_encoding_name(self, encoding: PageEncoding) -> String:
        """The engine's spelling of a page encoding. Calls `ustore_page_encoding_name`."""
        return text_at(self.symbols().page_encoding_name(encoding.code))

    def page_encoding_from_name(self, name: StringSlice) raises Error -> PageEncoding:
        """Reads a spelling back or raises `INVALID_ARGUMENT`. Calls `ustore_page_encoding_from_name`."""
        return PageEncoding(
            self._read_name(self.symbols().page_encoding_from_name, name, "ustore_page_encoding_from_name")
        )

    def _read_name(self, from_name: FromName, name: StringSlice, symbol: StaticString) raises Error -> c_int:
        """Reads `name` back through one of the `_from_name` family, raising the status it refuses with."""
        var value = c_int(0)
        var code = from_name(BytesView.of(name).arguments(), Pointer(to=value))
        if code != c_int(0):
            var status = Status.of(code)
            raise Error(status, symbol, None, self.symbols().status_name(c_int(status.code)), self)
        return value

    def _error(self, outcome: Outcome, symbol: StaticString) -> Error:
        """The failure a final outcome reports, its status spelled by the engine's own table."""
        var status = Status.of(outcome.status)
        return Error(status, symbol, outcome.message, self.symbols().status_name(c_int(status.code)), self)

    def _check(self, outcome: Outcome, symbol: StaticString) raises Error:
        """Raises the failure a final outcome reports, and returns on success."""
        if outcome.status != c_int(0):
            raise self._error(outcome, symbol)
