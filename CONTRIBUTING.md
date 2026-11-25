# Contributing to UStore

Thank you for coming here!
This guide is for everyone who changes UStore, people and coding agents alike, and every rule in it holds for every file in the tree.

## Before You Start

Pull the sibling submodules — NumKong, StringZilla, ForkUnion and SmashTable — before building anything, and put each on the branch `.gitmodules` names, so it tracks that branch's upstream instead of sitting on a detached commit:

```sh
git submodule update --init --recursive
git submodule foreach --quiet 'git switch --quiet "$(git config -f "$toplevel/.gitmodules" "submodule.$name.branch")"'
```

Later, bring every sibling to the tip of its branch before bumping the pointers this repository records:

```sh
git submodule foreach --quiet 'git pull --ff-only --quiet'
```

`cmake/ustore_siblings.cmake` builds each sibling from its nested checkout when it is there, and otherwise fetches the commit its submodule pins, unless `FETCHCONTENT_SOURCE_DIR_<NAME>` names a local checkout.
Machine specifics such as a compiler path go into a gitignored `CMakeUserPresets.json`, whose presets inherit the ones in `CMakePresets.json`.

The repository holds its own hooks, and configuring CMake arms them; to arm them without a build:

```sh
git config core.hooksPath .githooks
```

`.githooks/pre-commit` checks the added lines of every staged file against the rules below and runs `clang-format` 22, `ruff`, `dprint` and `cmake-format` when they are installed.
`.githooks/commit-msg` checks the subject and rejects AI-assistant attribution.

## Building

The `release` and `relwithdebinfo` presets build with whatever toolchain the environment provides, MSVC, AppleClang, GCC or Clang.
`debug`, `check`, `dev`, `trace`, `tsan`, `profile` and `cuda` take Clang and lld, whose sanitizer, tracing and line-table flags they use.
`cuda` also hands Clang to `nvcc` as its host compiler, so the CUDA units mangle their templates like the C++ ones that link them.
`dev` is the everyday tree `.clangd` reads: every CPU target at `-O2` with line tables.
`trace` is `dev` plus `-ftime-trace`, writing a `.json` beside every object to show where compile time goes.
`check` turns warnings into errors through `USTORE_WERROR`, and suites, benchmarks and tools are off unless a preset or `-D` asks for them.
Only `release` and the Windows presets add RocksDB and LevelDB; every other preset builds the RAM engine and whatever `-D USTORE_WITH_*` adds.
`msvc2026_x64` and `msvc2026_arm64` build `release` with Visual Studio, while `android_arm64`, `wasm32_emscripten` and `wasm64_emscripten` compile the libraries over the RAM engine and run nothing.
`CMAKE_C_COMPILER_LAUNCHER`, `CMAKE_CXX_COMPILER_LAUNCHER` and `CMAKE_CUDA_COMPILER_LAUNCHER` come from the environment, so exporting `ccache` there caches objects in every tree.

`USTORE_MARCH` sets the floor of UStore's own units, an ISA level like `x86-64-v3`, `native` or a CPU name like `graniterapids`, and hands the same value and build kind to ForkUnion, StringZilla and SmashTable.
`USTORE_MULTIVERSIONED` is the build kind: `ON` by default, a multiversioned build of every CPU class the toolchain compiles, picked at runtime, or with `OFF` a monoversioned build of one `monoversioned` class and no named class, `serial` included.
That class takes the waiter, atomic reference and string kernels StringZilla and ForkUnion enabled: what the floor implies, plus what `-D <LIB>_TARGET_<CAP>=1` vouches for where no compiler macro reports it, like `FORKUNION_TARGET_ARM64_WFET=1` on Vera.
A handle pinned below it, or opened on a CPU lacking it, gets `ustore_status_unsupported_k`, and both switches are seeded from the environment variables of the same name.
`USTORE_CUDA_ARCH` adds NVMe's GPU compaction, whose kernels need only plain codes: `all` compiles a cubin per generation from Ampere through Blackwell and the Ampere PTX for every later GPU, `native` the code of each visible GPU, and one code like `100f` its cubin alone.
The machine profiles `xeon4_hopper`, `xeon6_blackwell`, `grace_hopper` and `vera_rubin` set `USTORE_MARCH` with `USTORE_MULTIVERSIONED=OFF` and the one CUDA code of their GPU, and CI compiles each of them.
`USTORE_METAL_ARCH` adds the Apple GPU classes, whose kernels travel as MSL source that each device compiles at the first spawn of its class, so no Metal toolchain is needed: `all` keeps `apple7`, `apple9` and `apple10`, and one family keeps its class alone.
The `metal` preset builds them on macOS, and off Apple platforms the same switch compiles the Metal host units against the SDK declarations StringZilla ships, linking none.

`CMakeLists.txt` builds two libraries from the same sources, every engine adding its own:

- __`ustore_static`__, aliased `ustore::static`, takes the objects of ForkUnion, StringZilla, yyjson and fmt, and links the engines' own archives, RocksDB, LevelDB and liburing, which install beside it in the same CMake package, so a change to UStore never rewrites them. The suites, benchmarks and tools inside the tree link it as `ustore::static` and reach the siblings through it.
- __`ustore_shared`__, aliased `ustore::shared`, links the same libraries privately, compiles with `USTORE_EXPORTING`, hands its consumers `USTORE_SHARED`, and exports only the functions `USTORE_EXPORT` marks, as hidden visibility, a `ustore_*` version script on Linux, Android and FreeBSD and `-exported_symbol,_ustore_*` on macOS keep every other symbol inside.

`c/test.c` builds against both, as `ustore_test_c` and `ustore_test_c_shared`, and CTest runs both.
`USTORE_INSTALL` installs the two libraries, the headers, a `.pc` file and a CMake package under `lib/cmake/ustore`, which `find_package(ustore)` reads as `ustore::abi`, `ustore::templates`, `ustore::static`, `ustore::shared` and `ustore::cpp`, the C++ SDK linking the C headers and the archive.
The install splits into components: `sdk` holds both libraries with the engines' archives, `c/ustore/`, `cpp/ustore/`, the package and the license, and `templates` the header-only `include/ustore/`.
`cpack --config build_release/CPackConfig.cmake` archives the `sdk` component alone, stripped, as the preview SDK `ustore-<version>-<platform>-sdk.tar.gz`, a ZIP on Windows.
A function the headers declare without `USTORE_EXPORT` links against the archive but is missing from the shared library, so every new declaration holds it.

## Layout and Namespaces

| Path                    | Contents                                             | Namespace                         |
| :---------------------- | :--------------------------------------------------- | :-------------------------------- |
| `c/ustore/*.h`          | the C ABI, flat                                      | none                              |
| `c/test.c`              | compiles the C ABI as plain C11                      | none                              |
| `cpp/ustore/*.hpp`      | the C++ wrappers over the C ABI, flat                | `unum::ustore`                    |
| `include/ustore/<dir>/` | header-only templates the engines instantiate        | `unum::ustore::<dir>`             |
| `src/lib.hpp`           | the router every exported call goes through          | `unum::ustore`                    |
| `src/*.cpp`             | one source per public header, documents and graphs   | `unum::ustore`                    |
| `src/engine.hpp`        | the calls every engine fills, and the CPU classes    | `unum::ustore`                    |
| `src/utilities/`        | batches, outputs and progress over the C ABI's types | `unum::ustore::utilities`         |
| `src/<engine>/`         | `engine.cpp` and the glue of `ram` and `nvme`        | `unum::ustore::<engine>`          |
| `src_oracles/<engine>/` | `engine.cpp` of RocksDB and LevelDB, kept as oracles | `unum::ustore::oracles::<engine>` |
| `tools/`                | the CLI, flag parsing, and dataset import and export | `unum::ustore::tools`             |
| `test/`                 | suites mirroring `src/`, and their harness           | `unum::ustore::test`              |
| `bench/`                | benchmarks mirroring `src/`, and their harness       | `unum::ustore::bench`             |
| `verification/`         | Spin models and GenMC checks, run by `check.sh`      | none                              |

Namespaces mirror directories and nothing else: a directory adds a namespace component, and no namespace groups symbols by theme.
`src_oracles/` adds `oracles`, and an engine sees `unum::ustore::utilities` through one `using namespace` right after its own namespace opens.
`c/`, `cpp/` and `include/` are include roots that add no component, so `cpp/ustore/` opens `unum::ustore` and `include/ustore/blobs/` opens `unum::ustore::blobs`.
The pre-commit hook refuses a namespace no directory backs, a nameless one, and one a file opens twice.
A file name never repeats its directory — `include/ustore/blobs/nvme_tree.hpp`, never `include/ustore/blobs/blobs_nvme_tree.hpp`.

## Code Rules

### Naming

