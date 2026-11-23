// Statuses, enums, tapes, arenas, executors and the cost of a call.
//
// File: golang/types_test.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore_test

import (
	"errors"
	"runtime"
	"testing"

	ustore "github.com/unum-cloud/ustore/golang"
)

// fixture opens a database in memory with a blobs collection, an arena and a two-thread executor,
// closing all of them when the test ends.
func fixture(t *testing.T) (*ustore.Database, ustore.CollectionID, *ustore.Arena, *ustore.Executor) {
	t.Helper()
	database, err := ustore.DatabaseInitRam(nil)
	if err != nil {
		t.Fatalf("the database must open: %v", err)
	}
	t.Cleanup(func() { _ = database.Close() })
	people, err := database.CollectionCreate("people", ustore.ModalityBlobs)
	if err != nil {
		t.Fatalf("the collection must be created: %v", err)
	}
	executor, err := ustore.ExecutorForRam(nil, 2, ustore.CPUClassUnknown)
	if err != nil {
		t.Fatalf("the executor must spawn: %v", err)
	}
	t.Cleanup(func() { _ = executor.Close() })
	arena, err := ustore.NewArena(executor)
	if err != nil {
		t.Fatalf("the arena must be made: %v", err)
	}
	t.Cleanup(func() { _ = arena.Close() })
	return database, people, arena, executor
}

// keys spells integers as keys.
func keys(values ...uint64) []ustore.Key {
	spelled := make([]ustore.Key, len(values))
	for index, value := range values {
		spelled[index] = ustore.KeyFromUint64(value)
	}
	return spelled
}

func TestStatusesRoundTripAndMatch(t *testing.T) {
	if ustore.StatusNotFound.String() != "not-found" {
		t.Fatalf("spelled %q", ustore.StatusNotFound.String())
	}
	status, err := ustore.StatusFromName("write-conflict")
	if err != nil || status != ustore.StatusWriteConflict {
		t.Fatalf("read back %v, %v", status, err)
	}
	if _, err := ustore.MoodFromName("sleepy"); !errors.Is(err, ustore.StatusInvalidArgument) {
		t.Fatalf("an unknown spelling answered %v", err)
	}
	var failure error = &ustore.Error{Status: ustore.StatusContended, Message: "held"}
	if !errors.Is(failure, ustore.StatusContended) {
		t.Fatal("errors.Is missed the status of an Error")
	}
}

func TestTapesPanicOnOverrun(t *testing.T) {
	if ustore.TapeOf("Ada", "", "Grace").Len() != 3 {
		t.Fatal("three strings packed into another count")
	}
	defer func() {
		if recover() == nil {
			t.Fatal("offsets past the bytes did not panic")
		}
	}()
	ustore.U32Tape([]byte("Ada"), []uint32{0, 4})
}

func TestArenasLiveOnTheHost(t *testing.T) {
	_, _, arena, _ := fixture(t)
	description, err := arena.Describe()
	if err != nil {
		t.Fatalf("the arena must describe itself: %v", err)
	}
	if description.Residency != ustore.ResidencyRam || description.Device != -1 {
		t.Fatalf("described %+v", description)
	}
}

func TestClosedHandlesRefuseBeforeCrossing(t *testing.T) {
	database, people, arena, executor := fixture(t)
	closed, err := ustore.ExecutorForRam(nil, 1, ustore.CPUClassUnknown)
	if err != nil {
		t.Fatalf("the executor must spawn: %v", err)
	}
	_ = closed.Close()
	if _, err := ustore.NewArena(closed); !errors.Is(err, ustore.StatusUninitialized) {
		t.Fatalf("a closed executor answered %v", err)
	}
	_ = database.Close()
	if _, err := database.BlobsRead(people, keys(1), arena, nil); !errors.Is(err, ustore.StatusUninitialized) {
		t.Fatalf("a closed database answered %v", err)
	}
	if deferred, err := database.Deferred(executor).Describe(); deferred != nil || !errors.Is(err, ustore.StatusUninitialized) {
		t.Fatalf("a deferred verb on a closed database answered %v, %v", deferred, err)
	}
}

func TestDeferredCallsAnswerTheSameEveryTime(t *testing.T) {
	database, people, arena, executor := fixture(t)
	if err := database.BlobsWrite(people, keys(1), ustore.TapeOf("Ada"), nil); err != nil {
		t.Fatalf("the value must be written: %v", err)
	}
	reading, err := database.Deferred(executor).BlobsRead(people, keys(1), arena, nil)
	if err != nil {
		t.Fatalf("the read must be submitted: %v", err)
	}
	first, err := reading.Wait()
	if err != nil {
		t.Fatalf("the read must finish: %v", err)
	}
	second, err := reading.Wait()
	if err != nil || first != second || reading.Status() != ustore.StatusSuccess || reading.Close() != nil {
		t.Fatalf("waited twice into %v and %v, %v", first, second, err)
	}
	if value, ok := second.Get(0); !ok || string(value) != "Ada" {
		t.Fatalf("read %q, %v", value, ok)
	}
}

func TestDroppedDeferredCallsReleaseThemselves(t *testing.T) {
	database, people, arena, executor := fixture(t)
	for range 8 {
		if _, err := database.Deferred(executor).BlobsSample(people, 4, arena); err != nil {
			t.Fatalf("the sample must be submitted: %v", err)
		}
		if err := executor.Drain(ustore.MoodParked); err != nil {
			t.Fatalf("the executor must drain: %v", err)
		}
		runtime.GC()
	}
	runtime.GC()
	if _, err := database.On(executor).BlobsSample(people, 4, arena); err != nil {
		t.Fatalf("the arena stayed held after its dropped calls: %v", err)
	}
}

func TestCallsAllocateOnlyTheirFrames(t *testing.T) {
	database, people, arena, executor := fixture(t)
	written := keys(1, 2)
	values := ustore.TapeOf("Ada", "Grace")
	spanned := ustore.Each{people, people}
	closed, err := ustore.NewArena(nil)
	if err != nil {
		t.Fatalf("the arena must be made: %v", err)
	}
	_ = closed.Close()
	on, deferred := database.On(executor), database.Deferred(executor)
	for name, verb := range map[string]func(){
		"blocking write":    func() { _ = on.BlobsWrite(people, written, values, nil) },
		"blocking read":     func() { _, _ = on.BlobsRead(spanned, written, arena, nil) },
		"blocking scan":     func() { _, _ = on.BlobsScan(people, 4, arena, nil) },
		"blocking describe": func() { _, _ = on.Describe() },
		"refused read":      func() { _, _ = on.BlobsRead(people, written, closed, nil) },
	} {
		if allocations := testing.AllocsPerRun(100, verb); allocations != 1 {
			t.Errorf("a %s allocated %v times", name, allocations)
		}
	}
	// The holder stands for the Deferred, whose cleanup the runtime registers the same way.
	cleanup := testing.AllocsPerRun(100, func() { runtime.AddCleanup(new([2]uintptr), func(uint) {}, 0) })
	for name, verb := range map[string]func(){
		"deferred read": func() {
			call, _ := deferred.BlobsRead(people, written, arena, nil)
			_, _ = call.Wait()
		},
		"deferred describe": func() {
			call, _ := deferred.Describe()
			_, _ = call.Wait()
		},
	} {
		if allocations := testing.AllocsPerRun(100, verb); allocations != 1+cleanup {
			t.Errorf("a %s allocated %v times, %v of them registering its cleanup", name, allocations, cleanup)
		}
	}
}
