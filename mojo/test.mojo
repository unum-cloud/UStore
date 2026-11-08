"""The Mojo binding's tests, registered in `main` since Mojo has no test discovery.

The pure-logic group needs no engine. The rest open in-memory databases, one of them served from a
temporary directory, so they touch no raw drive and no device, and are skipped when nothing is on
the loader path.

File: mojo/test.mojo
Author: Ash Vardanian
Date: November 8, 2026
"""

from std.os import listdir, remove, rmdir
from std.tempfile import mkdtemp
from std.testing import TestSuite, assert_equal, assert_false, assert_true

from ustore import (
    Admission,
    Arena,
    BlobsScanConfig,
    CollectionsView,
    Continuation,
    Database,
    DocsReadConfig,
    Error,
    Executor,
    Key,
    KeysView,
    Library,
    Modality,
    Predicate,
    Progress,
    Residency,
    StringsView,
    Status,
    UUID,
    VertexRole,
    VertexRolesView,
)

# region Helpers


struct Tape:
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


def _engine_is_present() -> Bool:
    """Whether a library is on the loader path, so the engine checks can run."""
    try:
        _ = Library()
        return True
    except:
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


def test_library_reports_its_version_and_names() raises:
    """The engine reports major version 1 and spells its enumerators both ways."""
    if not _engine_is_present():
        print("skipped: no UStore library on the loader path")
        return
    var library = Library()
    assert_equal(library.version()[0], 1)
    assert_equal(library.status_name(Status.NOT_FOUND), "not-found")
    assert_true(library.status_from_name("not-found") == Status.NOT_FOUND)
    assert_equal(library.modality_name(Modality.GRAPH), "graph")


def test_blobs_write_read_and_scan() raises:
    """Values written in one batch read back in key order, a missing key reads absent, and a scan lists them."""
    if not _engine_is_present():
        print("skipped: no UStore library on the loader path")
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var keys: List[Key] = [Key(1), Key(2), Key(3)]
    var values = Tape(["Ada", "Grace", "Barbara"])
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
    if not _engine_is_present():
        print("skipped: no UStore library on the loader path")
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("", Modality.BLOBS)
    var key = Key(1)
    var values = Tape(["one"])
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
    if not _engine_is_present():
        print("skipped: no UStore library on the loader path")
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var names = db.collection_create("names", Modality.STRINGS)
    var strings = Tape(["apple", "apricot", "banana"])
    var keys: List[Key] = [Key(10), Key(11), Key(12)]
    db.strings_write(CollectionsView.repeating(names), StringsView.of(strings.bytes, strings.offsets), KeysView.of(keys))

    var arena = Arena(library)
    var asked = Tape(["apricot", "cherry"])
    var found = db.strings_read(CollectionsView.repeating(names), StringsView.of(asked.bytes, asked.offsets), arena)
    assert_true(found.present(0))
    assert_false(found.present(1))
    assert_true(found.keys()[0] == Key(11))

    var prefix = Tape(["ap"])
    var listed = db.strings_scan(CollectionsView.repeating(names), StringsView.of(prefix.bytes, prefix.offsets), 10, arena)
    var tape = listed.strings()
    assert_equal(len(tape), 2)
    assert_equal(_text(tape[0]), "apple")
    assert_equal(_text(tape[1]), "apricot")


def test_docs_write_and_read() raises:
    """A JSON document reads back whole, and one field of it reads as text."""
    if not _engine_is_present():
        print("skipped: no UStore library on the loader path")
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var movies = db.collection_create("movies", Modality.DOCS)
    var key = Key(1)
    var documents = Tape(['{"title":"Alien","year":1979}'])
    db.docs_write(CollectionsView.repeating(movies), KeysView.repeating(key), StringsView.of(documents.bytes, documents.offsets))

    var arena = Arena(library)
    var whole = db.docs_read(CollectionsView.repeating(movies), KeysView.repeating(key), arena)
    assert_true(whole.present(0))
    assert_true("Alien" in _text(whole.values()[0]))

    var fields = Tape(["/year"])
    var year = db.docs_read(
        CollectionsView.repeating(movies), KeysView.repeating(key), arena, DocsReadConfig(fields=StringsView.of(fields.bytes, fields.offsets))
    )
    assert_equal(_text(year.values()[0]), "1979")