- `_t` marks concrete types and the aliases that materialize a template; `_k` marks enumerators and constants.
- Class templates hold no suffix: `class lsm_tree`, never `class lsm_tree_gt`.
- Template type parameters end in `_type_` and private members in one underscore, so a parameter never shadows a member: `typename locations_type_` beside `locations_type_ locations_`.
- Identifiers are whole words: `index`, `count`, `transaction`, `directory` — never `i`, `cnt`, `txn`, `dir`.
- `init`, `min`, `max`, `id`, `gds`, `ref` and vendor terms are whole words of the vocabulary and stay as they are.
- A transport type holds its operating system, as in `linux_spdk_environment`, while a type mirroring a vendor's own struct or API table keeps the vendor word, as in `cufile_api_t`.
- A type is named `<domain>_<role>`: the domain word says what it holds and the role word what it does, as in `lsm_tree`, `lsm_level`, `lsm_sorted_run`, `sorted_run_index`, `key_slot`, `stored_value`, `write_buffer` and `graph_store`.
  Only C ABI terms stay one word — `database`, `collection`, `transaction`, `snapshot` — and no name is a metaphor or a legacy project name.
- A file is named after its main type without `_t` and never repeats its directory; C++ headers are `.hpp`, never `.h` or `.tcc`.
- A `bool` answers a yes-or-no question and never selects behaviour; name the states in an `enum class`, backed by `bool` when width matters.
- A closed set of configurations gets one enumerator per configuration, never a struct of knobs that can express combinations that do not exist.
- These rules govern C and C++ alone; Python follows PEP 8, with `UPPER_SNAKE_CASE` module constants, `snake_case` functions and variables and `CapWords` classes, and never a `_t` or `_k` suffix.

### Templates and Macros

Every template head and argument list is spelled out where it is used, and constraints are named C++20 concepts.
No macro stands in for a template head, an argument list or a code pattern:

```cpp
// Rejected
#define lsm_tree_def_m template <typename stored_key_type_, value_layout_t value_layout_>
#define lsm_tree_args_m stored_key_type_, value_layout_
lsm_tree_def_m class lsm_tree;

// Written out, constrained by a concept
template <typename stored_key_type_, value_layout_t value_layout_>
    requires a_stored_key<stored_key_type_>
class lsm_tree;
```

A macro survives only as an object-like configuration flag or include guard.
`#if` brackets whole declarations or include blocks, never a fragment of an `if`/`else`.

Every all-caps name starts with the full project name, `USTORE_`.
A trailing `_` marks a name as internal: it may change in any release, and nothing outside this repository may define or test it.
A name without it is a public contract, either a switch you may set or a value you may read.

| Family                    | Form                            | Example                       |
| :------------------------ | :------------------------------ | :---------------------------- |
| CPU or GPU class          | `USTORE_TARGET_<CLASS>`         | `USTORE_TARGET_INTEL_HASWELL` |
| Optional facility         | `USTORE_WITH_[<OS>_]<FACILITY>` | `USTORE_WITH_LINUX_SPDK`      |
| Operating-system identity | `USTORE_OS_<OS>_`               | `USTORE_OS_POSIX_`            |

Every name in these families is always defined, as 0 or 1, and tested with `#if`.
`defined(...)` appears only in a `!defined` default and in a guard that an `#error` follows.
A dependency facility, like `USTORE_WITH_ROCKSDB`, defaults to 0, and the target that links the dependency sets it.
Only the Platform Identity region of `c/ustore/types.h` reads operating-system predefines like `__linux__`.
Compiler predefines like `__clang__`, `__GNUC__` and `_MSC_VER` are read raw where they matter.

### Errors and Memory

Nothing throws.
Fallible calls return a status or `expected`, constructors that can fail become factories returning `expected`, and no code adds `try` or `catch` except one `try` per exported `ustore_*` call in the RocksDB and LevelDB engines, whose libraries throw.
Errors are checked with an explicit `if` and `return`, never a macro.
The status is `status_t {status_code_t code; char const* note;}`, mirroring the C ABI's `ustore_outcome_t`, and `expected<value_type_>` keeps its value in a union alive only on success, so a move-only type needs no empty state.

No owning standard type that can throw appears in `cpp/` or `src/`:

| Instead of                     | Use                                                                        |
| :----------------------------- | :------------------------------------------------------------------------- |
| `std::string`                  | StringZilla `sz::string` through `try_assign`, `try_reserve`, `try_resize` |
| `std::vector`                  | SmashTable `vector` through `make()`                                       |
| `std::unordered_map`           | SmashTable `basic_hash_table`                                              |
| `std::unordered_set`           | SmashTable `basic_hash_table`                                              |
| `std::map`, `std::set`         | SmashTable `basic_avl_tree`, or `basic_wb_tree` for rank and select        |
| An array sized once            | SmashTable `vector` through `resize()`, which returns a status             |
| `std::thread`, `std::function` | The ForkUnion executor below, and a template callback parameter            |

Non-owning `std::span` and `std::string_view` are welcome.
The RocksDB and LevelDB engines hand their libraries the strings and vectors those libraries take, inside the one `try` of each exported call.
Tests, benchmarks and tools may hold a `std::string` where an API hands one over, as `std::filesystem::path::string` and `fmt::format` do, or where a case needs owned text to feed the library.
No mutable global state: no namespace-scope mutable variables, no function-local `static`, and never `static thread_local`.
The one exception is a test harness's `test_run`, which [Test Suites](#test-suites) describes.
Per-thread scratch belongs to the executor that owns the threads.

### Threads and Placement

Threads, topology and domain-placed memory come from ForkUnion's C ABI in `forkunion/forkunion.h`, wrapped once by the C++ `ram_executor_t` of `include/ustore/utilities/ram_device.hpp` and the arenas of `include/ustore/utilities/ram_buffers.hpp`; nothing else calls `fu_*` directly.
The executor holds its threads as `threads_t`, a variant of the `fu_pool_t` its factories spawn through `fu_topology_new`, `fu_pool_new` and `fu_pool_spawn` and its destructor deletes, or of `fu::dummy_pool_t`, the calling thread alone, which `on_calling_thread()` builds with no topology probe, pool or spawn; it forwards C++ callables to either through a C callback.
The arenas place their blocks with `fu_allocate_on_domain_id` and release them with `fu_free_on_domain_id`.

These principles hold in `include/` and `src/`, and the other three libraries state the same ones:

1. No mutable global or static, as the rule above says.
2. Ownership is a type.
   - Who frees a resource, and which call uses it exclusively, are types such as `io_channel_t&` and `claimed_channels_t`, never a `bool` or a raw pointer.
3. Threads belong to an executor.
   - The `ram_executor_t` of an executor handle creates every thread, and `nvme_queues_t` creates its queues but no thread.
   - An NVMe executor has one reading and one writing queue per thread, plus the pair at index `threads` for I/O run on the calling thread.
   - Nothing creates a thread inside a verb, no verb naming an executor creates a queue, and nothing runs that no one called.
4. Warm calls allocate nothing.
   - Sizes come from hardware facts or the caller's budget, fixed at creation.
   - A call naming no executor runs on the calling thread and creates no pool, thread, ring, SPDK queue pair or stream and probes no topology; its scratch and blocking queues live for that call only.
   - On RAM such a call runs through `run_alone`, and on NVMe `make_call_queues` in `src/nvme/database.hpp` builds blocking queues only, over files the database holds, freed when the call returns.
   - Nothing under `include/` builds queues: a call naming no queues answers `invalid_argument`, and a mount reads on the opening call's queues.
   - `ustore_database_free` builds no queues and does no I/O on any transport, so durability comes only from `ustore_database_flush`, and a collection drop that finds open readers answers `contended`.
5. Spin only on completions the call itself submitted.
   - Wait in the kernel for events a remote party triggers.
6. A library never names cores.
   - Threads and staging go on the memory domain the caller names, else the one the media sit on, inside the creating thread's affinity mask.
   - `ustore_executor_for_nvme` takes the first memory domain its `ram` options name, else the domain its `nvme.paths` share on a machine of several.
   - Zero threads take the fewest hardware queues of the disks under those paths, capped by the cores.
   - Every executor covers its own core set.
   - A third-party pool, such as RocksDB's, is created at one documented site, sized from the cores the executors left idle.
7. Residency is a type of the data handle.
   - An executor of another residency answers `device`: a RAM executor on an NVMe database is refused by the router before it claims the executor.
   - A `NULL` executor on an SPDK database answers `invalid_argument` before any work, since no blocking queue reaches a namespace.
   - A VRAM executor reads through the blocking queues it owns and queues device work on the stream it owns.
   - A caller's stream answers `unsupported`.
   - One call runs per executor; a second answers `contended`.
8. One word per concept, the same in all four libraries.
   - A name says who owns a thing and until when it is valid, never a hand-over, so `.githooks/pre-commit` refuses `lend`, `borrow`, `loan` and `lease` words in the identifiers of every language, as snake_case or camelCase words, except Rust's `std::borrow` names, and the docs use none of them.
   - "Lane" names only SIMD and warp lanes.

