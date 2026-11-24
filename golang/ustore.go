// Package ustore binds UStore, a GPU-accelerated multi-modal transactional database engine, through
// its C ABI in c/ustore/.
//
// The package links the precompiled engine archive, libustore_static, so building it compiles no
// engine. Every verb is a batch, and every blocking verb answers its value beside an error that
// errors.Is matches against a [Status]:
//
//   - [Database], [Transaction] and [Snapshot]: the head, a group of changes, and a point in time.
//   - [DatabaseOn.On] and [DatabaseOn.Deferred]: the receivers that pick an executor and whether a
//     verb blocks or answers a [Deferred].
//   - [Arena]: where each output whose length scales with a call lands, viewed until its next call.
//   - [Executor]: the threads, rings or stream calls run on.
//
// A blocking verb allocates one pinned frame, and a deferred one adds its [Deferred] and the
// cleanup the collector runs if it is dropped. The binding starts no goroutines and takes no locks.
//
// The package example commits a blob, a document and an edge in one transaction, then reads them.
//
// File: golang/ustore.go
// Author: Ash Vardanian
// Date: November 18, 2026
package ustore
