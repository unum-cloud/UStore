// Collections, transactions, snapshots, change feeds and ingests.
//
// File: golang/db_test.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore_test

import (
	"errors"
	"testing"

	ustore "github.com/unum-cloud/ustore/golang"
)

func TestCollectionsResolveByName(t *testing.T) {
	database, people, arena, _ := fixture(t)
	if _, err := database.CollectionCreate("people", ustore.ModalityBlobs); !errors.Is(err, ustore.StatusAlreadyExists) {
		t.Fatalf("a taken name answered %v", err)
	}
	found, err := database.CollectionFind(ustore.TapeOf("people", "nowhere"), arena)
	if err != nil {
		t.Fatalf("the names must resolve: %v", err)
	}
	if id, modality, ok := found.Get(0); !ok || id != people || modality != ustore.ModalityBlobs {
		t.Fatalf("found %v, %v, %v", id, modality, ok)
	}
	if _, _, ok := found.Get(1); ok {
		t.Fatal("a missing name resolved")
	}
	listed, err := database.CollectionList(arena)
	if err != nil || listed.Len() != 1 || string(listed.Name(0)) != "people" || listed.ID(0) != people {
		t.Fatalf("listed %d collections, %v", listed.Len(), err)
	}
	if err := database.CollectionDrop(people, ustore.DropCollection); err != nil {
		t.Fatalf("the collection must drop: %v", err)
	}
	if err := database.BlobsWrite(people, keys(1), ustore.TapeOf("Ada"), nil); !errors.Is(err, ustore.StatusNotFound) {
		t.Fatalf("a write to a dropped collection answered %v", err)
	}
}

func TestTransactionsCommitAllOrNothing(t *testing.T) {
	database, people, arena, _ := fixture(t)
	transaction, err := database.TransactionInit()
	if err != nil {
		t.Fatalf("the transaction must begin: %v", err)
	}
	defer transaction.Close()
	if err := transaction.BlobsWrite(people, keys(1), ustore.TapeOf("Ada"), nil); err != nil {
		t.Fatalf("the write must join the transaction: %v", err)
	}
	if values, err := database.BlobsRead(people, keys(1), arena, nil); err != nil || values.Len() != 1 {
		t.Fatalf("the head read failed: %v", err)
	} else if _, ok := values.Get(0); ok {
		t.Fatal("the head saw an uncommitted write")
	}
	if peek, err := transaction.View(ustore.WatchNone).BlobsRead(people, keys(1), arena, nil); err != nil {
		t.Fatalf("the transaction must read its own write: %v", err)
	} else if value, ok := peek.Get(0); !ok || string(value) != "Ada" {
		t.Fatalf("the transaction read %q", value)
	}
	stamp, err := transaction.Commit(ustore.DurabilityBuffered)
	if err != nil {
		t.Fatalf("the commit must land: %v", err)
	}
	snapshot, err := database.SnapshotInit()
	if err != nil {
		t.Fatalf("the snapshot must pin the head: %v", err)
	}
	defer snapshot.Close()
	if ustore.CommitStamp(snapshot.ReadStamp()) < stamp {
		t.Fatalf("a snapshot at %d misses the commit at %d", snapshot.ReadStamp(), stamp)
	}
}

func TestConcurrentWritersConflict(t *testing.T) {
	database, people, _, _ := fixture(t)
	first, err := database.TransactionInit()
	if err != nil {
		t.Fatalf("the first transaction must begin: %v", err)
	}
	defer first.Close()
	second, err := database.TransactionInit()
	if err != nil {
		t.Fatalf("the second transaction must begin: %v", err)
	}
	defer second.Close()
	for _, transaction := range []*ustore.Transaction{first, second} {
		if err := transaction.BlobsWrite(people, keys(1), ustore.TapeOf("Ada"), nil); err != nil {
			t.Fatalf("the write must join: %v", err)
		}
	}
	if _, err := first.Commit(ustore.DurabilityBuffered); err != nil {
		t.Fatalf("the first commit must land: %v", err)
	}
	if _, err := second.Commit(ustore.DurabilityBuffered); !errors.Is(err, ustore.StatusWriteConflict) {
		t.Fatalf("the second commit answered %v", err)
	}
	if err := second.Reset(); err != nil {
		t.Fatalf("the transaction must reset: %v", err)
	}
}

