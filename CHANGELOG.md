# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] - September 2022

### Added

- Collections with variable-length string keys.
- Faster `yyjson`-based backend for Docs.
- Concurrent writes support in STL backend.
- Zero-copy forwarding for Arrow/NumPy PyBind arguments with matching types.
- Supporting `substrate.io` serialized computations.

- Integer compression in Graphs.
- Media deserialization and exports via TenPack.
- Extending RPC to Document and Graph modalities.

## [0.3.0] - August 2022

### Added

- Support for shared memory exports/reads.
- Apache Arrow exports from Binary collections into columns of binary strings.
- Apache Arrow tabular exports from Document collections.
- Pandas-like interface for Document collections.
- RPC server and client with Apache Arrow.
- Binary and document reads now also output validity bitsets for Arrow compatibility.
- Separate calls for removing and clearing collections.

### Changed

- Better memory management with polymorphic allocators in arenas.
- Collection handle becomes a simple copyable integer.
- Shorter name for collection types.

### Removed

- CRUD operations on DB and transaction level in PyBind. Use `.main` to retrieve the nameless collection and operate on it.

## [0.2.0] - July 2022

### Added

- Fully-functional RocksDB backend.
- LevelDB backend, which has no support for transactions or named collections.
- Graph collection logic, which can operate on top of any binary KVS.
- Documents collection logic, which packs & queries JSONs, MsgPacks and other hierarchical docs into any binary KVS.
- Support for JSON-Pointer, JSON-Patch and JSON Merge Patch as an alternative to custom query language.
- NetworkX Python bindings with Python Protocol Buffers support.

## [0.1.0] - June 2022

### Added

- Initial binary KVS headers with "strided" arguments support.
- Initial implementation of STL-based in-memory KV store.
- PyBind-based bindings, mimicking a transactional `dict[str, dict[int, bytes]]`.
- PyBind-based interface for NumPy matrix exports.
- GoLang basic interface for single-entry operations.
- JNI interface with support for single operations and transactions.

[Unreleased]: https://github.com/unum-cloud/ukv/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/unum-cloud/ukv/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/unum-cloud/ukv/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/unum-cloud/ukv/releases/tag/v0.1.0
