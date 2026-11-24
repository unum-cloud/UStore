# UStore for C++

`cpp/` holds the C++ wrappers of UStore's public C SDK.
The nine headers of `cpp/ustore/`, one per header of `c/ustore/`, wrap the same calls in C++20 with RAII handles, batched references, smart views and typed streams, and add no ABI of their own.
They include nothing but the C headers and the standard library, everything lives in `unum::ustore`, `#include <ustore/ustore.hpp>` brings in all of it, and nothing throws:

- `database_t`, `transaction_t` and `snapshot_t`: the head, a group of changes, and a point in time, each owning its C handle.
- `blobs_collection_t`, `docs_collection_t`, `graph_collection_t`, `pages_collection_t` and `strings_collection_t`: cheap handles of one collection each.
- `arena_t`: where every output whose length scales with the call lands.
- `executor_t` and `deferred`: the threads a call runs on, and a call submitted there, which a coroutine may `co_await`.

## Installation

```sh
cmake -B build_release -D CMAKE_BUILD_TYPE=Release -D USTORE_BUILD_TEST=1
cmake --build build_release --target ustore_test_cpp && ctest --test-dir build_release -R ustore_test_cpp
```

The wrappers are headers over the C library, so they link exactly as [the C SDK](https://github.com/unum-cloud/UStore/blob/main/c/README.md#installation) does: `ustore::cpp` adds `cpp/` as an include root beside the `c/` of `ustore::abi` and links the archive `ustore::static`, and every header is spelled `<ustore/...>` in the tree and once installed.

```cmake
FetchContent_Declare(ustore GIT_REPOSITORY https://github.com/unum-cloud/UStore.git GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ustore)
target_link_libraries(your_target PRIVATE ustore::cpp)
```

With `USTORE_BUILD_STUB` on, `ustore_test_cpp_stub` runs the same suite against the brute-force stand-in, which links the SDK headers alone, so a header that reaches past `c/` fails to compile there.

## Quickstart

The program opens a database in memory, writes a blob, a document and an edge in one transaction, and reads them back.
Collections are created at the head, and a handle found through a transaction reads and writes through it.

```cpp
#include <ustore/ustore.hpp>

namespace us = unum::ustore;

int main() {
    auto [database, database_status] = us::database_t::init_ram();
    if (database_status.failed()) return 1;
    ustore_key_t const avatar = us::key_from_integer(1), titanic = us::key_from_integer(2);
    us::database_t const& head = database; // the calling thread
    auto [posters, posters_status] = head.find_or_create("posters");
    auto [movies, movies_status] = head.find_or_create<us::docs_collection_t>("movies");
    auto [similar, similar_status] = head.find_or_create<us::graph_collection_t>("similar");
    if (posters_status.failed() || movies_status.failed() || similar_status.failed()) return 1;

    // One transaction writes a blob, a document and an edge, and commits all three or none.
    auto [transaction, transaction_status] = head.transaction();
    if (transaction_status.failed()) return 1;
    us::transaction_t const& changes = transaction;
    auto [poster_changes, poster_status] = changes.find("posters");
    auto [movie_changes, movie_status] = changes.find<us::docs_collection_t>("movies");
    auto [similar_changes, edge_status] = changes.find<us::graph_collection_t>("similar");
    if (poster_status.failed() || movie_status.failed() || edge_status.failed()) return 1;
    if (poster_changes[avatar].assign("\x89PNG").failed()) return 1;
    if (movie_changes[avatar].assign(R"({"title":"Avatar","year":2009})").failed()) return 1;
    if (similar_changes.write_edge({avatar, titanic}).failed()) return 1;
    if (changes.commit().status.failed()) return 1;

    // Reads at the head see the commit: the poster, the year and the one edge leaving Avatar.
    auto [poster, poster_read_status] = posters[avatar].value();
    auto [year, year_status] = movies[us::collection_key_field_t {avatar, "year"}].value();
    auto [arena, arena_status] = us::arena_t::make();
    if (poster_read_status.failed() || year_status.failed() || arena_status.failed()) return 1;
    auto [edges, edges_status] = similar.read_edges(avatar, arena, ustore_vertex_role_source_k);
    if (edges_status.failed()) return 1;
    bool const committed = poster == us::value_view_t {"\x89PNG"} && year == us::value_view_t {"2009"} &&
                           edges.size() == 1 && edges[0].target == titanic;
    return committed ? 0 : 1;
}
```

`cpp/test.cpp` runs this program as its first case.
Shorthand such as `value()` makes a one-off arena per call and owns it in its answer, while the batch verbs below take an `arena_t&` and answer views into it.

## Collections

No collection exists until one is created, and each collection type names the modality it holds as `modality_k`.
`create` records it and refuses a name starting with `_`, which system collections keep, `find` refuses a missing name with `status_code_t::not_found_k` and a collection holding another modality with `status_code_t::invalid_argument_k`, and `find_or_create` creates only when no collection holds the name.
Brackets only find, so `db["articles"]` never creates a collection.
`find(name, arena)` reads the name into an arena on any receiver, a deferred one included, while `find(name)` is the blocking shorthand over a one-off arena.
A collection handle is cheap to construct and holds no state, so threads share one, each passing its own arena, and `handle()` answers the identifier the C calls take.
A snapshot hands out the read-only twin of every collection, such as `docs_read_collection_t` for `find<docs_collection_t>`, so nothing writes through it.

```cpp
auto [blobs, create_status] = db.create<us::blobs_collection_t>("blobs");
us::expected<us::docs_collection_t> people = db.find_or_create<us::docs_collection_t>("people");
us::expected<us::blobs_collection_t> same = db.find("blobs");
us::expected<us::graph_collection_t> refused = db.find<us::graph_collection_t>("blobs"); // invalid_argument_k
us::expected<us::blobs_collection_t> named = db["articles"];                                 // not_found_k when absent
auto [present, probe_status] = db.contains("articles");
```

## Reads and Writes

A blobs collection's `[]` operator returns a `blobs_ref` over one key or a batch of keys that reads and writes through the C interface.
Read without an arena, `value`, `present` and `length` are shorthand: a single key answers an `owned_value_t`, a `bool` or a length, and a batch the owned views of a one-off arena.
Given an `arena_t&`, as `blobs.read(keys, arena)` or `blobs[keys].value(arena)`, the same reads answer plain views into that arena.
`remove` removes the keys, while `clear_values` keeps them and empties their values.

```cpp
us::status_t status = blobs[us::key_from_integer(42)].assign("purpose of life");
us::expected<us::owned_value_t> value = blobs[us::key_from_integer(42)].value();

status = blobs[{us::key_from_integer(43), us::key_from_integer(44)}].assign("same value");
us::expected<bool> present = blobs[us::key_from_integer(43)].present();
status = blobs[us::key_from_integer(43)].remove();

auto [arena, arena_status] = us::arena_t::make();
std::array<ustore_key_t, 3> keys {us::key_from_integer(65), us::key_from_integer(66), us::key_from_integer(67)};
auto [values, read_status] = blobs.read(keys, arena);
for (us::value_view_t found : values) std::printf("%zu bytes\n", found.size());
```

A `strings_collection_t` keys arbitrary bytes by variable-length strings, kept in the order of their text, where `value` and `present` of one string are shorthand and `read`, `scan` and `prefixed_page` export into the arena they are given as a `strings_view_t`.

```cpp
auto [words, words_status] = db.create<us::strings_collection_t>("words");
status = words.assign("apple", us::key_from_integer(1));
us::expected<ustore_key_t> apple = words.value("apple");
us::expected<us::strings_view_t> page = words.prefixed_page("ap", 16, arena);
us::expected<std::size_t> count = words.size("ap");
```

`ingest` opens an `ingest_t` that lands every row appended to it as one commit of its own, joining no transaction, and `finish` answers that commit's stamp.
It opens on the receiver's executor, whose memory domain holds it, while `append` and `finish` each take an executor of their own, the calling thread without one.

```cpp
auto [ingest, ingest_status] = db.ingest(blobs.handle(), ustore_ingest_unsorted_k);
std::array<ustore_key_t, 2> rows {us::key_from_integer(2), us::key_from_integer(1)};
std::array<std::string_view, 2> bytes {"two", "one"};
status = ingest.append(rows, bytes); // copied before the call returns
us::expected<us::commit_stamp_t> landed = ingest.finish();
```

## Scans

`keys` and `pairs` range over a collection in key order from a `start_key` up to an `end_key`, `prefixed` over the strings of a prefix, and `edges` over a graph.
Each range is shorthand for a range-based loop, which has no place for an arena, so it owns the stream it walks, one arena made where the receiver's executor computes, the page in hand and a status.
Its pages are pulled on the receiver's executor, the calling thread for a collection found without one, and a failed pull ends the loop, which `status()` reads afterwards.
`size` counts through such a range, and `size_range` answers the bounds the engine estimates through its measure, without walking the keys.

```cpp
us::keys_range_t keys = articles.on(executor).keys(us::key_from_integer(100), us::key_from_integer(200));
for (ustore_key_t key : keys) consume(key); // pages pulled on the executor into the arena the range owns
if (keys.status().failed()) return keys.status();
for (auto [key, value] : articles.pairs()) consume(key, value); // the calling thread
```

`blobs_cursor_t` and `strings_cursor_t` take the caller's arena and executor on every pull instead, the calling thread without an executor, so a loop reuses one arena it keeps, and a page shorter than `wanted` exhausts the walk for good.

```cpp
auto [cursor, cursor_status] = us::blobs_cursor_t::make(db.receiver(), blobs.handle());
std::size_t const wanted = 256;
for (std::size_t pulled = wanted; !cursor_status.failed() && pulled == wanted;) {
    auto [page, page_status] = cursor.next_pairs(wanted, arena, executor); // or on the calling thread without one
    if (page_status.failed()) break;
    consume(page.values); // valid until the next call given `arena`
    pulled = page.keys.size();
}
```

`docs_cursor_t::make` also takes a table header naming the columns every row gathers, and its `next` pulls batches of rows, one per executor task that found any, none once the walk is exhausted.

```cpp
auto header = us::make_table_header().with<std::int64_t>("age");
auto [rows, rows_status] = us::docs_cursor_t::make(db.receiver(), people.handle(), us::smallest_key_k, {}, header.view());
auto [batches, batches_status] = rows.next(256, arena, executor); // each batch holds keys and one column per field
```

`sample` draws entries at random, on blobs, strings, documents and pages, and `measure` bounds how many entries and bytes a range holds, on strings, documents and pages, while graphs spell them `sample_vertices` and `measure_vertices`.
A `changes_cursor_t` is a feed of what was committed to one collection after a stamp, oldest first, which pins that stamp as a snapshot does.

```cpp
us::expected<std::span<ustore_key_t const>> drawn = people.sample(16, arena);
us::expected<us::size_estimates_t> bounds = people.measure(us::smallest_key_k, us::largest_key_k, arena);
auto [feed, feed_status] = db.changes_cursor(blobs.handle(), committed_at);
auto [changes, changes_status] = feed.next_pairs(256, arena, executor); // or on the calling thread without one
```

### Progress and Cancellation

`compact` and `save` take an optional progress handler, a `noexcept` callable answering `ustore_continue_k` or `ustore_cancel_k` for the units finished of a total, which the call views as a `progress_t`.
A handler that may throw does not compile, and the caller keeps it until the call is final, so a temporary does not compile either.
Answering `ustore_cancel_k` fails the call with `status_code_t::cancelled_k`, a cancelled save removing what it wrote.

```cpp
ustore_size_t const budget = 1ull << 30;
auto within_budget = [&](ustore_size_t finished, ustore_size_t total) noexcept {
    std::fprintf(stderr, "saved %llu of %llu bytes\n", (unsigned long long)finished, (unsigned long long)total);
    return finished <= budget ? ustore_continue_k : ustore_cancel_k;
};
status = snapshot.save("/var/backups/ustore", within_budget);
```

## Transactions

`db.transaction()` begins a `transaction_t`, which hands out collections that read and write through it, and the transaction commits only when `commit` is called, answering the commit's position in commit order.
A transaction serves one call at a time, and its `stage`, `rollback` and `reset` run the two-phase commit and start over, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#transactions) describe.
A conflicting commit answers `status_code_t::write_conflict_k`, `read_conflict_k` or `phantom_conflict_k`, and keeps the transaction's contents for a retry.
A transaction or a snapshot keeps no executor: it runs its verbs on the calling thread and `on(executor)` on that executor, whichever one began it, and a feed of changes takes one per pull.

```cpp
auto [transactional, probe_status] = db.supports_transactions();
if (probe_status.failed() || !transactional) return;
auto [transaction, transaction_status] = db.transaction();
auto [inside, inside_status] = transaction.find("blobs");
status = inside[us::key_from_integer(42)].assign("written inside a transaction");
us::expected<us::read_stamp_t> staged = transaction.stage();
us::expected<us::commit_stamp_t> committed = transaction.commit();
```

## Snapshots

`db.snapshot()` pins the whole database at one point, and hands out the read-only twin of every collection.
`read_stamp()` is the stamp it reads at, and `save(path)` writes it out as a new database of the same engine, taking an optional progress handler.

```cpp
auto [snapshot, snapshot_status] = db.snapshot();
auto [pinned, pinned_status] = snapshot.find("blobs").value[us::key_from_integer(42)].value();
us::read_stamp_t const at = snapshot.read_stamp();
```

## Documents

A `docs_collection_t` hands out `docs_ref` proxies, which read and write JSON, whole or by a field such as `/person`.
Besides `assign`, a reference offers `insert`, `update`, `upsert`, `patch` and `merge`, and `as` switches the format every later call exchanges.
`gather` reads fields of many documents into a `docs_table`, converting each into its column type on a best-effort basis, where every cell carries `valid`, `converted` and `collides` flags beside its `value`, and `docs_field_type_of<T>` names the field type a column of `T` gathers into.

```cpp
auto [people, people_status] = db.create<us::docs_collection_t>("people");
status = people[us::key_from_integer(1)].assign(R"({"person": "Alice", "age": 27})");
status = people[us::key_from_integer(1)].patch(R"([{"op": "replace", "path": "/age", "value": 28}])");
us::expected<us::owned_value_t> person =
    people[us::collection_key_field_t {us::key_from_integer(1), "/person"}].value(ustore_docs_field_type_string_k);

auto header = us::make_table_header().with<std::int32_t>("age").with<std::string_view>("person");
std::array<ustore_key_t, 1> keys {us::key_from_integer(1)};
auto [table, gather_status] = people.gather(keys, header, arena);
us::scalar_column_view<std::int32_t> const ages = table.column<0>();
```

A `pages_collection_t` holds ordered element trees, such as XML, where the page key doubles as the key of its root.
`import` replaces whole pages, `insert` answers the keys it minted, and `move`, `splice`, `set_attributes`, `remove_attributes` and `remove` edit by element key, one task each.

```cpp
auto [pages, pages_status] = db.create<us::pages_collection_t>("pages");
std::array<ustore_key_t, 1> page {us::key_from_integer(7)};
std::array<std::string_view, 1> markup {"<doc><p>one</p></doc>"}, fragment {"<p>two</p>"};
status = pages.import(page, markup);
us::expected<us::keys_view_t> landed = pages.insert(page, fragment, arena); // at the front of the root
```

## Graphs

A `graph_collection_t` batches edges and vertices, and can be used from inside a transaction like any other handle.
`neighbors` answers a `neighbors_view_t`, the other end of every edge of one vertex as stored, so an edge back to the vertex names the vertex and a neighbor joined twice appears twice.
`present` answers a `presences_view_t` over the degrees of a batch, where a vertex without edges is still present, and `degree` of one vertex is shorthand over a one-off arena.
The edges of a batch come in the order of its vertices, and their `offsets` split them by vertex, so `of_vertex(i)` views the edges of vertex `i`.

```cpp
auto [graph, graph_status] = db.create<us::graph_collection_t>("graph");
status = graph.write_edge(us::edge_t {us::key_from_integer(1), us::key_from_integer(2), us::key_from_integer(9)});

auto [similar, similar_status] = graph.neighbors(us::key_from_integer(1), arena);
for (ustore_key_t const neighbor : similar) visit(neighbor);
std::array<ustore_key_t, 3> vertices {us::key_from_integer(1), us::key_from_integer(7), us::key_from_integer(2)};
auto [presences, present_status] = graph.present(vertices, arena);
us::expected<ustore_vertex_degree_t> degree = graph.degree(us::key_from_integer(2));
auto [leaving, leaving_status] = graph.read_edges(vertices, arena);
```

## Receivers & Deferred Calls

The receiver chooses where a call runs, and the verb stays, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#receivers--deferred-calls) describe in full:

- `blobs.read(keys, arena)` runs on the calling thread, as `db.create(..)` and every verb of an owning handle do.
- `blobs.on(executor).read(keys, arena)` blocks on the executor, as `db.on(executor)` and the `on` of a transaction or a snapshot do.
- `blobs.deferred(executor).read(keys, arena)` submits the call and answers a `deferred<T>`.

The database, a transaction and a snapshot are each one class template over the receiver, such as `database<receiver>`, so every verb is written once: `database_t` owns the handle, and `on` and `deferred` answer a `database_on<completion>` that views it with the executor.
Only the owning handles open, begin transactions, pin snapshots and open feeds.
A `deferred<T>` never moves, so it lives on the caller's stack or in its coroutine frame, and the caller keeps the deferred handle, the inputs and the arena it names until the call is final.

```cpp
auto [executor, executor_status] = us::executor_t::for_ram({.threads = 4});
auto [arena, arena_status] = us::arena_t::make({.executor = executor.handle()});
auto [local, local_status] = blobs[us::key_from_integer(42)].value();          // the calling thread
auto [pooled, pooled_status] = blobs.on(executor).read(keys, arena);           // blocks on the executor
us::deferred<us::values_view_t> call = blobs.deferred(executor).read(keys, arena); // running now
other_work();
auto [values, status] = call.wait(); // blocks until final; destruction waits too
```

`co_await call.wait(loop)` suspends any coroutine on any loop with `post(void (*)(void*) noexcept, void*)`, which polls the call's status and resumes it once final, `status()` polls without blocking, and a `deferred` dropped unwaited waits in its destructor, so two collections are read at once:

```cpp
app::task read_both(us::blobs_collection_t const& east, us::blobs_collection_t const& west,
                    us::executor_t const& east_executor, us::executor_t const& west_executor,
                    us::keys_view_t keys, app::loop& loop) {
    auto [east_arena, east_arena_status] = us::arena_t::make({.executor = east_executor.handle()});
    auto [west_arena, west_arena_status] = us::arena_t::make({.executor = west_executor.handle()});
    us::deferred<us::values_view_t> east_call = east.deferred(east_executor).read(keys, east_arena);
    us::deferred<us::values_view_t> west_call = west.deferred(west_executor).read(keys, west_arena);
    auto [east_values, east_status] = co_await east_call.wait(loop); // both reads were already running
    auto [west_values, west_status] = co_await west_call.wait(loop);
    if (east_status.failed() || west_status.failed()) co_return;
    merge(east_values, west_values);
}
```

A loop is anything storing the two words `post` hands it and calling `poll(state)` later, such as this fixed ring, which allocates nothing:

```cpp
struct test_loop_t {
    struct posted_t {
        void (*poll)(void*) noexcept;
        void* state;
    };
    std::array<posted_t, 64> ring {};
    std::size_t head = 0, tail = 0;

    void post(void (*poll)(void*) noexcept, void* state) noexcept { ring[tail++ % ring.size()] = {poll, state}; }
    void run() noexcept {
        while (head != tail) {
            posted_t const next = ring[head++ % ring.size()];
            next.poll(next.state);
        }
    }
};
```

The deferred database also finds, creates, drops and lists collections, and describes, clears and compacts itself, while the deferred snapshot saves itself through `save`.

## Executors

An executor owns the threads that run calls, serves one call at a time, and is shared by any number of databases, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#executors) describe.
`executor_t::for_ram` spawns host threads, `for_nvme` the NVMe engine's rings, and `for_vram` one device stream it owns, a caller's stream answering `status_code_t::unsupported_k` since the executor drives only its own, and the owner frees the executor on destruction, waiting for the calls naming it.
Closing a database or dropping its handle writes nothing to disk, so `flush` on an executor makes commits with `ustore_durability_buffered_k` durable first.
Every arena is freed before the executor that made it, so declare the executor first and its arenas after it, and their destructors run in that order.
Opening blocks, as every constructor does: `init_ram`, `open_ram`, `open_rocksdb`, `view_rocksdb`, `open_leveldb`, `open_nvme` and `view_nvme` take the executor to block on as their last argument, and an NVMe database moves bytes as the placement of an NVMe executor names.

```cpp
auto [executor, executor_status] = us::executor_t::for_ram({.threads = 4});
auto [opened, opened_status] = us::database_t::open_rocksdb({.directory = us::bytes_of("/var/lib/ustore")}, executor);
auto [on_executor, on_executor_status] = opened.on(executor).find_or_create("blobs");
auto [arena, arena_status] = us::arena_t::make({.executor = executor.handle()}); // where the executor computes
status = executor.drain(ustore_mood_parked_k);
```

`gpu_devices` lists the GPUs the process drives, where a device's position is the ordinal `executor_t::for_vram` takes, and `version_name` spells the linked library's version.

```cpp
std::array<ustore_gpu_device_t, 8> devices {};
us::expected<std::size_t> count = us::gpu_devices(devices); // may exceed what fits
```

## Batches and Arenas

Shorthand and verbs trade convenience for reuse, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#batches-and-arenas) describe: `blobs[key].value()` reads into a one-off arena its answer owns and frees on destruction, while a verb given an `arena_t&` reuses it.
Every call rewinds its arena, so its views last until the next call given that arena; to keep them longer, pass that call another arena.
Keys come as `ustore_key_t`, spelled from integers by `key_from_integer`, and a batch is a `std::array`, a `std::span`, an initializer list or a `strided` range, while a batch of strings is a contiguous run of string views or a `strings_view_t` a call exported.

```cpp
auto [arena, arena_status] = us::arena_t::make({.executor = executor.handle()});
for (std::span<ustore_key_t const> batch : batches) {
    auto [values, read_status] = blobs.on(executor).read(batch, arena);
    if (read_status.failed()) break;
    consume(values); // valid until the next call given `arena`
}
```

An `owned_value_t` holds the `value_view_t` it answers beside the one-off arena that view reads, so it compares and converts like one, and `view()` hands out that plain view.

## Errors

Fallible calls return a `status_t`, a `status_code_t` beside an optional static note, or an `expected`, a plain pair of a `value` and the `status_t` beside it, and both are `[[nodiscard]]`.
Recovery branches on the code, while the note names the broken invariant, step or resource for logs, falling back to the status name.
`auto [value, status]` decomposes an `expected`, whose value is default-constructed on failure.
Nothing throws, and a callable that may throw, such as a progress handler, does not compile.

```cpp
auto [db, status] = us::database_t::init_ram();
if (status.failed()) fmt::println(stderr, "{}", status.note);
auto [missing, missing_status] = db.find("nowhere");
if (missing_status == us::status_code_t::not_found_k) fmt::println(stderr, "{}", missing_status.note);
```

## Apache Arrow

The converters of `c/ustore/arrow.h` view an Arrow column as a call's input in place, and export what a call wrote into an arena as an `arrow_export_t`, which reads that arena until its next call and releases its own nodes on destruction:

```cpp
#include <ustore/arrow.hpp>

auto [values, read_status] = blobs.read(keys, arena);
us::expected<us::arrow_export_t> strings = us::strings_view_to_arrow(values, arena);
ArrowSchema& schema = strings.value.schema();          // `binary` or `large_binary`
ArrowArray const& array = strings.value.array().array; // the arena's own buffers
us::expected<us::values_view_t> back = us::strings_view_from_arrow(schema, array);

auto header = us::make_table_header().with<std::int32_t>("age").with<std::string_view>("person");
auto [table, gather_status] = people.gather(keys, header, other);
us::expected<us::arrow_export_t> ages = us::docs_column_view_to_arrow(table.column(0), other);
```

`keys_view_from_arrow` and `strings_view_from_arrow` read keys and strings, and `keys_view_to_arrow`, `strings_view_to_arrow` and `docs_column_view_to_arrow` write keys, strings and gathered columns.

## Platform Notes

### Engines

`database_t::init_ram`, `open_ram`, `open_rocksdb`, `view_rocksdb`, `open_leveldb` and `open_nvme` wrap [the C opens](https://github.com/unum-cloud/UStore/blob/main/c/README.md#engines) of the same names, taking their argument structs as options, so the RAM and NVMe opens take `cpu_class_ceiling` as the C field.
`describe`, `clear` and `compact` wrap the C calls of the same names, and a compaction given a `database_compact_plan_t` stays open after a cancel until `compact_plan_finish`.
`supports_transactions` and `supports_snapshots` read the description, so a describe that fails answers its status rather than a capability.

```cpp
us::expected<ustore_database_description_t> description = db.describe();
status = db.clear(ustore_drop_pairs_k);
us::database_compact_plan_t plan;
status = db.compact(within_budget, &plan); // a cancel leaves the plan live
status = db.compact({}, &plan);            // continues from the unit it stopped at
status = db.compact_plan_finish(std::move(plan));
```

### Left Unbound

- Deferred opens, snapshot pins, cursor pulls and ingests: these block whatever receiver you hold.
- The progress handlers of scans and cursor pulls, and the end bounds and descending walks of cursors.

### Design

- __Each verb is written once.__
  A receiver's completion is a template parameter, so `blocks<completion>` gates the shorthand, each class's `deferrable<T>` names a `deferred<T>` where a blocking call answers an `expected<T>` or a `status_t`, and nothing writes through a snapshot.

  The owning handle and its `*_on<completion>` twins, which view it, are one class template over `owning` or `caller_owned` receiver storage.
- __The Rust analogy holds call for call.__
  `*_on<completion>` mirrors Rust's `On<'index, Index, Completion>`, `deferred<T>` its `Deferred`, `~deferred` its waiting `Drop`, and `wait(loop)` with its `awaiter` the `Future` that re-wakes itself, posting a function pointer and a state pointer as a `RawWaker` posts its two words.
- __Executors are named per call.__
  A transaction, snapshot or feed of changes keeps no executor, so every verb on it names its own through `on` or `deferred`, as in Rust and Go.
- __The allocation budget is fixed.__
  A blocking call, a refusal and every callback allocate nothing, keeping the outcome and the outputs on the stack.
  A deferred submit allocates nothing either: a `deferred<T>` never moves, so the caller's stack or coroutine frame holds it.
  Awaiting allocates nothing in the wrappers, the loop storing two words per poll, and variable-length outputs land in the caller's arena.
  Only shorthand without an arena slot, such as `db[key]`, makes and frees a one-off arena per call.
- __No life of its own.__
  The wrappers start no threads and keep no counts: freeing a database, transaction or snapshot waits in C for the deferred calls naming it.
  No handle keeps an arena, and only a range or a stream, shorthand for a range-based loop, owns the one arena it pulls its pages into.

| Path | C++ | Rust |
|---|---|---|
| Blocking call | 0 | 0 |
| Refusal before the call | 0 | 1 error |
| Deferred submit and wait | 0: the `deferred` lives with the caller | 0: the `Deferred` is pinned where it is polled |
| Each callback | 0 | 0 |
