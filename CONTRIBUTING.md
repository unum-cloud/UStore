# Contribution Guide

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

The repository carries its own hooks, and configuring CMake arms them; to arm them without a build:

```sh
git config core.hooksPath .githooks
```

`.githooks/pre-commit` checks the added lines of every staged file against the rules below and runs `clang-format` 22, `ruff`, `dprint` and `cmake-format` when they are installed.
`.githooks/commit-msg` checks the subject and rejects AI-assistant attribution.

The `release` and `relwithdebinfo` presets build with whatever toolchain the environment provides, MSVC, AppleClang, GCC or Clang.
`debug`, `check`, `dev`, `trace`, `tsan`, `profile` and `cuda` take Clang and lld, whose sanitizer, tracing and line-table flags they use.
`cuda` also hands Clang to `nvcc` as its host compiler, so the CUDA units mangle their templates like the C++ ones that link them.

## Layout and Namespaces

| Directory         | Contents                                                           | Namespace                |
| :---------------- | :----------------------------------------------------------------- | :----------------------- |
| `include/ustore/` | C ABI headers as `*.h`, C++ wrappers of the same names as `*.hpp`  | `unum::ustore`           |
| `c/`              | `lib.cpp` with the C ABI dispatch, `docs.cpp` and `graph.cpp`      | `unum::ustore`           |
| `src/<engine>/`   | `engine.cpp` and the engine's own headers                          | `unum::ustore::<engine>` |
| `src_oracles/`    | `engine.cpp` of the third-party engines kept as oracles            | `unum::ustore::<engine>` |
| `tools/`          | the CLI, flag parsing, and dataset import and export               | `unum::ustore`           |
| `test/`           | suites mirroring `src/`, one file per component, and their harness | `unum::ustore::test`     |
| `bench/`          | benchmarks mirroring `src/`, with a harness of their own           | `unum::ustore::bench`    |
| `verification/`   | Spin models and GenMC checks, run by `check.sh`                    | none                     |

Namespaces mirror directories and nothing else: a directory adds a namespace component, and no namespace groups symbols by theme.
A file name never repeats its directory — `src/nvme/lsm_tree.hpp`, never `src/nvme/nvme_lsm_tree.hpp`.

## C++

### Naming

- `_t` marks concrete types and the aliases that materialize a template; `_k` marks enumerators and constants.
- Class templates carry no suffix: `class lsm_tree`, never `class lsm_tree_gt`.
- Template type parameters end in `_type_` and private members in one underscore, so a parameter never shadows a member: `typename locations_type_` beside `locations_type_ locations_`.
- Identifiers are whole words: `index`, `count`, `transaction`, `directory` — never `i`, `cnt`, `txn`, `dir`.
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
#define lsm_tree_def_m template <typename stored_key_, value_layout_t value_layout_>
#define lsm_tree_args_m stored_key_, value_layout_
lsm_tree_def_m class lsm_tree;

// Written out, constrained by a concept
template <typename stored_key_type_, value_layout_t value_layout_>
    requires stored_key_concept<stored_key_type_>
class lsm_tree;
```

A macro survives only as an object-like configuration flag or include guard.
`#if` brackets whole declarations or include blocks, never a fragment of an `if`/`else`.

### Errors and Memory

Nothing throws.
Fallible calls return a status or `expected`, constructors that can fail become factories returning `expected`, and no code adds `try` or `catch` except one `try` per exported `ustore_*` call in the RocksDB and LevelDB engines, whose libraries throw.
Errors are checked with an explicit `if` and `return`, never a macro.
The status is `status_t {status_code_t code; char const* message;}`, mirroring the C ABI's `ustore_error_t`, and `expected<value_type_>` keeps its value in a union alive only on success, so a move-only type needs no empty state.

No owning standard type that can throw appears anywhere, tests included:

| Instead of                     | Use                                                                              |
| :----------------------------- | :------------------------------------------------------------------------------- |
| `std::string`                  | StringZilla `sz::basic_string` through `try_assign`, `try_reserve`, `try_resize` |
| `std::vector`                  | SmashTable `basic_vector` through `make()`                                       |
| `std::unordered_map`           | SmashTable `basic_hash_table`                                                    |
| `std::unordered_set`           | SmashTable `basic_hash_table`                                                    |
| `std::map`, `std::set`         | SmashTable `basic_avl_tree`, or `basic_wb_tree` for rank and select              |
| An array sized once            | ForkUnion `dynamic_array`, whose `resize()` returns a status                     |
| `std::thread`, `std::function` | The ForkUnion executor below, and a template callback parameter                  |

Non-owning `std::span` and `std::string_view` are welcome.
No mutable global state: no namespace-scope mutable variables, no function-local `static`, and never `static thread_local`.
Per-thread scratch belongs to the executor that owns the threads.

### Threads and Placement

Threads, topology and domain-placed memory come from ForkUnion's C ABI in `forkunion.h`, wrapped once by a C++ executor and allocator; nothing else calls `fu_*` directly.
The executor owns a `fu_topology_t` and a `fu_pool_t` created through `fu_topology_new`, `fu_pool_new` and `fu_pool_spawn`, returns `expected` from its factory, deletes the pool in its destructor, and forwards C++ callables to the pool through a C callback.
The allocator places memory with `fu_allocate_on_domain_id` and releases it with `fu_free_on_domain_id`.

### Documentation and Comments

- Every file opens with `@file`, `@author`, `@date` and `@brief`, in that order and contiguous; `@file` names the path from the repository root and `@date` reads like `January 9, 2023`.
- Docblock text sits two spaces past its asterisk, as in ` *  @brief`, and a continuation indents two more.
- Declarations are documented with `/** */`, never with a bare `//` line above them, and every `/**` has an empty line above it.
- A block that is one unstructured paragraph carries no `@brief`: `/** What it is. */`.
- Every `@param` states its direction: `@param[in]`, `@param[out]` or `@param[inout]`.
- No Markdown inside docblocks — no headings, bullets or `__bold__`; use `@note`, `@throws`, `@c`, `@p` and plain sentences.
- A code span inside a docblock is `@c token`, a shape or a range is `@b [minimum,maximum]` with the whitespace inside the brackets squeezed out, and a span too long for either becomes plain prose; backticks stay in `//` line comments and in the trailing comments of `#include` lines.
- Document all fields of a type or none of them, each with its own line.
- Comments stay to one line, two at most, state what the code cannot, and never narrate history.
- Sections are `#pragma region Name` and `#pragma endregion Name`, with letters, digits and spaces in the label, never banners of asterisks, dashes or equals signs.

### Includes and Printing

Includes form groups separated by blank lines, alphabetical inside each: C library headers such as `<cstdint>`, platform headers such as `<unistd.h>`, C++ standard headers, external libraries such as `<rocksdb/db.h>` and `<yyjson.h>`, sibling projects such as `<smashtable/basic_vector.hpp>`, then UStore's own headers in quotes, such as `"ustore/db.h"` and `"helpers/linked_memory.hpp"`.
Sibling projects are reached through the aliases `fu`, `nk`, `st` and `sz`, declared once in `include/ustore/types.hpp`, never through spelled-out `ashvardanian::` namespaces or local aliases.
C++ prints through `fmt::println` — no iostreams, no `printf`.
Discard a result through `[[maybe_unused]] T const result = call();`, never `(void)call();`, and declare return types before the name, never after an arrow.

## Tests and Benchmarks

Tests use `test/test.hpp`, not a framework: every check returns a failure count, and counts roll up to `main`.

