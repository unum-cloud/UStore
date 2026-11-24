"""The Mojo binding's tests, registered in `main` since Mojo has no test discovery.

The pure-logic group needs no engine. The rest open in-memory databases, one of them served from a
temporary directory, so they touch no raw drive and no device, and are skipped when nothing is on
the loader path.

File: mojo/test.mojo
Author: Ash Vardanian
Date: November 8, 2026
"""

from std.os import listdir, remove, rmdir
from std.runtime._asyncrt import create_raising_task
from std.tempfile import mkdtemp
from std.sys.info import CompilationTarget
from std.testing import TestSuite, assert_equal, assert_false, assert_raises, assert_true

from ustore import (
    Admission,
    Arena,
    BlobsScanConfig,
    CPUClass,
    CollectionID,
    CollectionsView,
    Continuation,
    Database,
    DatabaseCompactPlan,
    DocsReadConfig,
    FieldType,
    Error,
    Executor,
    GPUClass,
    GPUVendor,
    Key,
    KeysView,
    Library,
    Modality,
    Mood,
    Phase,
    PhaseProgress,
    Predicate,
    PredicateCost,
    Progress,
    Residency,
    column_to_arrow,
    keys_from_arrow,
    keys_to_arrow,
    tape_to_arrow,
    StringsView,
    Status,
    UUID,
    VertexRole,
    VertexRolesView,
)


# region Shared Testing
def _skipped(reason: String):
    """Announces a skipped test, so a run shows what it left out rather than passing in silence."""
    print("skipped:", reason)


def _split_mix_bits(value: UInt64) -> UInt64:
    """SplitMix64's finalizer, a bijection that spreads every input bit over all 64 output bits."""
    var mixed = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EB
    return mixed ^ (mixed >> 31)


def _stream_key(seed: UInt64, name: String, index: UInt64 = 0) -> UInt64:
    """The `index`-th stream of the family `name` under `seed`, identical in C, C++, Rust and Python."""
    var hashed: UInt64 = 0xCBF29CE484222325
    for byte in name.as_bytes():
        hashed = (hashed ^ UInt64(byte)) * 0x100000001B3
    return _split_mix_bits(_split_mix_bits(seed ^ hashed) + index)


# endregion Shared Testing


comptime library_missing_k = "the shared library is not on the loader path"

# region Helpers


struct _Tape:
    """Strings packed as an Apache Arrow tape, owned by the test."""

    var bytes: List[UInt8]
    var offsets: List[UInt32]

    def __init__(out self, texts: List[String]):
        self.bytes = List[UInt8]()
        self.offsets = [0]
        for text in texts:
            for byte in text.as_bytes():
                self.bytes.append(byte)
            self.offsets.append(UInt32(len(self.bytes)))


def _text[origin: Origin](bytes: Span[UInt8, origin]) -> String:
    """The ASCII bytes a result holds, as a string."""
    var text = String()
    for byte in bytes:
        text += chr(Int(byte))
    return text


def _library_is_present() raises -> Bool:
    """Whether the shared library is on the loader path; any other failure to load it raises rather than skips."""
    try:
        _ = Library()
        return True
    except error:
        if error.status != Status.LIBRARY_MISSING:
            raise error
        return False


# endregion Helpers

# region Pure Logic


def test_keys_spell_integers_big_endian() raises:
    """An integer key keeps its value in its last bytes, so keys made from integers order as the integers."""
    var key = Key(0x0102)
    assert_equal(Int(key.uuid.bytes[15]), 2)
    assert_equal(Int(key.uuid.bytes[14]), 1)
    assert_equal(Int(key.uuid.bytes[0]), 0)
    assert_true(Key(7) == Key(7))
    assert_false(Key(7) == Key(8))
    assert_equal(String(UUID(1)), "00000000-0000-0000-0000-000000000001")


# endregion Pure Logic

# region Engine


