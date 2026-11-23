// Pages: XML imports, attribute edits and renderings.
//
// File: golang/pages_test.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore_test

import (
	"testing"

	ustore "github.com/unum-cloud/ustore/golang"
)

func TestPagesImportAndRender(t *testing.T) {
	database, _, arena, _ := fixture(t)
	articles, err := database.CollectionCreate("articles", ustore.ModalityPages)
	if err != nil {
		t.Fatalf("the collection must be created: %v", err)
	}
	markup := ustore.TapeOf(`<article><title>Avatar</title><p>Blue people.</p></article>`)
	if err := database.PagesImport(articles, keys(1), markup, &ustore.PagesImportConfig{Encoding: ustore.PageEncodingXML}); err != nil {
		t.Fatalf("the page must import: %v", err)
	}
	rendered, err := database.PagesRead(articles, keys(1, 2), arena, &ustore.PagesReadConfig{OutputsEncoding: ustore.PageEncodingText})
	if err != nil {
		t.Fatalf("the page must render: %v", err)
	}
	if text, ok := rendered.Get(0); !ok || len(text) == 0 {
		t.Fatalf("rendered %q, %v", text, ok)
	}
	if _, ok := rendered.Get(1); ok {
		t.Fatal("a missing page rendered")
	}
	scanned, err := database.PagesScan(articles, 10, arena, nil)
	if err != nil || len(scanned.Keys(0)) != 1 {
		t.Fatalf("scanned %v, %v", scanned.Keys(0), err)
	}
}
