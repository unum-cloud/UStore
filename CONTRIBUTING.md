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

The repository carries its own hooks, and configuring CMake arms them; to arm them without a build:

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

- __`ustore_static`__, aliased `ustore::static` and `ustore::ustore`, takes the objects of ForkUnion, StringZilla, yyjson and fmt, and links the engines' own archives, RocksDB, LevelDB and liburing, which install beside it in the same CMake package, so a change to UStore never rewrites them. The suites, benchmarks and tools inside the tree link it as `ustore::static` and reach the siblings through it.
- __`ustore_shared`__, aliased `ustore::shared`, links the same libraries privately, compiles with `USTORE_EXPORTING`, hands its consumers `USTORE_SHARED`, and exports only the functions `USTORE_EXPORT` marks, as hidden visibility, a `ustore_*` version script on Linux, Android and FreeBSD and `-exported_symbol,_ustore_*` on macOS keep every other symbol inside.

`c/test.c` builds against both, as `ustore_test_c` and `ustore_test_c_shared`, and CTest runs both.
`USTORE_INSTALL` installs the two libraries, the headers, a `.pc` file and a CMake package under `lib/cmake/ustore`, which `find_package(ustore)` reads as `ustore::static` and `ustore::shared`.
The install splits into components: `sdk` holds the shared library, `c/ustore/`, `cpp/ustore/`, the package and the license, `static` the archive with the engines' archives, and `core` the header-only `include/ustore/`.
`cpack --config build_release/CPackConfig.cmake` archives the `sdk` component alone, stripped, as the preview SDK `ustore-<version>-<platform>-sdk.tar.gz`, a ZIP on Windows.
A function the headers declare without `USTORE_EXPORT` links against the archive but is missing from the shared library, so every new declaration carries it.

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
- Class templates carry no suffix: `class lsm_tree`, never `class lsm_tree_gt`.
- Template type parameters end in `_type_` and private members in one underscore, so a parameter never shadows a member: `typename locations_type_` beside `locations_type_ locations_`.
- Identifiers are whole words: `index`, `count`, `transaction`, `directory` — never `i`, `cnt`, `txn`, `dir`.
- `init`, `min`, `max`, `id`, `gds`, `ref` and vendor terms are whole words of the vocabulary and stay as they are.
- A transport type carries its operating system, as in `linux_spdk_environment`, while a type mirroring a vendor's own struct or API table keeps the vendor word, as in `cufile_api_t`.
- A type is named `<domain>_<role>`: the domain word says what it holds and the role word what it does, as in `lsm_tree`, `lsm_level`, `lsm_sorted_run`, `sorted_run_index`, `key_slot`, `stored_value`, `write_buffer` and `graph_store`.
  Only C ABI terms stay one word — `database`, `collection`, `transaction`, `snapshot` — and no name is a metaphor or a legacy project name.
- A file is named after its main type without `_t` and never repeats its directory; C++ headers are `.hpp`, never `.h` or `.tcc`.
- Concepts end in `_concept`, like `stored_key_concept`, so a concept never collides with a member of the same name.
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
    requires stored_key_concept<stored_key_type_>
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
The status is `status_t {status_code_t code; char const* message;}`, mirroring the C ABI's `ustore_outcome_t`, and `expected<value_type_>` keeps its value in a union alive only on success, so a move-only type needs no empty state.

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
Per-thread scratch belongs to the executor that owns the threads.

### Threads and Placement

Threads, topology and domain-placed memory come from ForkUnion's C ABI in `forkunion/forkunion.h`, wrapped once by the C++ executor of `include/ustore/utilities/ram_device.hpp` and the arenas of `include/ustore/utilities/ram_buffers.hpp`; nothing else calls `fu_*` directly.
The executor owns a `fu_topology_t` and a `fu_pool_t` created through `fu_topology_new`, `fu_pool_new` and `fu_pool_spawn`, returns `expected` from its factory, deletes the pool in its destructor, and forwards C++ callables to the pool through a C callback.
A pool another library lends through `ustore_executor_on_pool` is wrapped by `lent_pool_t` instead, which dispatches on it but never deletes or parks it.
The arenas place their blocks with `fu_allocate_on_domain_id` and release them with `fu_free_on_domain_id`.

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
Its phases are `flush`, `merge` and `reclaim`, between `none` and `done`, and each engine runs the ones it has: RAM a merge per collection, then one reclaim; NVMe a flush, then merge rounds; RocksDB a flush per column family, then a merge per family; LevelDB a merge per collection's key range.
An engine's `compact_open` lists its units once, the plan's only allocation, marking which may run at once, and `compact_advance` runs any one unit, in place of `tasks`, `run` and `advance`: RAM merges and RocksDB flushes are tasks the router's driver fans out over the executor, while RocksDB and LevelDB merges and NVMe rounds are steps, one after another.
The driver reports `ustore_phase_progress_t` as units finish, a cancel lets running units finish and starts no more, and a plan the cancel leaves live continues past every unit that finished.

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