def test_enums_spell_and_parse_as_the_library_names_them() raises:
    """The engine reports major version 1, spells its enumerators both ways and refuses other names."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    assert_equal(library.version()[0], 1)
    assert_equal(library.status_name(Status.NOT_FOUND), "not-found")
    assert_true(library.status_from_name("not-found") == Status.NOT_FOUND)
    with assert_raises():
        _ = library.durability_from_name("bogus")
    assert_equal(library.modality_name(Modality.GRAPH), "graph")
    assert_equal(library.phase_name(Phase.MERGE), "merge")
    assert_true(library.phase_from_name("reclaim") == Phase.RECLAIM)
    assert_equal(library.gpu_vendor_name(GPUVendor.APPLE), "apple")
    assert_true(library.gpu_class_from_name("apple-metal9") == GPUClass.APPLE_METAL9)


def test_blobs_write_read_and_scan() raises:
    """Values written in one batch read back in key order, a missing key reads absent, and a scan lists them."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var keys: List[Key] = [Key(1), Key(2), Key(3)]
    var values = _Tape(["Ada", "Grace", "Barbara"])
    db.write(CollectionsView.repeating(people), KeysView.of(keys), StringsView.of(values.bytes, values.offsets))

    var arena = Arena(library)
    assert_true(arena.describe().residency == Residency.RAM)
    var asked: List[Key] = [Key(2), Key(9)]
    var read = db.read(CollectionsView.repeating(people), KeysView.of(asked), arena)
    assert_true(read.present(0))
    assert_false(read.present(1))
    assert_equal(_text(read.values()[0]), "Grace")
    assert_equal(Int(read.lengths()[0]), 5)

    var start = Key(0)
    var scanned = db.scan(CollectionsView.repeating(people), KeysView.repeating(start), 10, arena)
    assert_equal(len(scanned.run(0)), 3)
    assert_true(scanned.run(0)[2] == Key(3))


def test_a_later_call_retires_earlier_results() raises:
    """A result refuses reads once a later call rewinds its arena."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("", Modality.BLOBS)
    var key = Key(1)
    var values = _Tape(["one"])
    db.write(CollectionsView.repeating(people), KeysView.repeating(key), StringsView.of(values.bytes, values.offsets))
    var arena = Arena(library)
    var first = db.read(CollectionsView.repeating(people), KeysView.repeating(key), arena)
    _ = db.read(CollectionsView.repeating(people), KeysView.repeating(key), arena)
    var refused = Status.SUCCESS
    try:
        _ = first.present(0)
    except error:
        refused = error.status
    assert_true(refused == Status.INVALID_ARGUMENT, "a rewound result must refuse reads")


def test_strings_map_onto_keys() raises:
    """Strings written onto keys read back as those keys, and a prefix scan lists only its prefix."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var names = db.collection_create("names", Modality.STRINGS)
    var strings = _Tape(["apple", "apricot", "banana"])
    var keys: List[Key] = [Key(10), Key(11), Key(12)]
    db.strings_write(
        CollectionsView.repeating(names), StringsView.of(strings.bytes, strings.offsets), KeysView.of(keys)
    )

    var arena = Arena(library)
    var asked = _Tape(["apricot", "cherry"])
    var found = db.strings_read(CollectionsView.repeating(names), StringsView.of(asked.bytes, asked.offsets), arena)
    assert_true(found.present(0))
    assert_false(found.present(1))
    assert_true(found.keys()[0] == Key(11))

    var prefix = _Tape(["ap"])
    var listed = db.strings_scan(
        CollectionsView.repeating(names), StringsView.of(prefix.bytes, prefix.offsets), 10, arena
    )
    var tape = listed.strings()
    assert_equal(len(tape), 2)
    assert_equal(_text(tape[0]), "apple")
    assert_equal(_text(tape[1]), "apricot")


