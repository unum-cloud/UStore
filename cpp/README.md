# UStore for C++

`cpp/` holds the C++ wrappers of UStore's public C SDK.
The `*.hpp` headers of `cpp/ustore/` wrap the same calls in C++ with RAII handles, batched references and typed ranges, and add no ABI of their own.
UStore's sources split into four folders:

- `c/`: the public C SDK every wrapper forwards to, the C ABI in `c/ustore/*.h`, with [its own guide](https://github.com/unum-cloud/UStore/blob/main/c/README.md), whose [Installation](https://github.com/unum-cloud/UStore/blob/main/c/README.md#installation) covers the wrappers too.
- `cpp/`: these wrappers in `cpp/ustore/*.hpp`, in `unum::ustore`, and this guide.
- `include/`: the first-party classes and templates that advanced users may reuse to build their own low-level stores, each subfolder `ustore/<dir>/` opening `unum::ustore::<dir>`, with [its own guide](../include/ustore/README.md).
- `src/`: UStore's own instantiations of those templates, the engines behind the C ABI.

`c/` and `cpp/` are both include roots, so every header is spelled `<ustore/...>` in the tree and once installed.
`cpp/ustore/` holds one header per part of the wrappers:

- `ustore.hpp`: every C++ header below.
- `db.hpp`: `database_t`, `transaction_t`, `snapshot_t` and receivers.
- `blobs_collection.hpp`: binary collections and their deferred twins.
- `blobs_ref.hpp`: batched references to binary values.
- `blobs_range.hpp`: cursors over keys, the streams built on them, and key ranges.
- `docs_collection.hpp`: document collections and their deferred twins.
- `docs_ref.hpp`: batched references to documents.
- `docs_table.hpp`: gathered tables.
- `graph_collection.hpp`: graph collections and their deferred twins.
- `graph_stream.hpp`: adjacency streams.
- `strings_collection.hpp`: string-keyed collections, cursors, prefix walks and deferred twins.
- `status.hpp`: `deferred`, and `status_code_t`, `status_t` and `expected` re-exported from `ustore/utilities/types.hpp`.
- `types.hpp`: arenas, stamps, receivers and executors, and the keys, edges and value views re-exported from `ustore/utilities/types.hpp`.
- `ranges.hpp`: strided ranges, argument adapters, read views and tapes.

## Conventions

Everything lives in `unum::ustore`, and `#include <ustore/ustore.hpp>` brings in all of it.
Fallible calls return a `status_t`, a `status_code_t` beside a static message, or an `expected` that holds either a value or the `status_t` explaining its absence; both are `[[nodiscard]]`.
Dereferencing an `expected<value_type>` yields the `value_type`, and `auto [value, status]` decomposes one whose value is default constructible.
`key_from_integer` spells a non-negative integer as a key, and `key_after` answers the key right after another.
Nothing copies values out of the arena, and nothing throws.

```cpp
using namespace unum::ustore;

auto [db, status] = database_t::init_ram();
if (status.failed())
    std::fprintf(stderr, "%s\n", status.message);
```

`database_t::init_ram`, `open_ram`, `open_rocksdb`, `view_rocksdb`, `open_leveldb` and `open_nvme` wrap [the C opens](https://github.com/unum-cloud/UStore/blob/main/c/README.md#opening-the-database) of the same names, taking their argument structs as options.

## Blobs

A collection handle is cheap to construct and holds no state, and its `[]` operator returns a `blobs_ref` over one key or a batch of keys that reads and writes through the C interface:

```cpp
blobs_collection_t main = db.main();

status_t status = main[key_from_integer(42)].assign("purpose of life");
expected<owned_value_t> value = main[key_from_integer(42)].value();
bool const matches = value && *value == "purpose of life";

status = main[{key_from_integer(43), key_from_integer(44)}].assign("same value");
expected<bool> present = main[key_from_integer(43)].present();
status = main[key_from_integer(43)].remove();

auto [arena, arena_status] = arena_t::make();
std::array<ustore_key_t, 3> keys {key_from_integer(65), key_from_integer(66), key_from_integer(67)};
expected<values_view_t> values = main.read(keys, arena);
for (value_view_t found : *values)
    std::printf("%zu bytes\n", found.size());
```

Read without an arena, `value`, `present` and `length` are shorthand: a single key answers an `owned_value_t`, a `bool` or a length, and a batch an `owned<values_view_t>`, an owned `bits_view_t` or owned `lengths_view_t`.
Given an `arena_t&`, as `main.read(keys, arena)` or `main[keys].value(arena)`, `.present(arena)` and `.length(arena)` are, the same reads answer plain views into that arena.
`remove` removes the keys, while `clear_values` keeps them and empties their values.
`keys` and `pairs` range over a collection in key order from a `start_key` to an `end_key`, and `size_range` estimates how many keys it holds.
Each stream is shorthand for a range-based loop, which has no place for an arena, so it pulls pages from a cursor into one arena it owns for its lifetime.

## Arenas

Shorthand and verbs trade convenience for reuse, as in every binding:

- __Shorthand makes a one-off arena.__
  `main[key].value()` calls `ustore_arena_init`, reads into that arena, and hands it to its answer, which frees it on destruction, so nothing is copied and no handle keeps state.
  A scalar answer, such as a presence or a degree, is copied out and the arena freed at once.
  The price is one arena per call, so a loop reads through a verb instead.
- __Verbs reuse a caller's arena.__
  `arena_t::make(executor)` places one where its executor computes, or on host pages for calls on the calling thread, and a loop passing it to every call allocates once.
  Every call rewinds it, so its views last until the next call given that arena; to keep them longer, pass that call another arena.

```cpp
auto [arena, arena_status] = arena_t::make(executor);
for (std::span<ustore_key_t const> batch : batches) {
    auto [values, read_status] = db.on(executor).main().read(batch, arena);
    if (read_status.failed()) break;
    consume(values); // valid until the next call given `arena`
}
```

An `owned_value_t` derives from the `value_view_t` it holds, so it compares and converts like one, and `view()` slices that plain view off.
Collections, transactions and snapshots hold no arena, so threads share one handle, each passing its own arena.

Walks split the same way: the `keys`, `pairs` and `prefixed` streams own one arena each, while `blobs_cursor_t` and `strings_cursor_t` take the caller's on every pull, so a loop reuses one arena it keeps:

```cpp
auto [cursor, cursor_status] = blobs_cursor_t::make(db.receiver(), ustore_collection_main_k);
std::size_t const wanted = 256;
for (std::size_t pulled = wanted; cursor_status.succeeded() && pulled == wanted;) {
    auto [page, page_status] = cursor.next_pairs(wanted, arena);
    if (page_status.failed()) break;
    consume(page.values); // valid until the next call given `arena`
    pulled = page.keys.size();
}
```

`make` opens the walk at the view its receiver reads and from an optional start key, `next` pulls keys alone and `next_pairs` keys with their values, and a page shorter than `wanted` exhausts the walk for good.
`strings_cursor_t::make` takes a prefix and an optional start string, and its `next` pulls strings in the order of their text.

## Named Collections and Transactions

```cpp
expected<docs_collection_t> people = db.find_or_create<docs_collection_t>("people");

expected<transaction_t> transaction = db.transaction();
blobs_collection_t inside = transaction->main();
status = inside[key_from_integer(42)].assign("written inside a transaction");
expected<commit_stamp_t> committed = transaction->commit();

expected<snapshot_t> snapshot = db.snapshot();
auto pinned = snapshot->main()[key_from_integer(42)].value();
```

Each collection type names the modality it holds as `modality_k`, so `create` records it, `find` refuses a collection holding another with `status_code_t::invalid_argument_k`, and `find_or_create` creates only when no collection carries the name.
A handle taken from a transaction reads and writes through it, and a transaction commits only when `commit` is called, answering the commit's position in commit order.
A snapshot hands out the read-only twin of every collection, such as `docs_read_collection_t` for `find<docs_collection_t>`, so nothing writes through it, and `save(path)` writes it out as a new database.

## Describing, Clearing and Compacting

`describe`, `clear` and `compact` wrap [the C calls](https://github.com/unum-cloud/UStore/blob/main/c/README.md#describing-clearing-and-compacting) of the same names, and a save or a compaction takes an optional progress callable:

```cpp
expected<ustore_database_description_t> description = db.describe();
if (description)
    std::printf("%s holds %llu keys\n", ustore_engine_name(description->engine),
                (unsigned long long)description->keys);
status = db.clear(ustore_drop_pairs_k);

ustore_size_t const budget = 1ull << 30;
auto within_budget = [&](ustore_size_t finished, ustore_size_t total) noexcept {
    std::fprintf(stderr, "saved %llu of %llu bytes\n", (unsigned long long)finished, (unsigned long long)total);
    return finished <= budget ? ustore_continue_k : ustore_cancel_k;
};
status = snapshot->save("/var/backups/ustore", within_budget);
status = db.compact();
```

`clear` empties every collection as its `ustore_drop_mode_t` names, so `ustore_drop_collection_k` drops every named collection, which the `ram` and RocksDB engines refuse while a snapshot is open.
A progress callable answers `ustore_continue_k` or `ustore_cancel_k` and is `noexcept`, so one that may throw does not compile, and the call borrows it until it is final, so a temporary does not compile either.

## Documents

A `docs_collection_t` hands out `docs_ref` proxies, which read and write JSON through `ustore_docs_read` and `ustore_docs_write`:

```cpp
docs_collection_t people = db.main<docs_collection_t>();
status_t status = people[key_from_integer(1)].assign(R"({"person": "Alice", "age": 27, "height": 1})");
status = people[key_from_integer(2)].assign(R"({"person": "Bob", "age": "27", "weight": 2})");

expected<owned_value_t> document = people[key_from_integer(1)].value();
expected<owned_value_t> person = people[collection_key_field_t {key_from_integer(1), "/person"}].value(ustore_docs_field_type_str_k);

status = people[key_from_integer(1)].patch(R"([{"op": "replace", "path": "/age", "value": 28}])");
status = people[key_from_integer(2)].merge(R"({"weight": null})");
expected<values_view_t> documents = people[{key_from_integer(1), key_from_integer(2)}].value(arena);
expected<sz::strings_view_t> paths = people[{key_from_integer(1), key_from_integer(2)}].gist(arena);
```

Field access through a proxy is shorthand like any other read without an arena, while `gist` and `gather` export into the arena they are given.
Besides `assign`, a reference offers `insert`, `update` and `upsert`, and `as` switches the format every later call exchanges.

## Tables

`gather` reads fields of many documents into a `docs_table`, converting each field into its column type on a best-effort basis.
A header whose column types are known at compile time is built column by column:

```cpp
auto header = make_table_header() //
                  .with<std::int32_t>("age")
                  .with<std::string_view>("age")
                  .with<std::string_view>("person");

std::array<ustore_key_t, 3> keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
auto table = people[keys].gather(header, arena);
auto ages = table->column<0>();
for (std::size_t row = 0; row != ages.size(); ++row)
    if (ages[row].valid)
        std::printf("%d\n", ages[row].value);
```

A header whose column types are known only at runtime views an array of `field_type_t`, and its columns are typed at the point of use:

```cpp
field_type_t const columns[] = {
    {"age", ustore_docs_field_type_i32_k},
    {"age", ustore_docs_field_type_str_k},
    {"person", ustore_docs_field_type_str_k},
};
auto dynamic = people[keys].gather(make_table_header(columns), arena);
auto names = dynamic->column(2).as<std::string_view>();
```

A table is a view: one `ustore_docs_column_view_t` per column lives in the arena, while its index and header view the keys and the header the gather was given.
Every cell carries `valid`, `converted` and `collides` flags beside its `value`, which is meaningful only when `valid`.

## Graphs

A `graph_collection_t` batches edges and vertices, and can be used from inside a transaction like any other handle:

```cpp
auto [graph, status] = db.create<graph_collection_t>("graph");
status = graph.write_edge(edge_t {key_from_integer(1), key_from_integer(2), key_from_integer(9)});

std::array<edge_t, 2> triplets {edge_t {key_from_integer(2), key_from_integer(3), key_from_integer(10)}, edge_t {key_from_integer(3), key_from_integer(4), key_from_integer(11)}};
status = graph.write_edges(edges(triplets));

expected<ustore_vertex_degree_t> degree = graph.degree(key_from_integer(2));
expected<bool> present = graph.present(key_from_integer(2));
auto successors = graph.successors(key_from_integer(2), arena);
auto between = graph.edges_between(key_from_integer(1), key_from_integer(2), arena);
auto touching = graph.read_edges(key_from_integer(3), arena, ustore_vertex_role_target_k);

std::array<ustore_key_t, 3> vertices {key_from_integer(1), key_from_integer(7), key_from_integer(2)};
expected<edges_view_t> leaving = graph.read_edges(strided(vertices).immutable(), arena);
edges_view_t of_missing = leaving->of_vertex(1); // empty, as vertex 7 is missing
```

`degree` and `present` of one vertex are shorthand, copying a scalar out of a one-off arena, while edges, neighbors and the degrees of a batch land in the arena the call is given.
The edges of a batch come in the order of its vertices, and their `offsets` split them by vertex, so `of_vertex(i)` views the edges of vertex `i` and a missing vertex has none.
`edges` streams every edge of the collection, and `vertex_stream` every vertex, both reading ahead in batches.

## Strings

A `strings_collection_t` keys arbitrary bytes by variable-length strings, kept in the order of their text:

```cpp
auto [words, status] = db.create<strings_collection_t>("words");
status = words.assign("apple", "red");
status = words.assign("apricot", "orange");

expected<owned_value_t> apple = words.value("apple");
std::array<value_view_t, 2> const asked {"apple", "cherry"};
expected<values_view_t> found = words.read(asked, arena);
expected<sz::strings_view_t> page = words.prefixed_page("ap", 16, arena);
expected<std::size_t> count = words.size("ap");
```

`value` and `present` of one string are shorthand, while `read` and `prefixed_page` export into the arena they are given.
`prefixed` and `prefix_begin` stream every string of a prefix through one arena of their own, pulling pages from a `strings_cursor_t`.

## Receivers and Deferred Calls

The receiver chooses where a call runs, and the verb stays.
`db.on(executor)`, `transaction->on(executor)` and `snapshot->on(executor)` hand out the same collections, proxies and operators, whose calls block on the executor:

```cpp
auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 4});
blobs_collection_t on_executor = db.on(executor).main();
status = on_executor[key_from_integer(42)].assign("purpose of life");
```

`deferred(executor)` hands out the deferred twin of every collection, whose verbs mirror [the C calls](https://github.com/unum-cloud/UStore/blob/main/c/README.md#executors-blocking-and-deferred-calls) and answer a `deferred<verb>` once submitted.
`main<docs_collection_t>()` and `find<graph_collection_t>(name)` pick the twin by modality, read-only at a snapshot, while the deferred database's `[]` takes a binary collection by the identifier `create` or `find` resolved, so addressing it never blocks.
`status()` polls without blocking, `std::move(call).wait()` answers what the call produced, and a `deferred` dropped unwaited waits in its destructor:

```cpp
deferred_database_t later = db.deferred(executor);
auto [arena, arena_status] = arena_t::make(executor);
std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
std::array<sz::string_view_t, 2> const texts {R"({"person": "Alice"})", R"({"person": "Bob"})"};

sz::expected<values_tape_t> values = sz::strings_view_t(texts).gather();
if (!values) return;
deferred<blobs_write_t> writing = later.main().write(keys, std::move(values.value));
status = std::move(writing).wait();
deferred<blobs_read_t> reading = later.main().read(keys, arena);
expected<values_view_t> read = std::move(reading).wait();
for (value_view_t found : *read)
    std::printf("%zu bytes\n", found.size());
```

Documents are written and read whole or by field, under any modification, and a gather answers a table together with the rows and columns it views outside the arena:

```cpp
deferred_docs_collection_t people = later.main<docs_collection_t>();
sz::expected<values_tape_t> documents = sz::strings_view_t(texts).gather();
if (!documents) return;
status = people.write(keys, std::move(documents.value), {}, ustore_docs_modify_merge_k).wait();

std::array<sz::string_view_t, 1> const columns {"person"};
sz::expected<values_tape_t> fields = sz::strings_view_t(columns).gather();
if (!fields) return;
auto gathered = people.gather(keys, std::move(fields.value), std::array {ustore_docs_field_type_str_k}, arena).wait();
auto names = gathered->table.column(0).as<std::string_view>();
```

Graphs read edges and degrees for one role of every vertex, and write and remove edges and vertices:

```cpp
deferred_graph_collection_t graph = *later.find<graph_collection_t>("graph");
std::array<edge_t, 2> const chain {edge_t {key_from_integer(1), key_from_integer(2)}, edge_t {key_from_integer(2), key_from_integer(3)}};
status = graph.write_edges(chain).wait();
expected<edges_view_t> leaving = graph.read_edges(keys, arena, ustore_vertex_role_source_k).wait();
expected<std::span<ustore_vertex_degree_t const>> degrees = graph.degrees(keys, arena).wait();
```

Strings are written and read in batches, and a scan answers a page of strings for every prefix:

```cpp
deferred_strings_collection_t words = *later.find<strings_collection_t>("words");
std::array<sz::string_view_t, 2> const spellings {"apple", "apricot"};
sz::expected<values_tape_t> strings = sz::strings_view_t(spellings).gather();
sz::expected<values_tape_t> meanings = sz::strings_view_t(texts).gather();
sz::expected<values_tape_t> prefixes = sz::strings_view_t(std::span {spellings}.first(1)).gather();
if (!strings || !meanings || !prefixes) return;
status = words.write(std::move(strings.value), std::move(meanings.value)).wait();
expected<strings_scan_t::finished_t> scanned = words.scan(std::move(prefixes.value), 16, arena).wait();
```

A transaction defers its own verbs too, `reset` among them:

```cpp
deferred_transaction_t deferred_transaction = transaction->deferred(executor);
status = deferred_transaction.main().remove(keys).wait();
expected<read_stamp_t> staged = deferred_transaction.stage().wait();
expected<commit_stamp_t> stamp = deferred_transaction.commit().wait();
```

The deferred database also creates, drops and lists collections, and describes, clears and compacts itself, while the deferred snapshot saves itself through `save`.
Opening blocks instead, as every constructor does: `init_ram`, `open_ram`, `open_rocksdb`, `view_rocksdb` and `open_leveldb` take the executor to block on as their last argument, while `open_nvme` lends the engine the executor its options name until close:

```cpp
expected<database_t> opened = database_t::open_rocksdb({.directory = bytes_of("/var/lib/ustore")}, executor);
```

Either way the database runs its own calls on the calling thread until a receiver names an executor.

- __Deferred inputs are owned, arenas borrowed.__
  Keys, vertices, edges and column types come as a `std::array` or an `st::basic_vector`, and values, strings and fields as a `values_tape_t`, StringZilla's `sz::strings_t` that `sz::strings_view_t::gather` fills, so a borrowed span or view does not compile.
  The arena comes as an `arena_t&`, borrowed until the call is final, which is safe because freeing an arena waits for the deferred calls naming it, while a temporary arena does not compile.
- __Submission copies the arguments.__
  A `deferred<verb>` keeps one heap block that never moves, holding the outcome, the owned inputs and the outputs the call writes through, while the C argument structure dies on the stack it was filled on.
  `status()` polls through `ustore_outcome_status`, and `wait()` and the destructor wait.
- __Answers view the arena.__
  A finished read answers the views a blocking one does, valid until the next call given that arena, and a finished gather owns its rows, columns and field names beside a table whose cells stay in the arena.
- __A transaction serves one call at a time.__
  Wait on one deferred call through it before submitting the next, which otherwise finishes at once with `status_code_t::contended_k`.
- __The wrappers have no life of their own.__
  They start no threads, keep no counts and hold no hidden arena: freeing a database, transaction or snapshot waits in C for the deferred calls naming it.
