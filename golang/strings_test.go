// Strings mapping onto keys: writes, reads, prefix scans and walks.
//
// File: golang/strings_test.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore_test

import (
	"slices"
	"testing"

	ustore "github.com/unum-cloud/ustore/golang"
)

func TestStringsMapOntoKeys(t *testing.T) {
	database, _, arena, _ := fixture(t)
	names, err := database.CollectionCreate("names", ustore.ModalityStrings)
	if err != nil {
		t.Fatalf("the collection must be created: %v", err)
	}
	if err := database.StringsWrite(names, ustore.TapeOf("ada", "alan", "grace"), keys(1, 2, 3), nil); err != nil {
		t.Fatalf("the strings must be written: %v", err)
	}
	mapped, err := database.StringsRead(names, ustore.TapeOf("grace", "kent"), arena)
	if err != nil {
		t.Fatalf("the strings must read: %v", err)
	}
	if key, ok := mapped.Get(0); !ok || key != keys(3)[0] {
		t.Fatalf("grace maps onto %v, %v", key, ok)
	}
	if _, ok := mapped.Get(1); ok {
		t.Fatal("a missing string mapped onto a key")
	}
	listing, err := database.StringsScan(names, 10, arena, &ustore.StringsScanConfig{Prefixes: ustore.TapeOf("a")})
	if err != nil || listing.Count(0) != 2 || string(listing.Get(0, 1)) != "alan" {
		t.Fatalf("listed %d strings, %v", listing.Count(0), err)
	}
	cursor, err := database.StringsCursorInit(names, &ustore.StringsCursorConfig{Prefix: "a"})
	if err != nil {
		t.Fatalf("the cursor must open: %v", err)
	}
	defer cursor.Close()
	page, err := cursor.NextPairs(10, arena)
	if err != nil || page.Len() != 2 || string(page.Strings().Get(0)) != "ada" || !slices.Equal(page.Values(), keys(1, 2)) {
		t.Fatalf("pulled %d strings, %v", page.Len(), err)
	}
}