def test_docs_write_and_read() raises:
    """A JSON document reads back whole, and one field of it reads as text."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var movies = db.collection_create("movies", Modality.DOCS)
    var key = Key(1)
    var documents = _Tape(['{"title":"Alien","year":1979}'])
    db.docs_write(
        CollectionsView.repeating(movies), KeysView.repeating(key), StringsView.of(documents.bytes, documents.offsets)
    )

    var arena = Arena(library)
    var whole = db.docs_read(CollectionsView.repeating(movies), KeysView.repeating(key), arena)
    assert_true(whole.present(0))
    assert_true("Alien" in _text(whole.values()[0]))

    var fields = _Tape(["/year"])
    var year = db.docs_read(
        CollectionsView.repeating(movies),
        KeysView.repeating(key),
        arena,
        DocsReadConfig(fields=StringsView.of(fields.bytes, fields.offsets)),
    )
    assert_equal(_text(year.values()[0]), "1979")


def test_graph_edges_round_trip() raises:
    """An edge written from one vertex to another reads back from both ends."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var graph = db.collection_create("graph", Modality.GRAPH)
    var source = Key(1)
    var target = Key(2)
    db.graph_write_edges(CollectionsView.repeating(graph), KeysView.repeating(source), KeysView.repeating(target))

    var arena = Arena(library)
    var vertices: List[Key] = [Key(1), Key(2)]
    var edges = db.graph_read_edges(CollectionsView.repeating(graph), KeysView.of(vertices), arena)
    assert_equal(Int(edges.degrees()[0]), 1)
    assert_equal(Int(edges.degrees()[1]), 1)
    var outgoing = edges.edges(0)
    assert_equal(len(outgoing), 3)
    assert_true(outgoing[0] == Key(1))
    assert_true(outgoing[1] == Key(2))

    var role = VertexRole.SOURCE
    var sources = db.graph_read_edges(
        CollectionsView.repeating(graph), KeysView.of(vertices), arena, roles=VertexRolesView.repeating(role)
    )
    assert_equal(Int(sources.degrees()[1]), 0)


def test_deferred_calls_match_blocking_ones() raises:
    """A write and a read submitted to an executor finish once waited on, with the blocking call's answer."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var executor = Executor.for_ram(library, threads=2)
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var key = Key(5)
    var values = _Tape(["deferred"])
    var write = db.deferred(executor).write(
        CollectionsView.repeating(people), KeysView.repeating(key), StringsView.of(values.bytes, values.offsets)
    )
    _ = write.wait()
    assert_true(write.status() == Status.SUCCESS)

    var arena = Arena(executor)
    var read = db.deferred(executor).read(CollectionsView.repeating(people), KeysView.repeating(key), arena)
    assert_equal(_text(read.wait().values()[0]), "deferred")


async def _read_both(
    east: Database,
    west: Database,
    east_executor: Executor,
    west_executor: Executor,
    east_people: CollectionID,
    west_people: CollectionID,
) raises -> String:
    """Reads one key from two databases at once, each on its own executor and arena, and joins the values."""
    var key = Key(5)
    var east_arena = Arena(east_executor)
    var west_arena = Arena(west_executor)
    var east_call = east.deferred(east_executor).read(
        CollectionsView.repeating(east_people), KeysView.repeating(key), east_arena
    )
    var west_call = west.deferred(west_executor).read(
        CollectionsView.repeating(west_people), KeysView.repeating(key), west_arena
    )
    ref east_values = await east_call
    ref west_values = await west_call
    return _text(east_values.values()[0]) + _text(west_values.values()[0])


def test_awaited_fan_out_answers_both_databases() raises:
    """Two deferred reads submitted before either is awaited both answer inside one task."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var east = Database.init_ram(Executor(library))
    var west = Database.init_ram(Executor(library))
    var east_people = east.collection_create("people", Modality.BLOBS)
    var west_people = west.collection_create("people", Modality.BLOBS)
    var key = Key(5)
    var east_values = _Tape(["east"])
    var west_values = _Tape(["west"])
    east.write(
        CollectionsView.repeating(east_people),
        KeysView.repeating(key),
        StringsView.of(east_values.bytes, east_values.offsets),
    )
    west.write(
        CollectionsView.repeating(west_people),
        KeysView.repeating(key),
        StringsView.of(west_values.bytes, west_values.offsets),
    )
    var east_executor = Executor.for_ram(library, threads=2)
    var west_executor = Executor.for_ram(library, threads=2)
    var both = create_raising_task(_read_both(east, west, east_executor, west_executor, east_people, west_people))
    assert_equal(both^.wait(), "eastwest")