Statuses 0 to 19 carry the same numbers and meanings in every Unum library: success 0, deferred 1, out_of_memory 2, shape_mismatch 3, unsupported_layout 4, unknown_kind 5, read_only 6, io 7, device 8, malformed 9, internal 10, overflow 11, uninitialized 12, config_mismatch 13, contended 14, cancelled 15, unsupported 16, invalid_argument 17, not_found 18, already_exists 19.
A library leaves out the ones it cannot answer but never renumbers the rest, and numbers its own statuses from 20.
Every enum the C interface names, statuses included, has a `ustore_*_name` answering "unrecognized" for a value it does not know and a `ustore_*_from_name(view, &out)` answering `invalid_argument` for a name it does not know, leaving `out` as it was.
CPU and GPU class values are ABI: each class keeps the number USearch gives the same hardware, written explicitly, with unused numbers reserved rather than reused.
`ustore_version_name` answers the full version, pre-release suffix included, in the engine and in the stub alike.

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
status_t run_plan(index_type_& index) noexcept requires is_nvme_index<index_type_> { /* NVMe rounds */ }

template <typename index_type_>
status_t run_plan(index_type_& index) noexcept { /* every other residency */ }
```

`if constexpr` branches on a named concept or a `constexpr bool`, one level deep.
An `else if constexpr` chain is still one level.

```cpp
template <typename query_type_>
concept hinted_query = requires(query_type_& query) { query.find_hinted(0u); };

if constexpr (hinted_query<query_type_>) seeds = query.find_hinted(shard); // allowed: a named concept
else if constexpr (serial_class_k) seeds = {};                            // allowed: one chain, one level
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
- A block that is one unstructured paragraph carries no `@brief`: `/** What it is. */`.
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

## Dispatch Classes

A CPU class bundles the waiter, the atomic reference and the StringZilla kit its silicon generation guarantees, in `src/engine.hpp`.
Only a class whose kit, waiter or atomic reference differs from its parent's carries a variant, and a `static_assert` refuses a compiled class that no longer advances.

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
Its value 0 is `unknown`, which a listed device carries when no compiled code runs on it.

### Picking Rules

The CPU pick reads the libraries' detected masks, StringZilla's `sz_capabilities_detected_cpu` and ForkUnion's `cpu_capabilities`, never their enabled ones, as UStore compiles its own kits.
A `cpu_class_ceiling` no class takes is `ustore_status_invalid_argument_k`, and a known one intersects with the machine's masks, so a class the machine lacks narrows the pick rather than refusing it.
The ladder then takes the newest compiled class whose masks fit under that intersection, and a build that compiled none, as a monoversioned build for a newer CPU, refuses with `ustore_status_unsupported_k`.
A description reports the class picked, never the ceiling.

Every GPU class compiles its own bundle of its vendor's kernels for the configured codes naming its generation, so the classes partition those codes.
Each vendor's oldest class is its floor, which carries later NVIDIA GPUs through Ampere's own PTX; a configured code no class claims, like Turing's, RDNA's or one newer than every class, stops the configure.
A monoversioned build keeps only the newest class of each vendor holding codes.
The GPU pick walks the device's vendor ladder newest first and takes the first class whose bundle the device's driver loads, at or under the executor's `gpu_class_ceiling`, and a device that loads none there refuses with `ustore_status_unsupported_k`.
A class unit reads its class from its own `USTORE_TARGET_<VENDOR>_<CLASS>` and writes StringZilla's GPU floor target off, so its bundle holds UStore's kernels alone over StringZilla's building blocks.
It scans with its generation's kit: `ampere_scan_t` for every NVIDIA class, `cdna3_scan_t` for CDNA3 and CDNA4, and `cdna5_scan_t`.
A future NVIDIA GPU takes the newest class whose PTX its driver compiles, and a device no class serves lists `unknown` and refuses executors with `ustore_status_unsupported_k`.