An NVMe executor owns its `nvme_queues_t` of `include/ustore/utilities/nvme_queues.hpp`, a reading and a writing ring per thread over that thread's own staging, and a call reads, writes and merges on its own thread's rings.
No engine compares executor addresses to pick a path, and `.githooks/pre-commit` refuses an added `executor ==` or `executor !=` under `src/` and `include/`; a database owns its media, cache and reader epochs, never an executor.
A ring registers a database's media by its `media_list_key_t`, the disk's media identity drawn from the system's entropy and its 64-bit media epoch, so a warm call makes no registration, and a database never holds a ring or a queue set.
A ring's registered-file table keeps the descriptors of the last media registered into it, so the kernel keeps those files open after a database closes them, until the ring registers other media or its executor is freed.

### Hardening

`.githooks/pre-commit` holds the rules here that a check on added lines can hold.

- A `NULL` executor runs the call on the calling thread. It creates no pool, thread, ring, SPDK queue pair or stream and probes no topology; its scratch and blocking queues live for that call only. Name an executor for calls that allocate nothing once warm.
- No static, global, shared or thread-local pool, executor, queue set, stream or handle exists, stateless ones included: whoever needs one creates it and frees it.
- A `*_free` waits for the calls naming its handle and releases memory; it reads and writes no drive, and durability comes only from the flush verb.
- An executor owns its streams, created with it; a call without one uses the null legacy stream or is refused, and no second origin sits beside the owner.
- Every arena is freed before its executor; a binding enforces the order by lifetime where its language can and documents it otherwise, never by reference counts.
- A C struct change lands with every binding that lays the struct out by hand, checked field by field in review.
- Tests never restate a struct's layout that the compiler already pins, so no binding asserts a mirror's size or offsets.
- A handler the engine may call from several threads at once is safe to call concurrently in every binding: predicates.
- Every executor is equal; a handle owns its media, cache and epochs, never an executor, so no engine compares executor addresses to pick a path; the C layer may refuse an arena or plan used on another executor than its own.
- Names say who owns a thing and until when a view is valid; no identifier in any language hands a thing over.
- A fix lands on every residency and vendor that has the bug, or its message names the ones left and why.
- A commit message names only what its diff shows.
- A wave never re-wraps or rewords comments it does not otherwise touch.
- A commit touching `.cu`, `.cuh`, `.hip` or `.metal` names the box that built and ran it, or says none did.

### Query Views

A batched kernel takes its queries as one flat transparent view over a static extent, like a `std::array` of views sized by the batch's extent, never as a span of pointers it chases one query at a time.
A query type that owns packed storage exposes a view of it, and the kernel reads that view.
`.githooks/pre-commit` refuses a `std::span` of pointers to const under `include/`, except a span of C strings, which names things rather than batching queries.

### Planned Operations

A long or multi-phase operation starts as a plan, like *Example (USearch): `plan_refine`*, and its blocking verb is only a loop over a one-shot plan, on every residency.
Each operation has one plan type and one finish, like `ustore_<family>_<op>_plan_t` and `ustore_<family>_<op>_plan_finish`, in every family that offers the operation, and no verb describes a plan.
The plan is a lazy handle field on its verb: `NULL` runs the operation once, a `NULL` pointee opens a plan, and a live plan continues.
The finish frees the plan whatever it answers, and finishing a finished plan answers `invalid_argument`.
Cursors are not plans, and keep their `init`, `next` and `free` verbs; `next` refuses a `NULL` cursor or a foreign handle, and `free(NULL)` does nothing.
An engine plan owns its input and output lists, copying the `{count, handles}` pairs it was given, and the glue opens each operation through its own plan-open table entry, with no `std::variant` and no opener callable.
A free waits for the calls naming its single handle, which the library counts on the handle, so a binding keeps no counts of its own.
Every handle a list names, like a plan's sources and targets, must outlive the call and the plan's finish; the headers say so, and nothing counts the handles in a list.

Its parts are named by kind, and the kinds never mix.
A phase is a different kind of work, ended by an `advance()` barrier.
Phases are one `u8` enum shared by every plan, starting at `none_k = 0`, and mirrored by one C enum with `ustore_phase_name` and `ustore_phase_from_name`, with a `static_assert` per value where the glue casts it; progress reports `ustore_phase_progress_t{phase, ordinal, completed, total}` and no strings.
A repetition is the same work again, named for what repeats and counted, like a round or a depth.
A task is an independent unit of one phase that any thread runs in any order, ForkUnion's word; "step" names only sequential work.
`tasks()` lists the current phase's tasks, `run(task, context)` runs any of them on the calling thread, `advance()` is the barrier answering whether another phase follows, and `finish()` publishes and releases.
After the last phase, `advance()` keeps answering false and changes nothing.
Planning claims and reserves everything a task writes, so a task never fails for lack of memory and a plan stopped between tasks leaves the data correct.
Per-thread state lives in the traversal context `run` is handed.
An operation is a plan only when its work crosses a barrier between phases, runs long enough to stop and resume, or writes stored data; a pass whose threads each finish their own share is a direct verb over the caller's contexts.
A plan owns only what crosses its barriers: scratch belongs to the context, results land in the caller's outputs, named at planning, and whatever follows from other members is computed rather than stored.
`run`, `advance` and `finish` never allocate.
A report's verdict-like enum starts at `none = 0`, so a zeroed report never reads as progress.

UStore's one long operation is compaction: `ustore_database_compact` takes a `ustore_database_compact_plan_t`, and `ustore_database_compact_plan_finish` frees it.
Its phases are `flush`, `merge` and `reclaim`, between `none` and `done`, and each engine runs the ones it has: RAM a merge task per collection, then one reclaim task; NVMe a flush task, then merge passes of one task each; RocksDB a flush task per column family, then a merge task per family; LevelDB a merge pass per collection's key range.
Each engine's `compact_plan_t` holds `task_t`, `tasks()`, `run(task, context)`, `advance()`, `stop()`, `status()`, `phase()`, `ordinal()`, `completed()`, `total()` and `finish()`, its table entry `compact_plan_open` opens one, and `compact_plan` holds the `plan_verbs` the router drives it through.
The driver fans each phase's tasks out over the executor and ticks the phase's units at its barrier, where a cancel stops, so a plan the cancel leaves live continues at the phase it reached.

A member of a plan, cursor or walk names what it holds and when it holds it, like `picked_this_round_` or `visited_`.
A pair that trades places shares one stem and differs in one word, like `picked_this_round_` and `picked_next_round_`, and the verb moving between them is named for the barrier it runs at, like `advance_round_`.
No member is `bits`, `current`, `state`, `data`, `flags`, `tmp` or `scratch` alone, and none is named by its type.
Sizes come from the owner, never from a helper on the state.

### Refusals

A call the library cannot run answers one status, picked by what is wrong, in this order: `invalid_argument`, `read_only`, `config_mismatch`, `unsupported`, `contended`, `overflow`.
A `NULL` handle the call operates on answers `uninitialized` before any of them.
A malformed argument or list, one that is empty, `NULL` or names a handle twice, answers `invalid_argument`.
A write to a handle opened read-only answers `read_only`.
Handles of another kind, or of a geometry the call cannot join, answer `config_mismatch`.
A well-formed case nothing implements yet, like UStore's blob write on an executor that computes on a device, answers `unsupported`.
An option the library cannot honour is refused with `unsupported`, never ignored.
A handle another call holds answers `contended` rather than waiting, and a size past its counter answers `overflow`.
A refusal moves nothing: every output and handle reads as it did before the call.
Each header lists a verb's refusals under `@par Statuses` in this order and in these words, and the stub library answers every one of them.

### Statuses

Statuses 0 to 19 hold the same numbers and meanings in every Unum library: success 0, deferred 1, out_of_memory 2, shape_mismatch 3, unsupported_layout 4, unknown_kind 5, read_only 6, io 7, device 8, malformed 9, internal 10, overflow 11, uninitialized 12, config_mismatch 13, contended 14, cancelled 15, unsupported 16, invalid_argument 17, not_found 18, already_exists 19.
A library leaves out the ones it cannot answer but never renumbers the rest, and numbers its own statuses from 20.
Every enum the C interface names, statuses included, has a `ustore_*_name` answering "unrecognized" for a value it does not know and a `ustore_*_from_name(view, &out)` answering `invalid_argument` for a name it does not know, leaving `out` as it was.
CPU and GPU class values are ABI: each class keeps the number USearch gives the same hardware, written explicitly, with unused numbers reserved rather than reused.
`ustore_version_name` answers the full version, pre-release suffix included, in the engine and in the stub alike.

### Notes

