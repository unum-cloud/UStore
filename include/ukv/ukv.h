/**
 * @file ukv.h
 * @author Ashot Vardanian
 * @date 12 Jun 2022
 * @brief C bindings for Universal Key-Value stores, that provide:
 * > @b ABI stability for the essential CRUD operations,
 * > @b Interoperability with higher-level languages.
 * > @b Flexibility in choosing the underlying implementation.
 * > Both transactional and "HEAD" operations.
 *
 * @section Assumptions and Limitations (in current version):
 * > Keys are preset to @b 8-byte unsigned integers.
 * > Values must be @b under 4GB long, zero length is OK too.
 * > Fully @b synchronous for the simplicity of interface.
 * > Collection names should be under 64 characters long. Postgres does 59 :)
 *
 * @section Extended Functionality: @b Docs, @b Graphs
 * We add "Document" and "Graph" typed collections, which store more than
 * just raw bytes in the underlying system. It allows to create arbitrarily
 * complex DBMS on top of it.
 *
 * @section Backends
 *
 * @subsection Embedded Backends
 * Any of the following systems runs within the same process as a
 * library of persistent associative data-structures:
 * * FOSS LevelDB: https://github.com/google/leveldb/
 * * FOSS RocksDB: https://github.com/facebook/rocksdb
 * * FOSS STL-based In-Memory
 * * Unums Persistent Transactional Embedded Key-Value Store
 * * Unums In-Memory Transactional Key-Value Store
 *
 * @subsection Standalone Backends
 * Any of the above embedded stores can be wrapped into any of
 * the following standalone systems, running as separate processes:
 * * FOSS RESTful Server with Boost.Beast: https://github.com/boostorg/beast
 * * FOSS gRPC Server:
 * * Unum RPC Server
 * * Unum Distributed RPCs server
 * The RPC variants are compatible with all frontends.
 *
 * @section Frontends
 * * Python: transactions ✓, batch ops ✓, NetworkX-like graphs ✓
 * * Java: transactions ✓, Apache Arrow support
 * * GoLang
 */

#pragma once
#include "ukv/db.h"
#include "ukv/docs.h"
#include "ukv/graph.h"