async def _finish_unopened(db: Database, executor: Executor) raises -> Int:
    """Awaits the finish of a plan no compaction opened and answers the status code it failed with."""
    var call = db.deferred(executor).compact_plan_finish(DatabaseCompactPlan())
    try:
        _ = await call
    except error:
        return Int(error.status.code)
    return 0


def test_awaited_failures_raise() raises:
    """A deferred call the engine refuses raises its failure through `await` and the task's wait."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var executor = Executor.for_ram(library, threads=2)
    var db = Database.init_ram(Executor(library))
    var failing = create_raising_task(_finish_unopened(db, executor))
    assert_equal(failing^.wait(), Int(Status.INVALID_ARGUMENT.code), "the refusal reaches the awaiting task")


def test_arenas_free_before_their_executor() raises:
    """An arena views the executor that made it, so the executor outlives it past its own last use."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var key = Key(5)
    var values = _Tape(["kept"])
    db.write(CollectionsView.repeating(people), KeysView.repeating(key), StringsView.of(values.bytes, values.offsets))
    var executor = Executor.for_ram(library, threads=2)
    var arena = Arena(executor)
    for _ in range(2):
        var read = db.read(CollectionsView.repeating(people), KeysView.repeating(key), arena)
        assert_equal(_text(read.values()[0]), "kept", "the arena serves calls after its executor's last use")


@fieldwise_init
struct _Holder:
    """What a predicate writes through, on the executor its own scan holds, and where the write's status lands."""

    var database: Pointer[Database, MutUntrackedOrigin]
    var executor: Pointer[Executor, MutUntrackedOrigin]
    var collection: CollectionID
    var refused: Pointer[Status, MutUntrackedOrigin]


def _write_on_the_held_executor(
    keys: Span[Key, MutAnyOrigin],
    admissions: Span[Admission, MutAnyOrigin],
    holder: Pointer[_Holder, ImmUntrackedOrigin],
):
    """A predicate admitting every key, which writes on the executor its own scan holds."""
    for index in range(len(keys)):
        admissions[index] = Admission.ADMIT
    var key = Key(9)
    var values = _Tape(["inner"])
    try:
        holder[].database[].on(holder[].executor[]).write(
            CollectionsView.repeating(holder[].collection),
            KeysView.repeating(key),
            StringsView.of(values.bytes, values.offsets),
        )
        holder[].refused[] = Status.SUCCESS
    except error:
        holder[].refused[] = error.status


def test_a_held_executor_refuses_the_next_call() raises:
    """A call finding another one holding its executor, here from that call's own predicate, finishes `CONTENDED`
    without running, and the executor serves the next call once the holder returns."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var executor = Executor.for_ram(library, threads=1)
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var key = Key(1)
    var values = _Tape(["held"])
    db.write(CollectionsView.repeating(people), KeysView.repeating(key), StringsView.of(values.bytes, values.offsets))

    var refused = Status.DEFERRED
    var holder = _Holder(
        Pointer(to=db).unsafe_origin_cast[MutUntrackedOrigin](),
        Pointer(to=executor).unsafe_origin_cast[MutUntrackedOrigin](),
        people,
        Pointer(to=refused).unsafe_origin_cast[MutUntrackedOrigin](),
    )
    var arena = Arena(executor)
    var start = Key(0)
    var scanned = db.on(executor).scan(
        CollectionsView.repeating(people),
        KeysView.repeating(start),
        10,
        arena,
        BlobsScanConfig(predicate=Predicate.batch[_write_on_the_held_executor](Pointer(to=holder))),
    )
    assert_true(refused == Status.CONTENDED, "a call on a held executor must finish contended")
    assert_equal(len(scanned.run(0)), 1)
    var inner = Key(9)
    db.on(executor).write(
        CollectionsView.repeating(people), KeysView.repeating(inner), StringsView.of(values.bytes, values.offsets)
    )


def test_a_drain_frees_the_executor_in_either_mood() raises:
    """A drain only waits until no call holds the executor, so the next call on it runs, whichever mood it left."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var executor = Executor.for_ram(library, threads=2)
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var key = Key(5)
    var values = _Tape(["drained"])
    var arena = Arena(executor)
    var moods: List[Mood] = [Mood.SPINNING, Mood.PARKED]
    for mood in moods:
        var write = db.deferred(executor).write(
            CollectionsView.repeating(people), KeysView.repeating(key), StringsView.of(values.bytes, values.offsets)
        )
        executor.drain(mood)
        var read = db.on(executor).read(CollectionsView.repeating(people), KeysView.repeating(key), arena)
        assert_equal(_text(read.values()[0]), "drained")
        _ = write.wait()