A failure is `status_t {code, note}`, two words: recovery branches on the code, and the note, a literal or other static text, names the broken invariant, step or resource for logs and regression analysis.
Codes use the C interface's spellings and numbers, and the SDK in `cpp/` keeps an identical copy of `status_t`, so a status reads the same on both sides of the C layer.
A note is 2 to 8 words in sentence case with no closing period, never restates the code, and never carries a value that differs per call, such as a path, an error number, a size or an identifier.
A failure forwards whole, as `if (x.failed()) return x;`, a site that returns its own code writes its own note, and no site hides a callee's more specific failure behind a code of its own.
A call made once per candidate, neighbor, match or key may answer `expected<value_type_, status_code_t>`, the bare code, and the note is attached once where the loop's failure leaves it.
An atomic status word holds the code alone, and only the thread whose compare-exchange wins writes the note.
The C outcome's message is the note, else the rule of the call that was broken, else the status's own `ustore_status_name`.

### Compile-Time Choices

Code picks between alternatives at compile time through three gates, each for one kind of choice.

`#if` picks a toolchain or a platform, and nothing else.
A feature the compiler or the operating system lacks is a `#if`; a choice between two algorithms is not.

```cpp
#if defined(_WIN32) // allowed: the platform decides
using file_handle_t = void*;
#else
using file_handle_t = int;
#endif
```

A `requires` clause picks an overload when a whole function differs.
Every constrained overload has an unconstrained fallback beside it, so a type the constraint rejects still finds a function.

```cpp
template <typename index_type_>
status_t run_plan(index_type_& index) noexcept requires an_nvme_index<index_type_> { /* NVMe rounds */ }

template <typename index_type_>
status_t run_plan(index_type_& index) noexcept { /* every other residency */ }
```

`if constexpr` branches on a named concept or a `constexpr bool`, one level deep.
An `else if constexpr` chain is still one level.

```cpp
template <typename query_type_>
concept a_hinted_query = requires(query_type_& query) { query.find_hinted(0u); };

if constexpr (a_hinted_query<query_type_>) seeds = query.find_hinted(shard); // allowed: a named concept
else if constexpr (serial_class_k) seeds = {};                              // allowed: one chain, one level
```

The hook refuses each of these, in the directories its `gated_source_directories` names:

```cpp
if constexpr (requires { query.find_hinted(0u); }) { /* ... */ }   // refused: name the probe as a concept

if constexpr (is_ram_k) {
    if constexpr (is_serial_k) { /* ... */ }                        // refused: a second choice inside the first
}

template <typename value_type_, typename = std::enable_if_t<std::is_integral_v<value_type_>>>
value_type_ clamp(value_type_ value) noexcept;                      // refused: write `requires` on the overload

struct sorted_tag_t {};
void insert(entry_t entry, sorted_tag_t) noexcept;                  // refused: a tag that only picks an overload
```

Control flow nests at most three deep: `if`, `for`, `while`, `switch`, `do`, `try` and `catch` each count, and so does `if constexpr`.
An `else` or `catch` body sits at its `if`'s or `try`'s depth, so an `else if` chain stays flat.
A body without braces counts like a braced one, while a lambda or a plain block adds nothing.
Both sides of a `#if` count from the depth the `#if` began at.

```cpp
for (shard_t& shard : shards)                       // 1
    for (node_t node : shard.nodes)                 // 2
        if (node.live)                              // 3
            while (node.next != end) step(node);    // refused: 4 deep
```

To flatten, return early from failures, extract the inner loop into a function named for what it does, or split an overload per case.

The checks are `check_constexpr_conditions_named`, `check_constexpr_branches_flat`, `check_enable_if_and_tags_absent` and `check_constexpr_nesting`.
They judge only the lines a commit adds, so existing code flattens as it is touched.

### Split String Literals

A string literal split across lines by hand ends every fragment but the last with a trailing `//`.
Without the pin, clang-format joins the fragments or restacks them into a staircase.
This holds in C, C++, CUDA and HIP sources alike.

```cpp
char const help[] =                                 //
    "Usage: probe [options]\n"                      // allowed: every fragment but the last is pinned
    "Runs one probe.\n";

char const help[] = "Usage: probe [options]\n"
                    "Runs one probe.\n";            // refused: the first fragment is unpinned
```

The check is `check_split_string_literal_pinned`.
A comment that explains the line pins it just as well, and a bare `//` on a member line is not taken for a member's documentation.

### Vocabulary

The first rows are shared by every U* project, and UStore's own follow.

| Concept                                                          | Word                                                      | Refused                                         |
| :--------------------------------------------------------------- | :-------------------------------------------------------- | :---------------------------------------------- |
| a kind of work in a plan, ended by a barrier                     | phase                                                     | stage, step, pass                               |
| the same work again, counted                                     | repetition, named for what repeats: round, depth, nesting | phase, pass                                     |
| an independent unit of one phase, run by any thread in any order | task                                                      | step, chunk                                     |
| sequential work                                                  | step                                                      | task                                            |
| a per-capability kernel bundle                                   | kit                                                       | trait                                           |
| a compiled alternative per class, dtype or residency             | variant                                                   | arm, cell                                       |
| RAM, VRAM or NVMe                                                | residency                                                 | home, memory side                               |
| an exported operation and its table entry                        | verb; trampoline                                          | …                                               |
| a fixed-shape collection a verb fills                            | outputs                                                   | landing, lander                                 |
| a member of a plan, cursor or walk                               | what it holds and when it holds it                        | bits, current, state, data, flags, tmp, scratch |
| one piece of a compaction the driver runs and reports            | unit                                                      | chunk, batch                                    |
| the handles of the C ABI                                         | database, collection, transaction, snapshot               | db, table, txn, snap                            |

### Documentation and Comments

- Every file opens with `@file`, `@author`, `@date` and `@brief`, in that order and contiguous; `@file` names the path from the repository root and `@date` reads like `January 9, 2023`.
- Docblock text sits two spaces past its asterisk, as in ` *  @brief`, and a continuation indents two more.
- Declarations are documented with `/** */`, never with a bare `//` line above them, and every `/**` has an empty line above it.
- A block that is one unstructured paragraph holds no `@brief`: `/** What it is. */`.
- Every `@param` states its direction: `@param[in]`, `@param[out]` or `@param[in,out]`.
- No Markdown inside docblocks — no headings, bullets or `__bold__`, bar the `- @c status` list of `@par Statuses:`; use `@note`, `@throws`, `@c`, `@p` and plain sentences.
- A closing `@par Statuses:` block lists only the statuses a verb's contract adds, while resource failures, cancellation, deferral and defects are covered once by the categories in the `ustore_status_t` docblock.
- A code span inside a docblock is `@c token`, a shape or a range is `@b [minimum,maximum]` with the whitespace inside the brackets squeezed out, and a span too long for either becomes plain prose; backticks stay in `//` line comments and in the trailing comments of `#include` lines.
- Backticks mark code, identifiers and literal values, never notation such as a mapping written with an arrow.
- Prose arrows in comments and Markdown are → with a space on each side, never the ASCII `->`.
- Document all fields of a type or none of them, each with its own line.
- Comments stay to one line, two at most, state what the code cannot, and never narrate history.
- Sections are `#pragma region Name` and `#pragma endregion Name`, with letters, digits and spaces in the label, never banners of asterisks, dashes or equals signs.

### Includes and Printing

Includes form groups separated by blank lines, alphabetical inside each: C library headers such as `<cstdint>`, platform headers such as `<unistd.h>`, C++ standard headers, external libraries such as `<rocksdb/db.h>` and `<yyjson.h>`, sibling projects such as `<smashtable/basic_vector.hpp>`, then UStore's own headers in quotes, such as `"ustore/db.h"`, `"ustore/utilities/ram_buffers.hpp"` and `"utilities/batches.hpp"`, where headers private to `src/` are written relative to `src/`.
An include's trailing comment lists in backticks the symbols the file takes from that header, and no other.
Sibling projects are reached through the aliases `fu`, `st` and `sz`, declared once in `cpp/ustore/types.hpp`, never through spelled-out `ashvardanian::` namespaces or local aliases.
C++ prints through `fmt::println` — no iostreams, no `printf`.
Discard a result through `[[maybe_unused]] T const result = call();`, never `(void)call();`, and declare return types before the name, never after an arrow.

## Lean State

A type holds only what it cannot recompute, and names each part for what it holds.
These rules are shared by every sibling library, and the pre-commit hook enforces each rule that names a check.

- Members are named for what they hold, never for their type.
  Write `request->call` or `call->kept`, never `call->call` or `file.file`.
  Checked by `check_self_repeat_access`.
- Derived state is a method, not a field.
  A count, mask or capacity that follows from other members is computed when asked, so it can never disagree with them.
- One fact, one constant.
  Every `_k` constant states its derivation in its docblock, or says "measured" and where, and no other constant restates it.
- An owner that never moves holds its parts by value.
  A pointer is for a part that outlives its owner, moves apart from it, or is shared beyond it.
- Empty allocators take no bytes.
  An allocator member is declared `[[no_unique_address]]`, so a container over a stateless allocator is no larger than its own state.
  Checked by `check_empty_member_bytes`.
