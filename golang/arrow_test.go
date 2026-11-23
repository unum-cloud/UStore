// Arrow columns exported from an arena and viewed back as inputs.
//
// File: golang/arrow_test.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore_test

import (
	"slices"
	"testing"

	ustore "github.com/unum-cloud/ustore/golang"
)

func TestArrowColumnsRoundTrip(t *testing.T) {
	database, people, arena, _ := withPeople(t)
	scanned, err := database.BlobsScan(people, 10, arena, nil)
	if err != nil {
		t.Fatalf("the scan must run: %v", err)
	}
	var schema ustore.ArrowSchema
	var array ustore.ArrowDeviceArray
	if err := ustore.KeysViewToArrow(scanned.Keys(0), arena, &schema, &array); err != nil {
		t.Fatalf("the keys must export: %v", err)
	}
	defer schema.Release()
	defer array.Release()
	viewed, err := ustore.KeysViewFromArrow(&schema, array.Array())
	if err != nil || !slices.Equal(viewed, keys(1, 2, 3)) {
		t.Fatalf("viewed %v, %v", viewed, err)
	}
	// The keys live in arena, which the read would rewind under them, so it writes into another.
	fetched, err := ustore.NewArena(nil)
	if err != nil {
		t.Fatalf("the arena must be made: %v", err)
	}
	defer fetched.Close()
	values, err := database.BlobsRead(people, viewed, fetched, nil)
	if err != nil {
		t.Fatalf("the exported keys must read: %v", err)
	}
	if value, _ := values.Get(2); string(value) != "Kent" {
		t.Fatalf("read %q", value)
	}
}