func TestSnapshotsKeepTheirState(t *testing.T) {
	database, people, arena, _ := fixture(t)
	if err := database.BlobsWrite(people, keys(1), ustore.TapeOf("Ada"), nil); err != nil {
		t.Fatalf("the value must be written: %v", err)
	}
	snapshot, err := database.SnapshotInit()
	if err != nil {
		t.Fatalf("the snapshot must pin the head: %v", err)
	}
	defer snapshot.Close()
	if err := database.BlobsWrite(people, keys(1), ustore.TapeOf("Grace"), nil); err != nil {
		t.Fatalf("the value must be overwritten: %v", err)
	}
	pinned, err := snapshot.BlobsRead(people, keys(1), arena, nil)
	if err != nil {
		t.Fatalf("the snapshot must read: %v", err)
	}
	if value, _ := pinned.Get(0); string(value) != "Ada" {
		t.Fatalf("the snapshot read %q", value)
	}
}

func TestDescribeAndMaintain(t *testing.T) {
	database, _, _, executor := fixture(t)
	description, err := database.Describe()
	if err != nil || description.Engine != ustore.EngineRam {
		t.Fatalf("described %+v, %v", description, err)
	}
	if err := database.On(executor).Flush(); err != nil {
		t.Fatalf("the flush must succeed: %v", err)
	}
	if err := database.Compact(nil); err != nil {
		t.Fatalf("the compaction must run: %v", err)
	}
	if err := database.Clear(ustore.DropPairs); err != nil {
		t.Fatalf("the clear must succeed: %v", err)
	}
}

func TestChangesFollowCommits(t *testing.T) {
	database, people, arena, _ := fixture(t)
	snapshot, err := database.SnapshotInit()
	if err != nil {
		t.Fatalf("the snapshot must pin the head: %v", err)
	}
	since := ustore.CommitStamp(snapshot.ReadStamp())
	feed, err := database.ChangesCursorInit(people, since)
	_ = snapshot.Close()
	if err != nil {
		t.Fatalf("the feed must open: %v", err)
	}
	defer feed.Close()
	if err := database.BlobsWrite(people, keys(1), ustore.TapeOf("Ada"), nil); err != nil {
		t.Fatalf("the value must be written: %v", err)
	}
	changes, err := feed.NextPairs(16, arena)
	if err != nil {
		t.Fatalf("the feed must pull: %v", err)
	}
	if changes.Len() != 1 || changes.Key(0) != keys(1)[0] || changes.Kind(0) != ustore.ChangeWrite || string(changes.Value(0)) != "Ada" {
		t.Fatalf("pulled %d changes", changes.Len())
	}
}

func TestIngestsLandAsOneCommit(t *testing.T) {
	database, people, arena, _ := fixture(t)
	ingest, err := database.IngestInit(people, ustore.IngestUnsorted)
	if err != nil {
		t.Fatalf("the ingest must open: %v", err)
	}
	defer ingest.Close()
	if err := ingest.Append(ustore.IngestRows{Keys: keys(2, 1), Values: ustore.TapeOf("Grace", "Ada")}); err != nil {
		t.Fatalf("the rows must append: %v", err)
	}
	if err := ingest.Append(ustore.IngestRows{Keys: keys(3), Values: ustore.TapeOf("Kent", "extra")}); !errors.Is(err, ustore.StatusInvalidArgument) {
		t.Fatalf("a ragged batch answered %v", err)
	}
	if _, err := ingest.Finish(ustore.DurabilityBuffered); err != nil {
		t.Fatalf("the ingest must land: %v", err)
	}
	values, err := database.BlobsRead(people, keys(1, 2), arena, nil)
	if err != nil {
		t.Fatalf("the rows must read back: %v", err)
	}
	if value, _ := values.Get(1); string(value) != "Grace" {
		t.Fatalf("read %q", value)
	}
}