- No wrapper without its own state or invariant.
  A type that only forwards to one member is a name for that member, so use an alias or a function instead.
- Concepts are `a_`/`an_` noun phrases.
  A concept names what satisfies it, as `an_allocator` or `a_task_callback`: `an_` before a vowel sound, singular, with no `is_` or `has_` prefix and no `_concept` or `_t` suffix.
  Checked by `check_concept_names`.

## Dispatch Classes

A CPU class bundles the waiter, the atomic reference and the StringZilla kit its silicon generation guarantees, in `src/engine.hpp`.
Only a class whose kit, waiter or atomic reference differs from its parent's holds a variant, and a `static_assert` refuses a compiled class that no longer advances.

| Value | Name            | Parent          | Waiter        | Atomics           | Strings kit                |
| ----: | :-------------- | :-------------- | :------------ | :---------------- | :------------------------- |
|     0 | `unknown`       |                 |               |                   | picks the newest that fits |
|     1 | `serial`        |                 | PAUSE, YIELD  | standard          | serial                     |
|     2 | `monoversioned` |                 | preferred     | preferred         | what the floor compiled    |
|     3 | `intel-haswell` | `serial`        | PAUSE         | standard          | Haswell, Westmere          |
|     4 | `intel-sierra`  | `intel-haswell` | TPAUSE        | CMPCCXADD         | Haswell, Goldmont          |
|     5 | `intel-skylake` | `intel-haswell` | PAUSE         | standard          | Skylake                    |
|     6 | `intel-icelake` | `intel-skylake` | PAUSE         | standard          | Ice Lake, Goldmont         |
|    12 | `aws-g2`        | `serial`        | YIELD         | standard          | NEON, AES, SHA             |
|    13 | `apple-m2`      | `aws-g2`        | YIELD         | LSE under RCpc    | NEON, AES, SHA             |
|    15 | `aws-g5`        | `apple-m2`      | WFE           | LSE under RCpc    | NEON, AES, SHA             |
|    17 | `alibaba-c930`  | `serial`        | WRS           | Zacas             | RVV                        |
|    18 | `ibm-power10`   | `serial`        | `pause_short` | ISA 3.0 AMOs      | Power VSX                  |

The serial floor spins on PAUSE or YIELD, a hint every core of its architecture has, rather than yielding to the scheduler.

A GPU class names one generation of one vendor: `nvidia-ampere`, `nvidia-ada`, `nvidia-hopper`, `nvidia-blackwell`, `nvidia-blackwellrtx`, `amd-cdna3`, `amd-cdna4`, `amd-cdna5`, `apple-metal7`, `apple-metal9` and `apple-metal10`.
Its value 0 is `unknown`, which a listed device holds when no compiled code runs on it.

### Picking Rules

The CPU pick reads the libraries' detected masks, StringZilla's `sz_capabilities_detected_cpu` and ForkUnion's `cpu_capabilities`, never their enabled ones, as UStore compiles its own kits.
A `cpu_class_ceiling` no class takes is `ustore_status_invalid_argument_k`, and a known one intersects with the machine's masks, so a class the machine lacks narrows the pick rather than refusing it.
The ladder then takes the newest compiled class whose masks fit under that intersection, and a build that compiled none, as a monoversioned build for a newer CPU, refuses with `ustore_status_unsupported_k`.
A description reports the class picked, never the ceiling.

Every GPU class compiles its own bundle of its vendor's kernels for the configured codes naming its generation, so the classes partition those codes.
Each vendor's oldest class is its floor, which holds later NVIDIA GPUs through Ampere's own PTX; a configured code no class claims, like Turing's, RDNA's or one newer than every class, stops the configure.
A monoversioned build keeps only the newest class of each vendor holding codes.
The GPU pick walks the device's vendor ladder newest first and takes the first class whose bundle the device's driver loads, at or under the executor's `gpu_class_ceiling`, and a device that loads none there refuses with `ustore_status_unsupported_k`.
A class unit reads its class from its own `USTORE_TARGET_<VENDOR>_<CLASS>` and writes StringZilla's GPU floor target off, so its bundle holds UStore's kernels alone over StringZilla's building blocks.
It scans with its generation's kit: `ampere_scan_t` for every NVIDIA class, `cdna3_scan_t` for CDNA3 and CDNA4, and `cdna5_scan_t`.
A future NVIDIA GPU takes the newest class whose PTX its driver compiles, and a device no class serves lists `unknown` and refuses executors with `ustore_status_unsupported_k`.

## Test Suites

### Principles

Suites hold no framework.
The C++ suites share `test/harness.hpp`, every C test file opens with the `Shared C Checks` region, the Python suites share `test/base.py` and `test/conftest.py`, the Rust tests share `rust/testing.rs`, and `mojo/test.mojo` holds its own helpers at its top.
Each of those holds a pinned region, `Shared Settings`, `Shared Run`, `Shared C Checks`, `Shared Pytest` or `Shared Testing`, whose tokens are identical across the Second Decade libraries and which the pre-commit hook keeps identical, so a fix lands in all of them.
The pinned C++ regions print with `std::printf`, the one exception to `fmt::println`, so every library's suites print the same bytes.