## Tests and Benchmarks

Tests use `test/harness.hpp`, not a framework: every check returns a failure count, and counts roll up to `main`.

```cpp
std::size_t lsm_tree_insert_then_find() {
    std::size_t failures = 0;
    failures += expect(inserted);
    failures += expect_eq(found_count, 3u);
    return failures;
}

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options); status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();
    report(totals, "lsm_tree.insert_then_find", lsm_tree_insert_then_find);
    return suite_exit_code(totals);
}
```

`expect_eq` prints both values, so it needs types `fmt` can format; compare anything else through `expect(a == b)`.
Waits on a peer thread use `spin_until`, and checks inside worker threads add into an atomic counter folded in after the threads finish.

### Flags and Constants

Binaries read no environment variables and no hard-coded directories; every input is a flag parsed by `tools/arguments.hpp`.

```sh
# Rejected
USTORE_TEST_PATH=/tmp/db ./twitter -tw 1000 --max_input_files 4 --mlimit 1048576 -n 10

# Accepted
./ustore_bench_twitter --directory /tmp/db --dataset data/twitter --max-tweets 1000 --max-input-files-total 4 --time-limit 10s
```

Flags are long kebab-case words with no short forms or aliases, sizes carry IEC suffixes, durations carry `ms` or `s`, and lists are comma-separated.
A namespace-scope `_k` constant names an invariant — a format, an alignment, an ABI limit — while workload sizes, thread counts and paths are option fields with defaults.

## Bindings

Every binding is a thin layer over the C interface, and these rules hold in each of them.
Typed bindings are those whose compiler checks callback signatures, and dynamic bindings are the rest; in UStore the typed ones are Rust, Mojo and C++, and the dynamic ones are Python.

The C library decides what is valid.
A binding forwards its status, turns each status into its language's error in one place, re-checks nothing the library refuses, and keeps no copy of state the library already holds.
Name tables come from the library's `ustore_*_name` and `ustore_*_from_name` pairs, never from a switch written in the binding.

One mechanism reports every outcome: `ustore_outcome_status` reads it without waiting, and `ustore_outcome_wait` parks until it is final.
Nothing in a binding takes a lock, sends a notification or adds synchronization of its own.
The object that submitted a deferred call is the only one that polls its outcome, so no other thread ever waits on it, and whatever a second caller asks, the library answers.
A blocking `wait` calls `ustore_outcome_wait`.
An asynchronous bridge never parks a thread: it reads `ustore_outcome_status` and, while the call is deferred, yields to its runtime.
A runtime with a timer pauses between reads with a backoff growing from about 1 µs to 1 ms, like Python's `asyncio.sleep`, Swift's `Task.sleep`, JavaScript's `setTimeout` and C#'s `Task.Delay`.
A Rust future yields only: `poll` reads the status and, while it is deferred, calls `wake_by_ref` and returns `Poll::Pending`, with no sleep and no timer thread.

A callback never raises into the library.
Its fault never unwinds through C frames and never waits in a latch for the call to end: typed bindings give their callback types no way to throw, like a `noexcept` C++ callback type, while dynamic bindings report the fault through their language's unraisable path, like Python's `sys.unraisablehook` and JavaScript's uncaught-exception handler, and the callback answers cancel.
A Rust callback that panics aborts the process, as Rust does for any unwind reaching an `extern "C"` frame; nothing catches the unwind.
A binding that hands the library no callback of its own has nothing to report.

A view the library hands back lives until the next call on its arena.
The library describes where the memory sits, and the caller checks it; the binding neither copies the view to be safe nor checks its residency on the caller's behalf.

Every array a verb reads shares the call's one count, so mismatched shapes cannot be expressed.
Where a binding's own types carry two lengths, like a slice of keys beside a matrix of values, it checks once that they agree before the call crosses into the library, and nowhere else.

