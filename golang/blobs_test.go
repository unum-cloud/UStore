// Binary values: reads, slices, scans with progress and predicates, walks, samples and measures.
//
// File: golang/blobs_test.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore_test

import (
	"errors"
	"slices"
	"testing"

	ustore "github.com/unum-cloud/ustore/golang"
)

// withPeople writes Ada, Grace and Kent under the keys 1, 2 and 3.
func withPeople(t *testing.T) (*ustore.Database, ustore.CollectionID, *ustore.Arena, *ustore.Executor) {
	t.Helper()
	database, people, arena, executor := fixture(t)
	if err := database.BlobsWrite(people, keys(1, 2, 3), ustore.TapeOf("Ada", "Grace", "Kent"), nil); err != nil {
		t.Fatalf("the values must be written: %v", err)
	}
	return database, people, arena, executor
}

func TestBlobsReadWholeValuesAndSlices(t *testing.T) {
	database, people, arena, _ := withPeople(t)
	values, err := database.BlobsRead(people, keys(1, 4), arena, nil)
	if err != nil {
		t.Fatalf("the values must read: %v", err)
	}
	if value, ok := values.Get(0); !ok || string(value) != "Ada" {
		t.Fatalf("read %q, %v", value, ok)
	}
	if _, ok := values.Get(1); ok {
		t.Fatal("a missing key read as present")
	}
	sliced, err := database.BlobsRead(people, keys(2), arena, &ustore.BlobsReadConfig{Offsets: []uint64{1}, Limits: []uint64{3}})
	if err != nil {
		t.Fatalf("the slice must read: %v", err)
	}
	if value, _ := sliced.Get(0); string(value) != "rac" {
		t.Fatalf("sliced %q", value)
	}
	if _, err := database.BlobsRead(people, keys(1, 2), arena, &ustore.BlobsReadConfig{Offsets: []uint64{1}}); !errors.Is(err, ustore.StatusInvalidArgument) {
		t.Fatalf("one offset for two keys answered %v", err)
	}
}

func TestBlobsWriteBackWhatTheyRead(t *testing.T) {
	database, people, arena, _ := withPeople(t)
	values, err := database.BlobsRead(people, keys(1, 2), arena, nil)
	if err != nil {
		t.Fatalf("the values must read: %v", err)
	}
	if err := database.BlobsWrite(people, keys(5, 6), values.Tape(), nil); err != nil {
		t.Fatalf("the tape must write back: %v", err)
	}
	copied, err := database.BlobsRead(people, keys(6), arena, nil)
	if err != nil {
		t.Fatalf("the copy must read: %v", err)
	}
	if value, _ := copied.Get(0); string(value) != "Grace" {
		t.Fatalf("copied %q", value)
	}
}

func TestBlobsScanPerCollection(t *testing.T) {
	database, people, arena, _ := withPeople(t)
	scanned, err := database.BlobsScan(ustore.Each{people, people}, 10, arena, &ustore.BlobsScanConfig{StartKeys: keys(0, 2)})
	if err != nil {
		t.Fatalf("the scans must run: %v", err)
	}
	if !slices.Equal(scanned.Keys(0), keys(1, 2, 3)) || !slices.Equal(scanned.Keys(1), keys(2, 3)) {
		t.Fatalf("scanned %v and %v", scanned.Keys(0), scanned.Keys(1))
	}
	descending, err := database.BlobsScan(people, 2, arena, &ustore.BlobsScanConfig{StartKeys: []ustore.Key{ustore.KeyUnknown}, Direction: ustore.ScanDescending})
	if err != nil || !slices.Equal(descending.Keys(0), keys(3, 2)) {
		t.Fatalf("scanned %v down, %v", descending.Keys(0), err)
	}
	odd := func(key ustore.Key) bool { return key[15]%2 == 1 }
	oddBatch := func(keys []ustore.Key, admissions []ustore.Admission) {
		for index, key := range keys {
			if odd(key) {
				admissions[index] = ustore.Admit
			}
		}
	}
	for name, admitter := range map[string]ustore.Admitter{
		"per key":     ustore.Predicate(odd),
		"batch":       ustore.BatchPredicate(oddBatch),
		"cheap batch": ustore.CheapBatchPredicate(oddBatch),
	} {
		filtered, err := database.BlobsScan(people, 10, arena, &ustore.BlobsScanConfig{Predicate: admitter})
		if err != nil || !slices.Equal(filtered.Keys(0), keys(1, 3)) {
			t.Fatalf("filtered %s %v, %v", name, filtered.Keys(0), err)
		}
	}
	// Admissions start rejected, so a handler that answers nothing keeps nothing.
	silent := ustore.BatchPredicate(func([]ustore.Key, []ustore.Admission) {})
	if kept, err := database.BlobsScan(people, 10, arena, &ustore.BlobsScanConfig{Predicate: silent}); err != nil || len(kept.Keys(0)) != 0 {
		t.Fatalf("a silent predicate kept %v, %v", kept.Keys(0), err)
	}
}

func TestProgressCancels(t *testing.T) {
	database, people, arena, _ := withPeople(t)
	stop := func(finished, total uint64) bool { return false }
	if _, err := database.BlobsScan(people, 10, arena, &ustore.BlobsScanConfig{Progress: stop}); !errors.Is(err, ustore.StatusCancelled) {
		t.Fatalf("a cancelled scan answered %v", err)
	}
}

func TestBlobsCursorsWalkPages(t *testing.T) {
	database, people, arena, executor := withPeople(t)
	cursor, err := database.On(executor).BlobsCursorInit(people, ustore.Key{}, nil)
	if err != nil {
		t.Fatalf("the cursor must open: %v", err)
	}
	defer cursor.Close()
	page, err := cursor.On(executor).NextPairs(2, arena)
	if err != nil || !slices.Equal(page.Keys(), keys(1, 2)) || string(page.Values().Get(1)) != "Grace" {
		t.Fatalf("pulled %v, %v", page.Keys(), err)
	}
	page, err = cursor.NextKeys(2, arena)
	if err != nil || !slices.Equal(page.Keys(), keys(3)) || page.Values().Len() != 0 {
		t.Fatalf("pulled %v, %v", page.Keys(), err)
	}
	_ = cursor.Close()
	if _, err := cursor.NextKeys(2, arena); !errors.Is(err, ustore.StatusUninitialized) {
		t.Fatalf("a closed cursor answered %v", err)
	}
}

func TestBlobsSampleAndMeasure(t *testing.T) {
	database, people, arena, _ := withPeople(t)
	sampled, err := database.BlobsSample(people, 2, arena)
	if err != nil || len(sampled.Keys(0)) != 2 {
		t.Fatalf("sampled %v, %v", sampled.Keys(0), err)
	}
	measured, err := database.BlobsMeasure(people, arena, nil)
	if err != nil {
		t.Fatalf("the range must measure: %v", err)
	}
	if bounds := measured.Get(0); bounds.MinCardinality > 3 || bounds.MaxCardinality < 3 {
		t.Fatalf("measured %+v", bounds)
	}
}

func TestBlobsRemoveRanges(t *testing.T) {
	database, people, arena, _ := withPeople(t)
	if err := database.BlobsRemove(people, keys(1), &ustore.RemoveConfig{EndKeys: keys(3)}); err != nil {
		t.Fatalf("the range must be removed: %v", err)
	}
	scanned, err := database.BlobsScan(people, 10, arena, nil)
	if err != nil || !slices.Equal(scanned.Keys(0), keys(3)) {
		t.Fatalf("kept %v, %v", scanned.Keys(0), err)
	}
}