```cpp
std::size_t lsm_tree_insert_then_find() {
    std::size_t failures = 0;
    failures += expect(inserted);
    failures += expect_eq(found_count, 3u);
    return failures;
}

int main() {
    std::size_t total = 0;
    total += report("lsm_tree.insert_then_find", lsm_tree_insert_then_find());
    return report("total", total) ? 1 : 0;
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

## Python Builds

`pyproject.toml` builds with `scikit-build-core`, which configures the top-level CMake tree with `USTORE_BUILD_LIB_PYTHON=ON` and suites, benchmarks and tools off, then installs the `ustore_python` target into the wheel.
`python/CMakeLists.txt` defines that target as the C11 extension `ustore.lib` from `python/lib.c`, installed into the `python/ustore/` package, and the build tree lives in `build_python/`.
Python suites sit in `test/` beside the C++ ones as `<topic>.py`, named so no module shadows a library it imports, and `pytest` collects every module there, running each case once per engine in `ustore.engines_compiled()`.

```sh
pip install -e .                                     # every engine
CMAKE_ARGS="-DUSTORE_WITH_ROCKSDB=OFF" pip install .  # all but RocksDB
pip install --group test . && python -X faulthandler -m pytest -x
```

`USTORE_SEED` pins the seed the suites draw from, `42` by default or `random` for a fresh one, and `USTORE_FILTER` keeps the cases whose node id matches it, as a regex or else as a substring.
pytest prints both in its header as `- Seed:` and `- Filter:`, so a failing run reruns with the same values.

An engine is left out through `CMAKE_ARGS` or `--config-settings=cmake.define.USTORE_WITH_<ENGINE>=OFF`, and `USTORE_BUILD_NVME=OFF` for NVMe; the build reads no environment variable of its own.

## Rust Builds

`Cargo.toml` and `build.rs` at the root build the crate from `rust/lib.rs`, and `build.rs` compiles no C or C++ itself.
The crate's own usage is described in [`rust/README.md`](rust/README.md).
It configures CMake with `USTORE_BUILD_LIB_C=ON`, `USTORE_INSTALL=ON` and suites off, installs the static libraries into `OUT_DIR`, and links them with the target's C++ standard library.

- __RAM__ always builds, in the one `ustore` library the crate links.
- __rocksdb, leveldb and nvme__ are default features, each adding its engine to that library; without one, its `Database::open_*` fails with `Status::Unsupported`.

```sh
cargo test                                        # every engine
cargo test --no-default-features --features nvme  # RAM and NVMe alone
```

`cargo package` skips every subdirectory holding a `Cargo.toml`, so ForkUnion's and StringZilla's manifests are moved aside before packaging.

## Commits

- The subject is `Verb: Summary` with one of `Add`, `Break`, `Build`, `Chore`, `Docs`, `Fix`, `Improve`, `Make`, `Release`, and the summary starts with a capital.
- `Break` marks a change to what the project offers, such as a dropped modality or binding; `Chore` removes or rearranges without changing it.
- The body is prose wrapped near 80 columns and explains the change in primary logic, not renames or formatting.
- One commit per topic, however many files it touches; an intermediate commit may leave the tree unbuildable.
- No AI-assistant attribution of any kind.

## Implementing a New Engine

An engine implements the calls from `db.h` and `blobs.h`; the documents and graph modalities in `c/` run on top of them for every engine.
Start with these, in order:

1. Opening and closing: `ustore_database_init()`, `ustore_database_free()`.
2. Metadata, including the isolation level the engine delivers: `ustore_database_capabilities()`.
3. Binary values in the main collection: `ustore_write()`, `ustore_read()`.
4. Collections and their names: `ustore_collection_create()`, `ustore_collection_find()`, `ustore_collection_list()`, `ustore_collection_drop()`.
5. Transactions: `ustore_transaction_init()`, `ustore_transaction_stage()`, `ustore_transaction_commit()`, `ustore_transaction_free()`.
6. Scans, resuming from the last returned key: `ustore_scan()`.
7. Sampling and measurements: `ustore_sample()`, `ustore_database_control()`, `ustore_measure()`.
8. Memory: `ustore_arena_free()`.

An advertised isolation level is a promise the conformance suite checks against behaviour, so report the level the engine delivers, not the one it aims for.

## Implementing a New Language Binding

Bindings wrap the C ABI and never instantiate C++ templates themselves.
A binding is usable once it covers opening and closing, binary reads and writes, named collections, transactions and scans; documents and graphs follow in any order.
