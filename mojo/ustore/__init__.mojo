"""A multi-modal transactional database engine, bound over its C ABI.

UStore keeps binary values, keys found by strings, JSON documents, graphs and pages in collections
of one database, under one transaction and one snapshot. This package binds the whole C interface
but its Arrow exchange: the database, its snapshots, transactions, collections, change feeds and
ingests, the verbs of every modality, and the executors and arenas they run on and write into.

The engine is reached by `dlopen`, so a consumer builds with no linker flags at all:

```mojo
from ustore import Arena, CollectionsView, Database, Executor, Key, KeysView, Library, Modality, StringsView

def main() raises:
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)
    var key = Key(42)
    var bytes = "Ada".as_bytes()
    var offsets: List[UInt32] = [0, 3]
    db.write(CollectionsView.repeating(people), KeysView.repeating(key), StringsView.of(bytes, offsets))
    var arena = Arena(library)
    var values = db.read(CollectionsView.repeating(people), KeysView.repeating(key), arena)
    print(len(values.values()[0]))
```

The receiver chooses where a verb runs and how it completes, and the verb stays the same: a verb on
a database, transaction or snapshot runs on the calling thread, `db.on(executor)` blocks on that
executor, and `db.deferred(executor)` submits the call there and answers a `Deferred` at once.

This package root re-exports every public symbol from the modules that mirror the C headers, so
`from ustore import X` resolves whatever module X lives in.

File: mojo/ustore/__init__.mojo
Author: Ash Vardanian
Date: November 8, 2026
"""

from ustore.blobs import (
    BlobsCursor,
    BlobsCursorConfig,
    BlobsReadConfig,
    BlobsScanConfig,
    KeyRuns,
    Measures,
    Pulled,
    Values,
)
from ustore.db import (
    Admission,
    Capabilities,
    ChangeKind,
    Changes,
    ChangesCursor,
    CollectionID,
    CollectionList,
    CollectionsFound,
    CollectionsView,
    Database,
    DatabaseDescription,
    DatabaseNvmeOptions,
    DataDirectory,
    DeferredOn,
    DropMode,
    Durability,
    Engine,
    Ingest,
    IngestOrder,
    Isolation,
    Key,
    KeysView,
    LengthsView,
    Modality,
    On,
    Predicate,
    PredicateCost,
    ScanDirection,
    ScanMode,
    Snapshot,
    Stamp,
    StridedView,
    Transaction,
    UUID,
    Watch,
)
from ustore.docs import (
    Column,
    Columns,
    DocsCursor,
    DocsCursorConfig,
    DocsReadConfig,
    DocsScanConfig,
    DocsWriteConfig,
    FieldType,
    Gist,
    Modification,
)
from ustore.graph import (
    Edges,
    GraphCursorConfig,
    GraphScanConfig,
    GraphVerticesCursor,
    VERTEX_DEGREE_MISSING,
    VertexRole,
    VertexRolesView,
)
from ustore.library import Library
from ustore.pages import (
    EffectiveKeys,
    PageEncoding,
    PagesCursor,
    PagesCursorConfig,
    PagesInsertConfig,
    PagesReadConfig,
    PagesScanConfig,
)
from ustore.strings import (
    StringKeys,
    StringRuns,
    StringsCursor,
    StringsCursorConfig,
    StringsPulled,
    StringsScanConfig,
)
from ustore.types import (
    Arena,
    ArenaDescription,
    BytesView,
    CaseSensitivity,
    Completion,
    Continuation,
    CPUClass,
    Deferred,
    Error,
    Executor,
    Gds,
    GPUClass,
    GPUDevice,
    GPUVendor,
    Mood,
    NvmeOptions,
    Patterns,
    PatternKind,
    Progress,
    RamOptions,
    Residency,
    Status,
    StringsView,
    Tape,
    Transport,
)