Each verb has one body, which fills the C struct and calls the library with no layers in between.
What it returns is read from the struct's output slot, a trampoline receives the handler itself as its state with no hub around it, and whether a call blocks or defers follows from the type of the receiver it is called on, never from a flag.
Verbs are never shared through a base class; a trait whose default methods hold the one body per verb is fine, like Rust's and Mojo's, since each receiver supplies only its handle.

A typed binding takes the scalar as a type parameter of each call, like *Example (USearch): `search::<f32>`*, rather than letting a handle's type fix one.
A deferred Rust call owns what it lends the library until the call is final, since the borrow checker cannot see the library holding it.
A C++ wrapper over the C interface is a full typed binding: it has a deferred receiver, `noexcept` callback types and per-call scalar templates.

A dynamic binding publishes every field of every report the library fills.
A type the binding hands to C by cast, like a slice of parts lent as the C array, is a transparent wrapper over the C struct, like Rust's `#[repr(transparent)]` newtype, so the cast is sound by construction and nothing asserts its size or alignment.
Tests never restate a struct's layout that the compiler already pins: no `offset_of!`, `size_of`, `MemoryLayout` or `Marshal.SizeOf` asserts against the C structs; `offsetof` tables that declare members are declarations, not tests.
Tests of how names print and parse, and of an engine run again through the binding, stay.

Bindings wrap the C ABI and never instantiate C++ templates themselves.
A binding is usable once it covers opening and closing, binary reads and writes, named collections, transactions and scans; documents and graphs follow in any order.

A binding has no life of its own: the C executor owns every thread, and every C free waits for the deferred calls naming its handle, so a binding starts no threads and counts no calls, transactions or snapshots.
Every verb whose outputs scale with the call takes an arena made through `ustore_arena_init` and answers views borrowing it, while shorthand with no place for an arena makes a one-off arena and frees it with its result.
A receiver chooses the executor and the completion: `on(executor)` blocks on it, and `deferred(executor)` offers the same verbs answering deferred calls.
Opening has no database yet, so every binding's constructor blocks and takes the executor to block on as an optional argument, as `database_t::open_rocksdb(options, executor)` does in C++, while only the C opens defer through their `completion`.
A deferred call polls only through `ustore_outcome_status`, and releasing one rides the language's own lifetime construct, which waits first.
A walk is a cursor whose every pull takes an arena, and a language's loop shorthand owns one arena for the loop's lifetime.
A progress callback runs on the thread that runs the call, an executor's own for a deferred one, so a binding's callback enters its runtime there.
The Design sections of [`rust/README.md`](rust/README.md) and [`python/README.md`](python/README.md) spell these rules out in each language.

`.githooks/pre-commit` refuses locks in binding sources, throwing callback types, caught unwinds and kept exceptions.

## Python

`pyproject.toml` builds with `scikit-build-core`, which configures the top-level CMake tree with `USTORE_BUILD_PYTHON=ON`, leaving suites, benchmarks and tools at their default off, then installs the `ustore_python` target into the wheel.
`python/CMakeLists.txt` defines that target as the C11 extension `ustore.lib` from `python/lib.c` and one source per C header beside it, linked against `ustore::static` and installed into the `python/ustore/` package, and the build tree lives in `build_python/`.
Python suites sit in `test/` beside the C++ ones as `<topic>.py`, named so no module shadows a library it imports, and `pytest` collects every module there, running each case once per engine in `ustore.engines_compiled()`.

```sh
pip install -e .                                     # RAM, RocksDB and LevelDB
CMAKE_ARGS="-DUSTORE_WITH_ROCKSDB=OFF" pip install .  # RAM and LevelDB
pip install --group test . && python -X faulthandler -m pytest -x
```

`USTORE_SEED` pins the seed the suites draw from, `42` by default or `random` for a fresh one, and `USTORE_FILTER` keeps the cases whose node id matches it, as a regex or else as a substring.
pytest prints both in its header as `- Seed:` and `- Filter:`, so a failing run reruns with the same values.

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
- The message carries no AI-assistant attribution, since authorship is the human who committed.
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
A compaction is a plan instead, as [Planned Operations](#planned-operations) describes: the router's driver reports as its units finish and starts no unit after a cancel.

The conformance suite checks every advertised isolation level against behaviour, so report the level the engine delivers, not the one it aims for.