def test_graph_edges_round_trip() raises:
    """An edge written from one vertex to another reads back from both ends."""
    if not _engine_is_present():
        print("skipped: no UStore library on the loader path")
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
    if not _engine_is_present():
        print("skipped: no UStore library on the loader path")
        return
    var library = Library()
    var executor = Executor.for_ram(library, threads=2)
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var key = Key(5)
    var values = Tape(["deferred"])
    var write = db.deferred(executor).write(
        CollectionsView.repeating(people), KeysView.repeating(key), StringsView.of(values.bytes, values.offsets)
    )
    _ = write.wait()
    assert_true(write.status() == Status.SUCCESS)

    var arena = Arena(executor)
    var read = db.deferred(executor).read(CollectionsView.repeating(people), KeysView.repeating(key), arena)
    assert_equal(_text(read.wait().values()[0]), "deferred")


def _stop_at_first_report(finished: Int, total: Int, reports: Pointer[Int, MutAnyOrigin]) -> Continuation:
    """A progress handler counting its reports and cancelling at the first."""
    reports[] += 1
    return Continuation.CANCEL


def test_progress_cancels_a_scan() raises:
    """A handler answering `CANCEL` stops the scan, which finishes `CANCELLED`."""
    if not _engine_is_present():
        print("skipped: no UStore library on the loader path")
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var keys: List[Key] = [Key(1), Key(2), Key(3), Key(4)]
    var values = Tape(["a", "b", "c", "d"])
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


def _admit_even(keys: Span[Key, MutAnyOrigin], admissions: Span[Admission, MutAnyOrigin], asked: Pointer[Int, MutAnyOrigin]):
    """A predicate admitting the keys whose integer is even, counting the keys it was asked about."""
    for index in range(len(keys)):
        asked[] += 1
        admissions[index] = Admission.ADMIT if keys[index].uuid.bytes[15] % 2 == 0 else Admission.REJECT


def test_predicate_filters_a_scan() raises:
    """A scan lists only the keys its predicate admits."""
    if not _engine_is_present():
        print("skipped: no UStore library on the loader path")
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var keys: List[Key] = [Key(1), Key(2), Key(3), Key(4), Key(5), Key(6)]
    var values = Tape(["a", "b", "c", "d", "e", "f"])
    db.write(CollectionsView.repeating(people), KeysView.of(keys), StringsView.of(values.bytes, values.offsets))

    var arena = Arena(library)
    var asked = 0
    var start = Key(0)
    var admitted = db.scan(
        CollectionsView.repeating(people),
        KeysView.repeating(start),
        10,
        arena,
        BlobsScanConfig(predicate=Predicate.of[_admit_even](Pointer(to=asked))),
    )
    var run = admitted.run(0)
    assert_equal(len(run), 3)
    assert_true(run[0] == Key(2) and run[1] == Key(4) and run[2] == Key(6))
    assert_true(asked >= 6)


def test_transactions_commit_their_writes() raises:
    """A write inside a transaction is invisible at the head until the commit lands it."""
    if not _engine_is_present():
        print("skipped: no UStore library on the loader path")
        return
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var key = Key(3)
    var values = Tape(["pending"])
    var arena = Arena(library)
    var transaction = db.transaction()
    transaction.write(CollectionsView.repeating(people), KeysView.repeating(key), StringsView.of(values.bytes, values.offsets))
    assert_false(db.read(CollectionsView.repeating(people), KeysView.repeating(key), arena).present(0))
    var stamp = transaction.commit()
    assert_true(stamp.value > 0)
    assert_true(db.read(CollectionsView.repeating(people), KeysView.repeating(key), arena).present(0))


def test_open_ram_persists_across_reopens() raises:
    """A database served from a directory reloads what it held when it closed."""
    if not _engine_is_present():
        print("skipped: no UStore library on the loader path")
        return
    var directory = mkdtemp()
    var library = Library()
    var key = Key(8)
    var values = Tape(["kept"])
    var db = Database.open_ram(Executor(library), directory)
    var collection = db.collection_create("", Modality.BLOBS)
    db.write(CollectionsView.repeating(collection), KeysView.repeating(key), StringsView.of(values.bytes, values.offsets))
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


# endregion Engine


def main() raises:
    var suite = TestSuite()
    suite.test[test_keys_spell_integers_big_endian]()
    suite.test[test_library_reports_its_version_and_names]()
    suite.test[test_blobs_write_read_and_scan]()
    suite.test[test_a_later_call_retires_earlier_results]()
    suite.test[test_strings_map_onto_keys]()
    suite.test[test_docs_write_and_read]()
    suite.test[test_graph_edges_round_trip]()
    suite.test[test_deferred_calls_match_blocking_ones]()
    suite.test[test_progress_cancels_a_scan]()
    suite.test[test_predicate_filters_a_scan]()
    suite.test[test_transactions_commit_their_writes]()
    suite.test[test_open_ram_persists_across_reopens]()
    suite^.run()