def _stop_at_first_report(finished: Int, total: Int, reports: Pointer[Int, MutAnyOrigin]) -> Continuation:
    """A progress handler counting its reports and cancelling at the first."""
    reports[] += 1
    return Continuation.CANCEL


def test_progress_reports_and_cancels() raises:
    """A handler answering `CANCEL` stops the scan, which finishes `CANCELLED`."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var keys: List[Key] = [Key(1), Key(2), Key(3), Key(4)]
    var values = _Tape(["a", "b", "c", "d"])
    db.write(CollectionsView.repeating(people), KeysView.of(keys), StringsView.of(values.bytes, values.offsets))

    var arena = Arena(library)
    var reports = 0
    var refused = Status.SUCCESS
    try:
        _ = db.scan(
            CollectionsView.repeating(people),
            KeysView.of(keys),
            10,
            arena,
            BlobsScanConfig(progress=Progress.of[_stop_at_first_report](Pointer(to=reports))),
        )
    except error:
        refused = error.status
    assert_true(refused == Status.CANCELLED, "a cancelling handler must stop the scan")
    assert_equal(reports, 1)


def test_compact_plans_stay_live_until_finished() raises:
    """A cancelled compaction leaves its plan live, a deferred one continues it to the end, and a finish frees it."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var executor = Executor.for_ram(library, threads=2)
    var db = Database.init_ram(Executor(library))
    var keys: List[Key] = [Key(1), Key(2)]
    var values = _Tape(["a", "b"])
    for name in ["first", "second"]:
        var collection = db.collection_create(name, Modality.BLOBS)
        db.write(CollectionsView.repeating(collection), KeysView.of(keys), StringsView.of(values.bytes, values.offsets))
        db.write(CollectionsView.repeating(collection), KeysView.of(keys), StringsView.of(values.bytes, values.offsets))

    var plan = DatabaseCompactPlan()
    var phase = PhaseProgress()
    var reports = 0
    var refused = Status.SUCCESS
    try:
        db.on(executor).compact(
            Progress.of[_stop_at_first_report](Pointer(to=reports)), Pointer(to=plan), Pointer(to=phase)
        )
    except error:
        refused = error.status
    assert_true(refused == Status.CANCELLED, "a cancelling handler must stop the compaction")
    assert_true(plan.address != 0, "the cancelled call leaves its plan live")

    var continued = db.deferred(executor).compact(plan=Pointer(to=plan), phase=Pointer(to=phase))
    _ = continued.wait()
    assert_true(phase.phase == Phase.DONE, "the continued plan runs to its end")
    var finished = db.deferred(executor).compact_plan_finish(plan^)
    _ = finished.wait()

    var again = Status.SUCCESS
    try:
        db.on(executor).compact_plan_finish(DatabaseCompactPlan())
    except error:
        again = error.status
    assert_true(again == Status.INVALID_ARGUMENT, "a plan no compaction opened is refused")
    _ = db^  # the database a plan names outlives its finish


