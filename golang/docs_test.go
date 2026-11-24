// JSON documents: writes, field reads, gists and typed gathers.
//
// File: golang/docs_test.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore_test

import (
	"testing"

	ustore "github.com/unum-cloud/ustore/golang"
)

func TestDocumentsGatherIntoColumns(t *testing.T) {
	database, _, arena, _ := fixture(t)
	movies, err := database.CollectionCreate("movies", ustore.ModalityDocs)
	if err != nil {
		t.Fatalf("the collection must be created: %v", err)
	}
	documents := ustore.TapeOf(`{"title":"Avatar","year":2009}`, `{"title":"Titanic","year":"1997"}`)
	if err := database.DocsWrite(movies, keys(1, 2), documents, nil); err != nil {
		t.Fatalf("the documents must be written: %v", err)
	}
	rated := &ustore.DocsWriteConfig{Fields: ustore.TapeOf("rating")}
	if err := database.DocsWrite(movies, keys(1), ustore.TapeOf("7.9"), rated); err != nil {
		t.Fatalf("the field must be written: %v", err)
	}
	titles, err := database.DocsRead(movies, keys(1, 3), arena, &ustore.DocsReadConfig{Fields: ustore.TapeOf("title", "title"), OutputsType: ustore.DocsFieldTypeString})
	if err != nil {
		t.Fatalf("the titles must read: %v", err)
	}
	if title, ok := titles.Get(0); !ok || string(title) != "Avatar" {
		t.Fatalf("read %q, %v", title, ok)
	}
	if _, ok := titles.Get(1); ok {
		t.Fatal("a missing document read as present")
	}
	paths, err := database.DocsGist(movies, keys(1), arena)
	if err != nil || paths.Len() != 3 {
		t.Fatalf("found %d paths, %v", paths.Len(), err)
	}
	columns, err := database.DocsGather(movies, keys(1, 2), ustore.TapeOf("year"), []ustore.DocsFieldType{ustore.DocsFieldTypeU64}, arena)
	if err != nil {
		t.Fatalf("the column must gather: %v", err)
	}
	years := columns.Column(0)
	if !years.Valid(0) || years.Converted(0) || !years.Converted(1) {
		t.Fatal("the bitmaps disagree with the documents")
	}
	if scalars := ustore.DocsColumnScalars[uint64](years); scalars[0] != 2009 || scalars[1] != 1997 {
		t.Fatalf("gathered %v", scalars)
	}
	cursor, err := database.DocsCursorInit(movies, ustore.Key{}, &ustore.DocsCursorConfig{Fields: ustore.TapeOf("year"), ColumnsTypes: []ustore.DocsFieldType{ustore.DocsFieldTypeU64}})
	if err != nil {
		t.Fatalf("the cursor must open: %v", err)
	}
	defer cursor.Close()
	batches, err := cursor.Next(8, arena)
	if err != nil {
		t.Fatalf("the cursor must pull: %v", err)
	}
	rows := uint(0)
	for index := uint(0); index != batches.Len(); index++ {
		if columns := batches.Columns(index); columns.Len() != 1 || columns.Column(0).Len() != uint(len(batches.Keys(index))) {
			t.Fatal("a batch holds one column of its own row count")
		}
		rows += uint(len(batches.Keys(index)))
	}
	if rows != 2 {
		t.Fatalf("the cursor pulled %d rows of 2", rows)
	}
}
