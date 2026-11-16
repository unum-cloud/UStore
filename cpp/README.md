# UStore for C++

`cpp/` holds the C++ wrappers of UStore's public C SDK.
The `*.hpp` headers of `cpp/ustore/` wrap the same calls in C++20 with RAII handles, batched references and typed ranges, and add no ABI of their own.
Everything lives in `unum::ustore`, `#include <ustore/ustore.hpp>` brings in all of it, and nothing throws:

- `database_t`, `transaction_t` and `snapshot_t`: the head, a group of changes, and a point in time, each owning its C handle.
- `blobs_collection_t`, `docs_collection_t`, `graph_collection_t`, `pages_collection_t` and `strings_collection_t`: cheap handles of one collection each.
- `arena_t`: where every output whose length scales with the call lands.
- `executor_handle_t` and `deferred`: the threads a call runs on, and a call submitted there.

## Installation

The wrappers are headers over the C library, so they install, link and build exactly as [the C SDK](https://github.com/unum-cloud/UStore/blob/main/c/README.md#installation) does: `c/` and `cpp/` are both include roots, and every header is spelled `<ustore/...>` in the tree and once installed.

```cmake
FetchContent_Declare(ustore GIT_REPOSITORY https://github.com/unum-cloud/UStore.git GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ustore)
target_link_libraries(your_target PRIVATE ustore::static)
```

From a checkout, `test/compilation.cpp` calls every wrapper in each accepted form, so a change that breaks a call site fails to build:

```sh
cmake -B build_release -D CMAKE_BUILD_TYPE=Release -D USTORE_BUILD_TEST=1
cmake --build build_release --target ustore_test_compilation && ctest --test-dir build_release -R compilation
```

## Quickstart

The program opens a database in memory, writes a blob, a document and an edge in one transaction, and reads them back.
A handle taken from a transaction reads and writes through it, while collections are created at the head.

```cpp
#include <cassert>

#include <ustore/ustore.hpp>

using namespace unum::ustore;

int main() {
    auto [db, open_status] = database_t::init_ram();
    auto posters = db.on().create<blobs_collection_t>("posters");
    auto movies = db.on().create<docs_collection_t>("movies");
    auto similar = db.on().create<graph_collection_t>("similar");
    assert(open_status.succeeded() && posters && movies && similar);

    // One transaction writes a blob, a document and an edge, and commits all three or none
    ustore_key_t const avatar = key_from_integer(1), titanic = key_from_integer(2);
    auto [transaction, transaction_status] = db.on().transaction();
    transaction_ref_t const inside = transaction.on();
    status_t const poster = inside["posters"]->at(avatar).assign("\x89PNG");
    status_t const movie = inside.find<docs_collection_t>("movies")->at(avatar).assign(
        R"({"title":"Avatar","year":2009})");
    status_t const edge = inside.find<graph_collection_t>("similar")->write_edge(edge_t {avatar, titanic});
    expected<commit_stamp_t> const committed = inside.commit();
    assert(transaction_status.succeeded() && poster.succeeded() && movie.succeeded() && edge.succeeded() && committed);

    // Reads at the head see the commit: the poster, the title and the one edge leaving Avatar
    auto const stored = (*posters)[avatar].value();
    auto const title = (*movies)[collection_key_field_t {avatar, "title"}].value(ustore_docs_field_type_string_k);
    auto const degree = similar->degree(avatar, ustore_vertex_role_source_k);
    assert(stored && *stored == "\x89PNG" && title && *title == "Avatar" && degree && *degree == 1);
}
```

## Collections

No collection exists until one is created, and each collection type names the modality it holds as `modality_k`.
`create` records it and refuses a name starting with `_`, which system collections keep, `find` refuses a collection holding another modality with `status_code_t::invalid_argument_k`, and `find_or_create` creates only when no collection carries the name.
A collection handle is cheap to construct and holds no state, so threads share one, each passing its own arena.
A snapshot hands out the read-only twin of every collection, such as `docs_read_collection_t` for `find<docs_collection_t>`, so nothing writes through it.

```cpp
auto [blobs, create_status] = db.on().create<blobs_collection_t>("blobs");
expected<docs_collection_t> people = db.on().find_or_create<docs_collection_t>("people");
expected<blobs_collection_t> same = db.on().find("blobs");
expected<graph_collection_t> refused = db.on().find<graph_collection_t>("blobs"); // invalid_argument_k
blobs_collection_t named = *db.on()["articles"];                                   // found or created
```

## Reads and Writes

A blobs collection's `[]` operator returns a `blobs_ref` over one key or a batch of keys that reads and writes through the C interface.
Read without an arena, `value`, `present` and `length` are shorthand: a single key answers an `owned_value_t`, a `bool` or a length, and a batch the owned views of a one-off arena.
Given an `arena_t&`, as `blobs.read(keys, arena)` or `blobs[keys].value(arena)`, the same reads answer plain views into that arena.
`remove` removes the keys, while `clear_values` keeps them and empties their values.

```cpp
status_t status = blobs[key_from_integer(42)].assign("purpose of life");
expected<owned_value_t> value = blobs[key_from_integer(42)].value();
bool const matches = value && *value == "purpose of life";

status = blobs[{key_from_integer(43), key_from_integer(44)}].assign("same value");
expected<bool> present = blobs[key_from_integer(43)].present();
status = blobs[key_from_integer(43)].remove();

auto [arena, arena_status] = arena_t::make();
std::array<ustore_key_t, 3> keys {key_from_integer(65), key_from_integer(66), key_from_integer(67)};
expected<values_view_t> values = blobs.read(keys, arena);
for (value_view_t found : *values)
    std::printf("%zu bytes\n", found.size());
```

`ingest` opens an `ingest_t` that lands every row appended to it as one commit of its own, joining no transaction, and `finish` answers that commit's stamp.
Blobs, documents, pages and strings append keys beside values, and graphs append edges.

```cpp
expected<ingest_t> ingest = db.on().ingest(blobs, ustore_ingest_unsorted_k);
std::array<ustore_key_t, 2> rows {key_from_integer(2), key_from_integer(1)};
std::array<std::string_view, 2> bytes {"two", "one"};
status = ingest->append(rows, bytes);       // copied before the call returns
expected<commit_stamp_t> landed = ingest->finish();
```

### Strings

A `strings_collection_t` keys arbitrary bytes by variable-length strings, kept in the order of their text, where `value` and `present` of one string are shorthand and `read` and `prefixed_page` export into the arena they are given.

```cpp
auto [words, words_status] = db.on().create<strings_collection_t>("words");
status = words.assign("apple", key_from_integer(1));
status = words.assign("apricot", key_from_integer(2));
expected<ustore_key_t> apple = words.value("apple");
expected<sz::strings_view_t> page = words.prefixed_page("ap", 16, arena);
expected<std::size_t> count = words.size("ap");
```

## Scans

`keys` and `pairs` range over a collection in key order from a `start_key` to an `end_key`, and `size_range` estimates how many keys it holds.
Each stream is shorthand for a range-based loop, which has no place for an arena, so it pulls pages from a cursor into one arena it owns for its lifetime.
`blobs_cursor_t` and `strings_cursor_t` take the caller's arena on every pull instead, so a loop reuses one arena it keeps.
`make` opens the walk at the view its receiver reads and from an optional start key, `next` pulls keys alone and `next_pairs` keys with their values, and a page shorter than `wanted` exhausts the walk for good.

```cpp
auto [cursor, cursor_status] = blobs_cursor_t::make(db.receiver(), blobs);
std::size_t const wanted = 256;
for (std::size_t pulled = wanted; cursor_status.succeeded() && pulled == wanted;) {
    auto [page, page_status] = cursor.next_pairs(wanted, arena);
    if (page_status.failed()) break;
    consume(page.values); // valid until the next call given `arena`
    pulled = page.keys.size();
}
```

`strings_cursor_t::make` takes a prefix and an optional start string, and its `next` pulls strings in the order of their text.

`sample` draws entries at random and `measure` bounds how many entries and bytes a range holds, on strings, documents and pages, while graphs spell them `sample_vertices` and `measure_vertices`.
Both export into the arena they are given and answer deferred calls on a deferred handle.

```cpp
expected<std::span<ustore_key_t const>> drawn = people.sample(16, arena);
expected<size_estimates_t> bounds = people.measure(smallest_key_k, largest_key_k, arena);
expected<strings_pages_t> words_drawn = words.sample(16, arena);
expected<size_estimates_t> from_ap = words.measure("ap", value_view_t::make_empty(), arena);
```

A `changes_cursor_t` is a feed of what was committed to one collection after a stamp, oldest first, which pins that stamp as a snapshot does.

```cpp
auto [feed, feed_status] = db.on().changes_cursor(blobs, committed_at);
auto [changes, changes_status] = feed.next_pairs(256, arena);
for (std::size_t index = 0; changes_status.succeeded() && index != changes.keys.size(); ++index)
    std::printf("%s at %llu\n", ustore_change_kind_name(changes.kinds[index]),
                (unsigned long long)changes.stamps[index]);
```

## Transactions

`db.on().transaction()` begins a `transaction_t`, whose `on()` hands out collections that read and write through it, and the transaction commits only when `commit` is called, answering the commit's position in commit order.
A transaction serves one call at a time, and its `stage`, `rollback` and `reset` run the two-phase commit and start over, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#transactions) describe.

```cpp
expected<transaction_t> transaction = db.on().transaction();
blobs_collection_t inside = *transaction->on().find("blobs");
status = inside[key_from_integer(42)].assign("written inside a transaction");
expected<read_stamp_t> staged = transaction->on().stage();
expected<commit_stamp_t> committed = transaction->on().commit();
```

A conflicting commit answers `status_code_t::write_conflict_k`, `read_conflict_k` or `phantom_conflict_k`, and keeps the transaction's contents for a retry.

## Snapshots

`db.on().snapshot()` pins the whole database at one point, and its `on()` hands out the read-only twin of every collection.
`read_stamp()` is the stamp it reads at, and `save(path)` writes it out as a new database of the same engine, taking an optional progress callable.
A progress callable answers `ustore_continue_k` or `ustore_cancel_k` and is `noexcept`, so one that may throw does not compile, and the call borrows it until it is final, so a temporary does not compile either.

```cpp
expected<snapshot_t> snapshot = db.on().snapshot();
auto pinned = (*snapshot->on().find("blobs"))[key_from_integer(42)].value();
read_stamp_t const at = snapshot->read_stamp();

ustore_size_t const budget = 1ull << 30;
auto within_budget = [&](ustore_size_t finished, ustore_size_t total) noexcept {
    std::fprintf(stderr, "saved %llu of %llu bytes\n", (unsigned long long)finished, (unsigned long long)total);
    return finished <= budget ? ustore_continue_k : ustore_cancel_k;
};
status = snapshot->on().save("/var/backups/ustore", within_budget);
```

## Documents

A `docs_collection_t` hands out `docs_ref` proxies, which read and write JSON through `ustore_docs_read` and `ustore_docs_write`, whole or by a field such as `/person`.
Besides `assign`, a reference offers `insert`, `update`, `upsert`, `patch` and `merge`, and `as` switches the format every later call exchanges.
Field access through a proxy is shorthand like any other read without an arena, while `gist` and `gather` export into the arena they are given.

```cpp
auto [people, people_status] = db.on().create<docs_collection_t>("people");
status = people[key_from_integer(1)].assign(R"({"person": "Alice", "age": 27, "height": 1})");
status = people[key_from_integer(2)].assign(R"({"person": "Bob", "age": "27", "weight": 2})");

expected<owned_value_t> document = people[key_from_integer(1)].value();
expected<owned_value_t> person = people[collection_key_field_t {key_from_integer(1), "/person"}].value(ustore_docs_field_type_string_k);

status = people[key_from_integer(1)].patch(R"([{"op": "replace", "path": "/age", "value": 28}])");
status = people[key_from_integer(2)].merge(R"({"weight": null})");
expected<values_view_t> documents = people[{key_from_integer(1), key_from_integer(2)}].value(arena);
expected<sz::strings_view_t> paths = people[{key_from_integer(1), key_from_integer(2)}].gist(arena);
```

### Pages

A `pages_collection_t` holds ordered element trees, such as XML, where the page key doubles as the key of its root.
`import` replaces whole pages, `insert` answers the keys it minted, and `move`, `splice`, `set_attributes`, `remove_attributes` and `remove` edit by element key, one task each, where an empty column stands for the root, the front or a minted key.

```cpp
auto [pages, pages_status] = db.on().create<pages_collection_t>("pages");
std::array<ustore_key_t, 1> page {key_from_integer(7)};
std::array<std::string_view, 1> markup {"<doc><p>one</p></doc>"}, fragment {"<p>two</p>"};
status = pages.import(page, markup);
expected<keys_view_t> landed = pages.insert(page, fragment, arena); // at the front of the root
std::array<ustore_key_t, 1> element {(*landed)[0]};
std::array<std::string_view, 1> name {"id"}, value {"x"};
status = pages.set_attributes(page, element, name, value);
expected<values_view_t> rendered = pages.read(page, arena, {}, ustore_page_encoding_xml_k);
```

### Gathered Tables

`gather` reads fields of many documents into a `docs_table`, converting each field into its column type on a best-effort basis, and every cell carries `valid`, `converted` and `collides` flags beside its `value`.
A header whose column types are known at compile time is built column by column, and one known only at runtime views an array of fields and an array of types.

```cpp
auto header = make_table_header() //
                  .with<std::int32_t>("age")
                  .with<std::string_view>("person");

std::array<ustore_key_t, 3> keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
auto table = people[keys].gather(header, arena);
auto ages = table->column<0>();
for (std::size_t row = 0; row != ages.size(); ++row)
    if (ages[row].valid)
        std::printf("%d\n", ages[row].value);
```

## Graphs

A `graph_collection_t` batches edges and vertices, and can be used from inside a transaction like any other handle.
`degree` and `present` of one vertex are shorthand, copying a scalar out of a one-off arena, while edges, neighbors and the degrees of a batch land in the arena the call is given.
The edges of a batch come in the order of its vertices, and their `offsets` split them by vertex, so `of_vertex(i)` views the edges of vertex `i` and a missing vertex has none.
`edges` streams every edge of the collection, and `vertex_stream` every vertex, both reading ahead in batches.

```cpp
auto [graph, graph_status] = db.on().create<graph_collection_t>("graph");
status = graph.write_edge(edge_t {key_from_integer(1), key_from_integer(2), key_from_integer(9)});

expected<ustore_vertex_degree_t> degree = graph.degree(key_from_integer(2));
auto successors = graph.successors(key_from_integer(2), arena);
auto between = graph.edges_between(key_from_integer(1), key_from_integer(2), arena);
auto touching = graph.read_edges(key_from_integer(3), arena, ustore_vertex_role_target_k);

std::array<ustore_key_t, 3> vertices {key_from_integer(1), key_from_integer(7), key_from_integer(2)};
expected<edges_view_t> leaving = graph.read_edges(strided(vertices).immutable(), arena);
edges_view_t of_missing = leaving->of_vertex(1); // empty, as vertex 7 is missing
```

## Receivers & Deferred Calls

The receiver chooses where a call runs, and the verb stays, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#receivers--deferred-calls) describe in full.
`db.on(executor)`, `transaction->on(executor)` and `snapshot->on(executor)` hand out the same collections, proxies and operators, whose calls block on the executor, and `deferred(executor)` hands out the deferred twin of every collection, whose verbs answer a `deferred<verb>` once submitted.
RAII carries the lifetime rules: a deferred verb takes owned inputs, a `std::array`, an `st::vector` or a `values_tape_t`, so a borrowed span does not compile, while its arena is an `arena_t&` borrowed until the call is final.
`status()` polls without blocking, `std::move(call).wait()` answers what the call produced, and a `deferred` dropped unwaited waits in its destructor.

```cpp
database_on<ustore_completion_deferred_k> const later = db.deferred(executor);
std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
std::array<sz::string_view_t, 2> const texts {R"({"person": "Alice"})", R"({"person": "Bob"})"};
sz::expected<values_tape_t> values = sz::strings_view_t(texts).gather();
if (!values) return;
auto const deferred_blobs = later[blobs];
status = deferred_blobs.write(keys, values.value).wait();
deferred<values_view_t> reading = deferred_blobs.read(keys, arena);
expected<values_view_t> read = reading.wait();
expected<commit_stamp_t> stamp = transaction->deferred(executor).commit().wait();
```

The deferred database also creates, drops and lists collections, and describes, clears and compacts itself, while the deferred snapshot saves itself through `save`.

## Executors

An executor owns the threads that run calls, serves one call at a time, and is shared by any number of databases, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#executors) describe.
`executor_handle_t::for_ram` spawns host threads, and the handle frees the executor on destruction, waiting for the calls naming it.
Opening blocks, as every constructor does: `init_ram`, `open_ram`, `open_rocksdb`, `view_rocksdb` and `open_leveldb` take the executor to block on as their last argument, while `open_nvme` blocks on the executor its options name.
Either way the database runs its own calls on the calling thread until a receiver names an executor.

```cpp
auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 4});
expected<database_t> opened = database_t::open_rocksdb({.directory = bytes_of("/var/lib/ustore")}, executor);
blobs_collection_t on_executor = *opened->on(executor).find_or_create("blobs");
status = on_executor[key_from_integer(42)].assign("purpose of life");
auto [arena, arena_status] = arena_t::make(executor); // placed where the executor computes
```

`executor_handle_t::gpu_devices` lists the GPUs the process drives, where a device's position is the ordinal `for_vram` takes.

```cpp
std::array<ustore_gpu_device_t, 8> devices {};
expected<std::size_t> count = executor_handle_t::gpu_devices(devices); // may exceed what fits
for (std::size_t index = 0; count && index != *count && index != devices.size(); ++index)
    std::printf("%s %s\n", ustore_gpu_vendor_name(devices[index].vendor), devices[index].name);
```

## Batches and Arenas

Shorthand and verbs trade convenience for reuse, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#batches-and-arenas) describe: `blobs[key].value()` reads into a one-off arena its answer owns and frees on destruction, while a verb given an `arena_t&` reuses it.
Every call rewinds its arena, so its views last until the next call given that arena; to keep them longer, pass that call another arena.
Keys come as `ustore_key_t`, spelled from integers by `key_from_integer`, and a batch is a `std::array`, a `std::span`, an initializer list or a `strided` range.

```cpp
auto [arena, arena_status] = arena_t::make(executor);
for (std::span<ustore_key_t const> batch : batches) {
    auto [values, read_status] = db.on(executor).find("blobs")->read(batch, arena);
    if (read_status.failed()) break;
    consume(values); // valid until the next call given `arena`
}
```

`keys_view_from_arrow` and `strings_view_from_arrow` view an Arrow column as call inputs in place, while `keys_view_to_arrow`, `strings_view_to_arrow` and `docs_column_view_to_arrow` export what a call wrote into an arena as an `arrow_export_t`, which reads that arena until its next call and releases its own nodes on destruction.

```cpp
expected<values_view_t> values = blobs.read(keys, arena);
expected<arrow_export_t> column = strings_view_to_arrow(*values, arena);
ArrowSchema& schema = column->schema();          // `binary` or `large_binary`
ArrowArray const& array = column->array().array; // the arena's own buffers
expected<values_view_t> back = strings_view_from_arrow(schema, array);
status = blobs.write(keys, *back);
```

An `owned_value_t` derives from the `value_view_t` it holds, so it compares and converts like one, and `view()` slices that plain view off.

## Errors

Fallible calls return a `status_t`, a `status_code_t` beside a static message, or an `expected` that holds either a value or the `status_t` explaining its absence, and both are `[[nodiscard]]`.
Dereferencing an `expected<value_type>` yields the `value_type`, `auto [value, status]` decomposes one whose value is default constructible, and an `expected` compares with a `status_code_t` directly.
Nothing throws, and a callable that may throw, such as a progress callback, does not compile.

```cpp
auto [db, status] = database_t::init_ram();
if (status.failed())
    std::fprintf(stderr, "%s\n", status.message);
expected<blobs_collection_t> missing = db.on().find("nowhere");
if (missing == status_code_t::not_found_k)
    std::fprintf(stderr, "%s\n", missing.status().message);
```

## Platform Notes

### Engines

`database_t::init_ram`, `open_ram`, `open_rocksdb`, `view_rocksdb`, `open_leveldb` and `open_nvme` wrap [the C opens](https://github.com/unum-cloud/UStore/blob/main/c/README.md#engines) of the same names, taking their argument structs as options, so the RAM and NVMe opens take `cpu_class_ceiling` as the C field.
`describe`, `clear` and `compact` wrap the C calls of the same names, and a compaction given a `database_compact_plan_t` stays open after a cancel until `compact_plan_finish`.

```cpp
expected<ustore_database_description_t> description = db.on().describe();
status = db.on().clear(ustore_drop_pairs_k);
database_compact_plan_t plan;
status = db.on().compact(within_budget, &plan); // a cancel leaves the plan live
std::printf("stopped in %s\n", ustore_phase_name(plan.phase().phase));
status = db.on().compact({}, &plan);            // continues from the unit it stopped at
status = db.on().compact_plan_finish(std::move(plan));
```

### Design

- __Deferred inputs are owned, arenas borrowed.__
  A `deferred<verb>` keeps one heap block that never moves, holding the outcome, the owned inputs and the outputs, while the C argument structure dies on the stack it was filled on.
- __Answers view the arena.__
  A finished read answers the views a blocking one does, and a finished gather owns its rows, columns and field names beside a table whose cells stay in the arena.
- __No exceptions.__
  Every failure is a `status_t` or an `expected`, and every callable the wrappers take is `noexcept`.
- __No life of its own.__
  The wrappers start no threads, keep no counts and hold no hidden arena: freeing a database, transaction or snapshot waits in C for the deferred calls naming it.