def _is_even(key: Key, remainder: Pointer[UInt8, ImmUntrackedOrigin]) -> Bool:
    """Whether the key's integer leaves `remainder` over two, reading the state only."""
    return key.uuid.bytes[15] % 2 == remainder[]


def _admit_even(
    keys: Span[Key, MutAnyOrigin],
    admissions: Span[Admission, MutAnyOrigin],
    remainder: Pointer[UInt8, ImmUntrackedOrigin],
):
    """A predicate admitting the keys whose integer leaves `remainder` over two, reading the state only."""
    for index in range(len(keys)):
        admissions[index] = Admission.ADMIT if _is_even(keys[index], remainder) else Admission.REJECT


def test_predicate_filters_a_scan() raises:
    """A scan lists only the keys its per-key or batch predicate admits."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var keys: List[Key] = [Key(1), Key(2), Key(3), Key(4), Key(5), Key(6)]
    var values = _Tape(["a", "b", "c", "d", "e", "f"])
    db.write(CollectionsView.repeating(people), KeysView.of(keys), StringsView.of(values.bytes, values.offsets))

    var arena = Arena(library)
    var remainder = UInt8(0)
    var start = Key(0)
    var predicates = [
        Predicate.each[_is_even](Pointer(to=remainder)),
        Predicate.batch[_admit_even](Pointer(to=remainder)),
    ]
    assert_true(predicates[0].cost == PredicateCost.CHEAP, "a per-key predicate is cheap")
    assert_true(predicates[1].cost == PredicateCost.EXPENSIVE, "and a batch one expensive unless told otherwise")
    for predicate in predicates:
        var admitted = db.scan(
            CollectionsView.repeating(people),
            KeysView.repeating(start),
            10,
            arena,
            BlobsScanConfig(predicate=predicate),
        )
        var run = admitted.run(0)
        assert_equal(len(run), 3)
        assert_true(run[0] == Key(2) and run[1] == Key(4) and run[2] == Key(6))


def test_transactions_commit_their_writes() raises:
    """A write inside a transaction is invisible at the head until the commit lands it."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var key = Key(3)
    var values = _Tape(["pending"])
    var arena = Arena(library)
    var transaction = db.transaction()
    transaction.write(
        CollectionsView.repeating(people), KeysView.repeating(key), StringsView.of(values.bytes, values.offsets)
    )
    assert_false(db.read(CollectionsView.repeating(people), KeysView.repeating(key), arena).present(0))
    var stamp = transaction.commit()
    assert_true(stamp.value > 0)
    assert_true(db.read(CollectionsView.repeating(people), KeysView.repeating(key), arena).present(0))


