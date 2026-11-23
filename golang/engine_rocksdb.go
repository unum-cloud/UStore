// Links the RocksDB engine archive under the ustore_rocksdb tag, as the rocksdb Cargo feature does.
//
// File: golang/engine_rocksdb.go
// Author: Ash Vardanian
// Date: November 18, 2026

//go:build ustore_rocksdb

package ustore

// The archive repeats ahead of RocksDB, since a static linker resolves left to right.

/*
#cgo LDFLAGS: -lustore_static -lrocksdb
#cgo linux LDFLAGS: -luring
*/
import "C"