Tests reach the library through its public surface: the C ABI in `c/ustore/`, the C++ wrappers in `cpp/ustore/`, the Python, Rust and Mojo bindings, and the templates of `include/ustore/` that the component suites check below the C ABI.
The calls past that surface are listed under [Private Surface Waivers](#private-surface-waivers), each with what the public surface lacks.
Oracles never compile out: a test checks with `expect_true` and its siblings, never `assert`, and every C and C++ test unit opens with `#undef NDEBUG // ! A test's oracle must stay live in every build`.
Benchmarks never include `test/`, and each tree keeps its own harness.

A harness holds one mutable global, `test_run`, the documented exception to [Errors and Memory](#errors-and-memory)'s rule against mutable globals.
Checks fire on worker threads and inside helpers as well as in a case's own body, so the run counts failed checks in one place and `run_test_case` diffs that count around each case, rather than threading a tally through every call.
After a crash, `p test_run` in a core dump names the case in progress and the last failed check, and the crash handlers print the case before the backtrace.

Settings are read once, and a value that does not parse prints one line naming it and exits with 1.
Skips are honest: a skipped case names its reason, a filter that selects no case fails the run, and `USTORE_NO_SKIPS=1` turns any skip into exit code 3.
Every reason takes one of four forms, and a reason used twice is a constant, such as `needs_directory_k` or `needs_transactions_k`:

- `needs <symbol>`, like `needs ustore_capabilities_transactions_k` or `needs USTORE_DIRECTORY`;
- `built without <SWITCH>`, like `built without USTORE_WITH_ROCKSDB`;
- `<symbol> is refused on this machine`, like `ucall_io_backend_linux_uring618_k is refused on this machine`;
- `no <device>`, like `no GPU device`.

Every engine suite runs on every engine and isolation the build holds, and a case needing a capability skips on the engines that lack it.
Inputs come from `stream_key(seed, name, index)`, identical in C, C++, Python, Rust and Mojo, with each case keyed by its own label, so a case draws the same inputs whichever cases ran before it, and `USTORE_SEED` with `USTORE_FILTER` reproduces any one of them.

### Running

The `dev` preset builds every suite, benchmark and tool, and CTest runs each engine suite once per engine:

```sh
cmake --preset dev -DUSTORE_WITH_ROCKSDB=1 -DUSTORE_WITH_LEVELDB=1
cmake --build --preset dev
ctest --preset dev
```

Executables land in `build_dev/build/bin/`, and any of them runs alone:

```sh
build_dev/build/bin/ustore_test_abi_blobs --engine rocksdb --directory /dev/shm/ustore
USTORE_ENGINE=rocksdb USTORE_DIRECTORY=/dev/shm/ustore build_dev/build/bin/ustore_test_abi_blobs
```

A suite prints the library and the machine, then the settings it read, one verdict per case and a summary.
A failed check prints where it failed and the operands, and the case goes on, so one run lists every mismatch it reaches; the failing case then prints the line that reruns it alone:

```text
UStore 1.0.0
- Compiled for: …
- This machine: …
- Seed: 42, scale: 1
- Filter: none
- test_ring_holds_a_medium_in_flight(transport_t::memory_k) ... ok (0.12 s)
  check failed at test/nvme/ring.cpp:120
    expected 4, got 3
- test_ring_wraps_its_slots(transport_t::memory_k) ... FAILED: 1 failed check(s)
  rerun: build_dev/build/bin/ustore_test_nvme_ring --seed 42 --filter '^test_ring_wraps_its_slots\(transport_t::memory_k\)$'
- test_ring_holds_a_medium_in_flight(transport_t::device_k) ... skipped: needs USTORE_DEVICE
2 passed, 1 failed, 1 skipped, seed 42
```

Verdicts and the summary go to standard output, and failures and reruns to standard error.
The exit code is 1 when a case or a check failed or no case ran, 3 when `USTORE_NO_SKIPS` refused a skip, and 0 otherwise.
`USTORE_TRAP_ON_FAILURE=1` raises `SIGTRAP` at the first failed check instead, which stops a debugger there, or dumps a core without one:

```sh
USTORE_TRAP_ON_FAILURE=1 USTORE_FILTER='^test_ring_wraps_its_slots\(transport_t::memory_k\)$' gdb -ex run --args build_dev/build/bin/ustore_test_nvme_ring
```

The bindings run their suites as CI does:

```sh
pip install --group test . && python -X faulthandler -m pytest -x   # Python, once per engine the extension holds
cargo test --features serde                                         # Rust, over RAM, RocksDB and LevelDB
pixi run test                                                       # Mojo, against the library on the loader path
CGO_LDFLAGS="-L$PWD/build_release" go test -race ./golang/...       # Go, against the archive CGO_LDFLAGS names
```

pytest prints `UStore <version> from <path>`, the capabilities of each engine and the same `- Seed: …, scale: …` line in its header.

### Environment

Every setting is a variable, and a flag of the same name overrides it in every suite and benchmark: `USTORE_ENGINE_TRANSPORT` is `--engine-transport`.
A flag takes its value as `--name value` or `--name=value`, and a switch given alone, like `--no-skips`, is on.

| Variable                     | Flag                    | Default               | Meaning                                                                       |
| :--------------------------- | :---------------------- | :-------------------- | :---------------------------------------------------------------------------- |
| `USTORE_SEED`                | `--seed`                | `42`                  | Seed every stream descends from, or `random` to draw one, which a run prints  |
| `USTORE_FILTER`              | `--filter`              | unset                 | C++ only: ECMAScript regex over case labels and benchmark rows                |
| `USTORE_SCALE`               | `--scale`               | `1`                   | Multiplier of every baseline iteration count, like `0.1` or `10`              |
| `USTORE_NO_SKIPS`            | `--no-skips`            | off                   | Exits with 3 when any case skipped                                            |
| `USTORE_TRAP_ON_FAILURE`     | `--trap-on-failure`     | off                   | Raises `SIGTRAP` at the first failed check                                    |
| `USTORE_DIRECTORY`           | `--directory`           | unset                 | Parent of the unique directory a suite works in                               |
| `USTORE_DEVICE`              | `--device`              | unset                 | Block device the `device` transport opens                                     |
| `USTORE_SPDK_ADDRESS`        | `--spdk-address`        | unset                 | PCIe address of the device SPDK binds                                         |
| `USTORE_TRANSPORTS`          | `--transports`          | every transport       | Transports to mount, like `memory,linux-uring`                                |
| `USTORE_ENGINE`              | `--engine`              | `ram`                 | Engine to open: `ram`, `rocksdb`, `leveldb` or `nvme`                         |
| `USTORE_ISOLATION`           | `--isolation`           | `strict-serializable` | Isolation of `ram` transactions, or `snapshot`                                |
| `USTORE_ENGINE_TRANSPORT`    | `--engine-transport`    | `automatic`           | Transport of `nvme`, like `linux-uring`                                       |
| `USTORE_THREADS`             | `--threads`             | every core            | Threads the concurrent cases fan out over, or a list in the stress suites     |
| `USTORE_CLI`                 | `--cli`                 | unset                 | `tools.cpp`: the CLI executable its round trip drives                         |
| `USTORE_SPDK_ROOT`           | `--spdk-root`           | unset                 | `nvme/linux_spdk_disk.cpp`: SPDK tree with a finished build                   |
| `USTORE_SPDK_SERIAL`         | `--spdk-serial`         | the rig's             | `nvme/linux_spdk_disk.cpp`: serial the controller must identify as            |
| `USTORE_LARGE_UNIVERSE`      | `--large-universe`      | `600000`              | `nvme/compaction.cu`: random keys of the cases that cross the GPU threshold   |
| `USTORE_TIME_LIMIT`          | `--time-limit`          | `10s`                 | Stress: bound of the whole run, shared evenly by every configuration          |
| `USTORE_CHECKPOINTS`         | `--checkpoints`         | `1000`                | Stress: most rounds one configuration runs                                    |
| `USTORE_BATCH_SIZES`         | `--batch-sizes`         | `10,100,150`          | Atomicity: consecutive keys one transaction changes                           |
| `USTORE_KEYS`                | `--keys`                | `100000`              | Atomicity: keys one round writes, split into batches                          |
| `USTORE_DELETES_PERIODICITY` | `--deletes-periodicity` | `3`                   | Atomicity: one batch in this many is erased by the deleting case              |
| `USTORE_TRANSACTIONS`        | `--transactions`        | `1,2,…,10,20,50,100`  | Linearizability: transactions every thread commits between two checkpoints    |
| `USTORE_WRITES_PER_KEY`      | `--writes-per-key`      | `4`                   | Linearizability: mean writes one key receives per round, sizing the key range |

The Python suites read `USTORE_SEED` and `USTORE_SCALE`, the Rust tests read `USTORE_SEED`, and the Mojo suite reads none.
A C++ filter that does not compile as a regex stops the run, the C suites run every case, and pytest selects through its own `-k`.

With a directory, a suite creates a unique `ustore-XXXXXX` directory under it, runs every case inside it, and removes it at exit, so parallel runs never share a database; without one, the cases that need persistence or files skip, naming the setting they need.
CTest passes `test_data` inside the build directory as the directory, the engine suites get their engine, and the tools suite also gets the CLI it drives on that engine.

Flags are long kebab-case words with no short forms or aliases, sizes hold IEC suffixes, durations hold `ms`, `s` or `m`, and lists are comma-separated.
A namespace-scope `_k` constant names an invariant — a format, an alignment, an ABI limit — while workload sizes, thread counts and paths are settings with defaults.
No binary hard-codes a directory, and the CLI and the tools take flags alone, parsed by `tools/arguments.hpp`:

```sh
# Rejected
USTORE_TEST_PATH=/tmp/db ./twitter -tw 1000 --max_input_files 4 --mlimit 1048576 -n 10

# Accepted
./ustore_bench_twitter --directory /tmp/db --dataset data/twitter --max-tweets 1000 --max-input-files-total 4 --time-limit 10s
USTORE_DIRECTORY=/tmp/db ./ustore_bench_twitter --dataset data/twitter --max-tweets 1000 --time-limit 10s
```

### Writing a Test

A C++ case is `void test_<behaviour>(arguments…)`, or returns `case_result_t` when it may skip, and takes no settings or context: the run is global.
`main` prints the machine, starts the run, registers every case under its label and answers the run's verdict:

```cpp
int main(int argc, char** argv) {
    print(probe_machine());
    start_test_run(argc, argv);
    run_test_case("test_ring_holds_a_medium_in_flight<memory_disk_t>", test_ring_holds_a_medium_in_flight<memory_disk_t>, *disk);
    return finish_test_run();
}
```

A label is `<area>.<case>[.<variant>…]`, every segment lowercase snake, where the area is the file's stem and the case is the function's name without `test_`.
Each check counts a failure at the caller's line, lets the case go on, and returns whether it held:

- `expect_true(held)`, for any condition;
- `expect_equal(actual, wanted)`, which prints both;
- `expect_near(actual, wanted, tolerance)`, which holds within `tolerance · max(1, |actual|, |wanted|)`;
- `expect_status(outcome, wanted)` and its shorthand `expect_success(outcome)`, which print the statuses by name;
- `expect_throws<std::out_of_range>(callable)`, whose subject is the failure.

A message is always the trailing `format_at_t` and its arguments, as in `expect_true(found, "key {}", key)`.
Where the step after a failed check would index or dereference past what it checked, the case stops on the check's answer: `if (!expect_equal(found.size(), wanted.size())) return;`.
A case that cannot run returns `case_result_t::skipped(reason)`, and generators draw from `split_mix_generator_t {case_stream_key(index)}`.
Waits go through `spin_until`, bounded by `wait_limit_k`, and loop counts through `scaled_iterations(baseline)`.

A C case is bracketed by `start_test_case(label)` and `finish_test_case(skip_reason)`, and checks through the file-local `expect_`, `#undef`'d at the end of the file:

```c
start_test_case("test_init_ram_starts_empty");
test_init_ram_starts_empty();
finish_test_case(NULL);
```

A Python test is `test_<subject>_<behaviour>`, a sentence with a one-line docstring, and asks for what it opens by fixture: `database`, `view` (the database or a transaction over it), `later` (a deferred view), `arena`, the `blobs`, `docs` and `graph` collections, `seed`, `rng` and `unraisable`.
A case taking `engine` runs once per engine and isolation; `@pytest.mark.needs("transactions")` skips it on the engines without, and `@pytest.mark.any_engine` runs it on the first engine alone, for a case whose engine sits below what it checks, like the conversions of `blobs.py::test_batch_round_trip`.
A missing dependency is a decorator, as in `@pytest.mark.skipif(not pyarrow_available, reason="PyArrow is not installed")`, and only a fixture skips from its body.
Matrices are named in upper snake case, like `BATCH_SIZES`, and `MAX_CASES_PER_TEST` in `test/base.py` refuses a test whose cases outgrow it beside its engines.

A Rust test is named as a behaviour sentence with no prefix, returns `()` and unwraps, so a failure names its line; only `readme_quickstart` returns the `Result` of the README's own `main`.
It skips through `return skipped("built without the nvme feature")` and the like, which says so on standard error, takes its directories from `ScratchDirectory::new(name)`, its random streams from `case_stream_key(index)`, and polls futures with `block_on`.
A test that needs a cargo feature is compiled under `#[cfg(feature = "…")]`, never branched on `cfg!` in its body.

A Mojo test is `test_<behaviour>`, registered in `main` in the order it is defined, and opens with `if not _library_is_present(): _skipped(library_missing_k)`, as Mojo has no discovery; its private helpers and types take a leading underscore.

One concept holds one stem in every language: the Rust name is the stem, and C, C++, Python and Mojo call it `test_<stem>`, as in `readme_quickstart`, `snapshots_save_a_database_of_their_own`, `databases_open_at_a_directory_and_keep_what_they_held`, `a_held_executor_refuses_the_next_call`, `compact_plans_stay_live_until_finished` and `progress_reports_and_cancels`.
`test/readme.py` runs the README Quickstarts verbatim, as `test_readme_quickstart` and `test_readme_mojo_mirror`.

### Benchmarks

The benchmarks of `bench/` share `bench/harness.hpp`, which holds the same pinned `Shared Settings` region, so `USTORE_SEED`, `USTORE_FILTER`, `USTORE_WARMUP`, `USTORE_TIME_LIMIT` and every other benchmark flag read as the suites' settings do.
Rows follow the suites' label grammar, every number comes from the `release` preset, and [`bench/README.md`](bench/README.md) describes the suite, its flags and its datasets.
The evaluation harnesses outside this repository read `USTORE_LIBRARY_DIR` and `USTORE_CUDA_ARCH`, which keep their names.

### Devices

The GPU suites of `test/vram/` build into the `cuda`, `rocm` and `metal` presets, and on a machine without a device every GPU case reports `skipped: no GPU device`.
The NVMe suites mount every transport the machine offers: without `USTORE_DEVICE` they skip the `device` transport, and `scripts/rigs/nvmet_loop.sh up` hands out a RAM-backed NVMe namespace to pass to it, while SPDK needs `USTORE_SPDK_ADDRESS`.

### Private Surface Waivers

These tests reach past the public surface, each because the surface lacks what it checks:

- `test/abi/fixture.hpp` and `test/abi/executors.cpp` take `engine_t`, `view_t`, `ustore_database_handle_t` and the verb tables from `src/engine.hpp`, which the C ABI keeps behind opaque handles, and `executors.cpp` also takes `tasks_per_word_k` from `src/utilities/slices.hpp`.
- `test/vram/towers.cpp`, `test/nvme/tree.cpp` and the compaction suites of `test/nvme/` take the GPU class tables of `src/vram/classes.hpp`, which no public header exports.
- `test/vram/fake_amd_bundles.cpp` compiles `src/vram/bundles.cpp` itself, so the bundle loader runs over a fake HIP runtime.
- `test/tools/*.cpp` take `tools/arrow_ipc.hpp` and `tools/dataset.h`, which the CLI keeps to itself.
- `rust/db.rs`'s `a_call_without_an_arena_is_refused` calls `ffi::ustore_collection_list` with a null arena, which the safe API cannot express.
- `rust/db.rs`'s `statuses_map_spell_and_tolerate_newer_libraries` builds raw `ffi::ustore_outcome_t` values and calls the crate-private `check` and `Status::from_raw`, as a status from a newer library reaches Rust no other way.
- `rust/db.rs`'s `a_forgotten_deferred_call_holds_its_database_open` reads the private `submitted` field of `Deferred`, which no public accessor exposes.

## Bindings

Every binding is a thin layer over the C interface, and these rules hold in each of them.
Typed bindings are those whose compiler checks callback signatures, and dynamic bindings are the rest; in UStore the typed ones are Rust, Mojo, Go and C++, and the dynamic ones are Python.

The C library decides what is valid.
A binding forwards its status, turns each status into its language's error in one place, re-checks nothing the library refuses, and keeps no copy of state the library already holds.
Name tables come from the library's `ustore_*_name` and `ustore_*_from_name` pairs, never from a switch written in the binding.

One mechanism reports every outcome: `ustore_outcome_status` reads it without waiting, and `ustore_outcome_wait` parks until it is final.
Nothing in a binding takes a lock, sends a notification or adds synchronization of its own.
The object that submitted a deferred call is the only one that polls its outcome, so no other thread ever waits on it, and whatever a second caller asks, the library answers.
A blocking `wait` calls `ustore_outcome_wait`.
An asynchronous bridge never parks a thread: it reads `ustore_outcome_status` and, while the call is deferred, yields to its runtime.
A runtime that can reschedule a task reads again on its next turn with no sleep, like Python's `loop.call_soon`, a C++ scheduler's `post(poll, state)` and Mojo's runtime, while one that resumes only on a timer pauses with a backoff growing from about 1 µs to 1 ms, like Swift's `Task.sleep`, JavaScript's `setTimeout` and C#'s `Task.Delay`.
A Rust future yields only: `poll` reads the status and, while it is deferred, calls `wake_by_ref` and returns `Poll::Pending`, with no sleep and no timer thread.

A callback never raises into the library.
Its fault never unwinds through C frames and never waits in a latch for the call to end: typed bindings give their callback types no way to throw, like a `noexcept` C++ callback type, while dynamic bindings report the fault through their language's unraisable path, like Python's `sys.unraisablehook` and JavaScript's uncaught-exception handler, and the callback answers cancel.
A Rust callback that panics aborts the process, as Rust does for any unwind reaching an `extern "C"` frame; nothing catches the unwind.
A binding that hands the library no callback of its own has nothing to report.

A view the library hands back lives until the next call on its arena.
The library describes where the memory sits, and the caller checks it; the binding neither copies the view to be safe nor checks its residency on the caller's behalf.

Every array a verb reads shares the call's one count, so mismatched shapes cannot be expressed.
Where a binding's own types hold two lengths, like a slice of keys beside a matrix of values, it checks once that they agree before the call crosses into the library, and nowhere else.

Each verb has one body, which fills the C struct and calls the library with no layers in between.
What it returns is read from the struct's output slot, a trampoline receives the handler itself as its state with no hub around it, and whether a call blocks or defers follows from the type of the receiver it is called on, never from a flag.
Verbs are never shared through a base class; a trait whose default methods hold the one body per verb is fine, like Rust's and Mojo's, since each receiver supplies only its handle, and C++ writes each handle and its receivers as one class template over what the receiver holds, so the handle carries the verbs as Rust's `&Handle` does.

A typed binding takes the scalar as a type parameter of each call, like *Example (USearch): `search::<f32>`*, rather than letting a handle's type fix one.
A deferred Rust call owns everything the library reads until the call is final, since Rust's lifetimes cannot see the library holding it.
A C++ wrapper over the C interface is a full typed binding: it has a deferred receiver, `noexcept` callback types and per-call scalar templates.

A dynamic binding publishes every field of every report the library fills.
A type the binding hands to C by cast, like a slice of parts passed as the C array, is a transparent wrapper over the C struct, like Rust's `#[repr(transparent)]` newtype, so the cast is sound by construction and nothing asserts its size or alignment.
Tests never restate a struct's layout that the compiler already pins: no `offset_of!`, `size_of`, `MemoryLayout` or `Marshal.SizeOf` asserts against the C structs; `offsetof` tables that declare members are declarations, not tests.
Tests of how names print and parse, and of an engine run again through the binding, stay.
A binding test asserts a status, or the exception or error type mapped from it, and never the wording of a message or note, in any language: no pytest `match=`, no comparison of `error.message`, `err.Error()` or a note's text, and no substring search in them, since engines refine their notes over time.

Bindings wrap the C ABI and never instantiate C++ templates themselves.
A binding is usable once it covers opening and closing, binary reads and writes, named collections, transactions and scans; documents and graphs follow in any order.

A binding has no life of its own: the C executor owns every thread, and every C free waits for the deferred calls naming its handle, so a binding starts no threads and counts no calls, transactions or snapshots.
Every verb whose outputs scale with the call takes an arena made through `ustore_arena_init` and answers views viewing it, while shorthand with no place for an arena makes a one-off arena and frees it with its result.
A receiver chooses the executor and the completion: `on(executor)` blocks on it, and `deferred(executor)` offers the same verbs answering deferred calls.
Opening has no database yet, so every binding's constructor blocks and takes the executor to block on as an optional argument, as `database_t::open_rocksdb(options, executor)` does in C++, while only the C opens defer through their `completion`.
A deferred call polls only through `ustore_outcome_status`, and releasing one rides the language's own lifetime construct, which waits first.
A walk is a cursor whose every pull takes an arena and names its executor, which the cursor never keeps, and a language's loop shorthand owns one arena for the loop's lifetime.
A progress callback runs on the thread that runs the call, an executor's own for a deferred one, so a binding's callback enters its runtime there.
The Design sections of [`rust/README.md`](rust/README.md) and [`python/README.md`](python/README.md) spell these rules out in each language.

A verb called on the handle itself runs on the calling thread, `on(executor)` blocks on an executor, and `deferred(executor)` submits there, with the same arguments in every language.
A deferred call allocates nothing in C++, Rust or Mojo: C++ and Mojo construct it immovable where the caller holds it and submit at once, while a Rust `Deferred` submits on its first poll or `wait`, once pinned in place.
A callback crosses as the C struct it lowers to, built by one generic constructor per handler shape with a trampoline made per handler type, never a trait object, a box or an enum.
A per-key predicate lowers as cheap, and a batch predicate as expensive unless the caller says it is cheap.
A predicate may run on several executor threads at once, so every binding takes only handlers safe to call concurrently: Rust's `Fn + Sync`, a C++ handler called through `const&`, a goroutine-safe Go function, Mojo's read-only judge state, while Python's calls stay serialized by the GIL.
An arena never outlives the executor that made it: Rust's `Arena<'executor>` and Mojo's arena origin enforce the order, and the other bindings document it.
Brackets and name lookups only find, and creating or dropping a collection stays an explicit database verb.

`.githooks/pre-commit` refuses locks and reference counts in binding sources, throwing callback types, caught unwinds and kept exceptions.

## Python

`pyproject.toml` builds with `scikit-build-core`, which configures the top-level CMake tree with `USTORE_BUILD_PYTHON=ON`, leaving suites, benchmarks and tools at their default off, then installs the `ustore_python` target into the wheel.
`python/CMakeLists.txt` defines that target as the C11 extension `ustore.lib` from `python/lib.c` and one source per C header beside it, linked against `ustore::static` and installed into the `python/ustore/` package, and the build tree lives in `build_python/`.
Python suites sit in `test/` beside the C++ ones as `<topic>.py`, named so no module shadows a library it imports, and `pytest` collects every module there, running each case once per engine in `ustore.engines_compiled()`.

```sh
pip install -e .                                     # RAM, RocksDB and LevelDB
CMAKE_ARGS="-DUSTORE_WITH_ROCKSDB=OFF" pip install .  # RAM and LevelDB
pip install --group test . && python -X faulthandler -m pytest -x
```

The suites read `USTORE_SEED` and `USTORE_SCALE` as [Environment](#environment) lists, select through pytest's own `-k`, and pytest prints them in its header, so a failing run reruns with the same values.

An engine is left out through `CMAKE_ARGS` or `--config-settings=cmake.define.USTORE_WITH_<ENGINE>=OFF`, while NVMe stays out until its sources build; the build reads no environment variable of its own.

## Rust

`Cargo.toml` and `build.rs` at the root build the crate from `rust/lib.rs`, and `build.rs` compiles no C or C++ itself.
The crate's own usage is described in [`rust/README.md`](rust/README.md).
It configures CMake with `USTORE_BUILD_SHARED=ON` and `USTORE_INSTALL=ON`, installs into `OUT_DIR`, and links the `ustore_static` archive with the archives of the engines its features enable, and the target's C++ standard library.
`USTORE_LIBRARY_DIR` names a directory already holding those archives, such as the `lib/` of a prefix installed with `USTORE_BUNDLE_SIBLINGS=OFF`, and skips the CMake build.

- __RAM__ always builds, in the `ustore_static` archive the crate links.
- __rocksdb and leveldb__ are default features, each adding its engine to that library; without one, its `Database::open_*` fails with `Status::Unsupported`.
- __nvme__ is off by default until NVMe's sources build.

```sh
cargo test                                           # RAM, RocksDB and LevelDB
cargo test --no-default-features --features rocksdb  # RAM and RocksDB alone
```

`cargo package` skips every subdirectory holding a `Cargo.toml`, so ForkUnion's and StringZilla's manifests are moved aside before packaging.

## Commits

`.githooks/commit-msg` checks every message:

- The title reads `<Verb>: <Summary>`, where the verb is one of `Add`, `Break`, `Chore`, `Docs`, `Fix`, `Improve` or `Make`.
- The summary opens with a capital letter, unless it opens with a code identifier, which keeps its own case.
- The title runs to at most 80 characters, and a body line to at most 100 columns, unless it holds a URL or is indented code.
- The message holds no AI-assistant attribution, since authorship is the human who committed.
- `Break` marks a change to what the project offers, such as a dropped modality or binding; `Chore` removes or rearranges without changing it.
- The body is prose wrapped near 80 columns and explains the change in primary logic, not renames or formatting.
- One commit per topic, however many files it touches; an intermediate commit may leave the tree unbuildable.

Git's merge, `fixup!` and `squash!` titles are exempt from the title rules.
A `Release: vX.Y.Z` title is exempt too, but its commit must stamp every version site, and no other commit may change `VERSION`.
`.githooks/pre-commit` formats and judges the staged lines, and `.githooks/selftest` runs both hooks against their fixtures.
On a pull request, `.github/scripts/check-commits.sh` replays `commit-msg` over every commit and runs `pre-commit` over the whole range.

## Releases

`.github/scripts/mint-release.sh X.Y.Z` cuts a release from a clean checkout of the branch it ships.
It stamps every version site and commits `Release: vX.Y.Z`.
`.github/scripts/check-release.sh` then checks that commit, even in a clone where `core.hooksPath` is unset, before the script tags it and pushes both.
Then it publishes the GitHub release, marking `X.Y.Z-rc.N` candidates as prereleases.
Publishing starts every `release-<binding>.yml` at once, and each runs `check-release.sh` before it builds anything, so each registry trusts only its own binding's file.

## Implementing a New Engine

An engine fills the table of calls in `src/engine.hpp`, one entry per call of the C ABI.
For documents and graphs it plugs in the shared implementations of `src/docs_layer.hpp` and `src/graph_layer.hpp`, which run on its own reads and writes, as RocksDB and LevelDB do, or serves them itself, as `ram` serves graphs on half-edges of its own.
An entry the engine leaves null answers `ustore_status_unsupported_k`, as every pages entry does today.
Start with these, in order:

1. Opening and closing: its own `ustore_database_open_<engine>()` and `ustore_database_free()`, which the router calls once no transaction, snapshot or cursor holds the database open.
2. Description of the engine, the transactions, collections and snapshots it supports, its isolation and its estimates of keys, memory and disk bytes: `ustore_database_describe()`.
3. Binary values in the main collection: `ustore_blobs_write()`, `ustore_blobs_remove()`, `ustore_blobs_read()`.
4. Collections and their names: `ustore_collection_create()`, `ustore_collection_find()`, `ustore_collection_list()`, `ustore_collection_drop()`, where a name lookup and a listing read the head or the snapshot they take.
5. Snapshots and views: `ustore_snapshot_init()`, `ustore_snapshot_free()`, and the `view_t` every call receives, which the router decodes from the `ustore_view_t` of a read or a scan, the transaction of a write, or the `snapshot` of a name lookup or a listing.
6. Transactions: `ustore_transaction_init()`, `ustore_transaction_stage()`, `ustore_transaction_commit()`, `ustore_transaction_rollback()`, `ustore_transaction_free()`.
7. Scans, resuming from the last returned key: `ustore_blobs_scan()`, which the router's cursors compose with the read, so cursors need no entry of their own.
8. Strings keyed by their own bytes: `ustore_strings_write()`, `ustore_strings_remove()`, `ustore_strings_read()`, `ustore_strings_scan()`.
9. Sampling and measurements: `ustore_blobs_sample()`, `ustore_blobs_measure()`.
10. Upkeep of the whole database: `ustore_database_clear()` in every `ustore_drop_mode_t`, `ustore_database_compact()` and `ustore_snapshot_save()`.
11. Outputs: every output whose length scales with the call lands in the call's `arena` through `include/ustore/utilities/ram_buffers.hpp`, while the router makes and frees arenas in `ustore_arena_init()` and `ustore_arena_free()`.

Scans and saves report through the `progress_t` of `src/utilities/progress.hpp`, in the units their verbs document, and unwind with `progress_cancelled_k` once `advance` answers false, leaving nothing a caller would read: a cancelled save removes what it wrote.
A compaction is a plan instead, as [Planned Operations](#planned-operations) describes: the driver reports at every barrier and starts no phase after a cancel.

The conformance suite checks every advertised isolation level against behaviour, so report the level the engine delivers, not the one it aims for.
