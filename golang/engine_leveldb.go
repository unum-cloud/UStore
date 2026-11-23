// Links the LevelDB engine archive under the ustore_leveldb tag, as the leveldb Cargo feature does.
//
// File: golang/engine_leveldb.go
// Author: Ash Vardanian
// Date: November 18, 2026

//go:build ustore_leveldb

package ustore

// The archive repeats ahead of LevelDB, since a static linker resolves left to right.

/*
#cgo LDFLAGS: -lustore_static -lleveldb
*/
import "C"