def test_databases_open_at_a_directory_and_keep_what_they_held() raises:
    """A database served from a directory reloads what it held when it closed."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var directory = mkdtemp()
    var library = Library()
    var key = Key(8)
    var values = _Tape(["kept"])
    var db = Database.open_ram(Executor(library), directory)
    var collection = db.collection_create("", Modality.BLOBS)
    db.write(
        CollectionsView.repeating(collection), KeysView.repeating(key), StringsView.of(values.bytes, values.offsets)
    )
    db.flush()
    _ = db^
    var reopened = Database.open_ram(Executor(library), directory)
    var arena = Arena(library)
    var read = reopened.read(CollectionsView.repeating(collection), KeysView.repeating(key), arena)
    assert_equal(_text(read.values()[0]), "kept")
    _ = reopened^
    for name in listdir(directory):
        remove(directory + "/" + name)
    rmdir(directory)


def test_arrow_exports_read_in_place() raises:
    """Scanned keys export as Arrow and read back as a call's keys, and a read's tape and a gathered column export."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var keys: List[Key] = [Key(1), Key(2), Key(3)]
    var values = _Tape(["Ada", "Grace", "Barbara"])
    db.write(CollectionsView.repeating(people), KeysView.of(keys), StringsView.of(values.bytes, values.offsets))
    var arena = Arena(library)
    var start = Key(0)
    var scanned = db.scan(CollectionsView.repeating(people), KeysView.repeating(start), 10, arena)
    var listed = scanned.keys()
    var exported = keys_to_arrow(KeysView.of(listed), arena)
    assert_equal(len(exported), 3)
    assert_equal(exported.device_type(), 1)
    var back = keys_from_arrow(arena, exported.schema_address(), exported.array_address())
    assert_equal(len(back), 3)
    # Mojo frees a value at its last use, so the export lives past the view that reads its buffers
    _ = exported^
    var read = db.read(CollectionsView.repeating(people), KeysView.of(keys), arena)
    if not CompilationTarget.is_x86():
        assert_equal(len(tape_to_arrow(read.values())), 3)

    var movies = db.collection_create("movies", Modality.DOCS)
    var key = Key(1)
    var documents = _Tape(['{"year":1979}'])
    db.docs_write(
        CollectionsView.repeating(movies), KeysView.repeating(key), StringsView.of(documents.bytes, documents.offsets)
    )
    var fields = _Tape(["year"])
    var types: List[FieldType] = [FieldType.I64]
    var gathered = db.docs_gather(
        CollectionsView.repeating(movies),
        KeysView.repeating(key),
        StringsView.of(fields.bytes, fields.offsets),
        types,
        arena,
    )
    assert_equal(len(column_to_arrow(gathered.column(0), FieldType.I64)), 1)


def test_a_cpu_class_ceiling_the_machine_lacks_narrows_the_pick() raises:
    """A ceiling caps the class the database runs as: one the CPU lacks narrows the pick rather than refusing it."""
    if not _library_is_present():
        _skipped(library_missing_k)
        return
    var library = Library()
    var machine = Database.init_ram(Executor(library)).describe().cpu_class
    # A monoversioned build compiles no class under any other, so it refuses a lower ceiling
    if machine == CPUClass.MONOVERSIONED:
        _skipped("built without USTORE_MULTIVERSIONED")
        return
    var capped = Database.init_ram(Executor(library), cpu_class_ceiling=machine)
    assert_true(capped.describe().cpu_class == machine)
    # Power10 and AWS G5 share no architecture, so one of them is foreign to every machine
    var foreign = CPUClass.AWS_G5 if machine == CPUClass.IBM_POWER10 else CPUClass.IBM_POWER10
    var narrowed = Database.init_ram(Executor(library), cpu_class_ceiling=foreign).describe().cpu_class
    assert_false(narrowed == foreign)
    assert_false(narrowed == CPUClass.UNKNOWN)


# endregion Engine


def main() raises:
    var suite = TestSuite()
    suite.test[test_keys_spell_integers_big_endian]()
    suite.test[test_enums_spell_and_parse_as_the_library_names_them]()
    suite.test[test_blobs_write_read_and_scan]()
    suite.test[test_a_later_call_retires_earlier_results]()
    suite.test[test_strings_map_onto_keys]()
    suite.test[test_docs_write_and_read]()
    suite.test[test_graph_edges_round_trip]()
    suite.test[test_deferred_calls_match_blocking_ones]()
    suite.test[test_awaited_fan_out_answers_both_databases]()
    suite.test[test_awaited_failures_raise]()
    suite.test[test_arenas_free_before_their_executor]()
    suite.test[test_a_held_executor_refuses_the_next_call]()
    suite.test[test_a_drain_frees_the_executor_in_either_mood]()
    suite.test[test_progress_reports_and_cancels]()
    suite.test[test_compact_plans_stay_live_until_finished]()
    suite.test[test_predicate_filters_a_scan]()
    suite.test[test_transactions_commit_their_writes]()
    suite.test[test_databases_open_at_a_directory_and_keep_what_they_held]()
    suite.test[test_arrow_exports_read_in_place]()
    suite.test[test_a_cpu_class_ceiling_the_machine_lacks_narrows_the_pick]()
    suite^.run()
