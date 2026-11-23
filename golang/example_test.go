// The README quickstart, compiled and checked by go test.
//
// File: golang/example_test.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore_test

import (
	"errors"
	"fmt"
	"log"

	ustore "github.com/unum-cloud/ustore/golang"
)

func Example() {
	database, err := ustore.DatabaseInitRam(nil)
	if err != nil {
		log.Fatal(err)
	}
	defer database.Close()
	avatar, titanic := ustore.KeyFromUint64(1), ustore.KeyFromUint64(2)
	posters, err := database.CollectionCreate("posters", ustore.ModalityBlobs)
	if err != nil {
		log.Fatal(err)
	}
	movies, err := database.CollectionCreate("movies", ustore.ModalityDocs)
	if err != nil {
		log.Fatal(err)
	}
	similar, err := database.CollectionCreate("similar", ustore.ModalityGraph)
	if err != nil {
		log.Fatal(err)
	}

	// One transaction writes a blob, a document and an edge, and commits all three or none.
	transaction, err := database.TransactionInit()
	if err != nil {
		log.Fatal(err)
	}
	defer transaction.Close()
	err = errors.Join(
		transaction.BlobsWrite(posters, []ustore.Key{avatar}, ustore.TapeOf("\x89PNG"), nil),
		transaction.DocsWrite(movies, []ustore.Key{avatar}, ustore.TapeOf(`{"title":"Avatar","year":2009}`), nil),
		transaction.GraphWriteEdges(similar, []ustore.Edge{{Source: avatar, Target: titanic, ID: ustore.EdgeIDDefault}}, nil),
	)
	if err != nil {
		log.Fatal(err)
	}
	if _, err := transaction.Commit(ustore.DurabilityBuffered); err != nil {
		log.Fatal(err)
	}

	// Reads at the head see the commit: the poster, the year and the one edge leaving Avatar.
	arena, err := ustore.NewArena(nil)
	if err != nil {
		log.Fatal(err)
	}
	defer arena.Close()
	poster, err := database.BlobsRead(posters, []ustore.Key{avatar}, arena, nil)
	if err != nil {
		log.Fatal(err)
	}
	value, _ := poster.Get(0)
	fmt.Printf("%q ", value)
	movie, err := database.DocsRead(movies, []ustore.Key{avatar}, arena, &ustore.DocsReadConfig{Fields: ustore.TapeOf("year")})
	if err != nil {
		log.Fatal(err)
	}
	year, _ := movie.Get(0)
	fmt.Printf("%s ", year)
	edges, err := database.GraphReadEdges(similar, []ustore.Key{avatar}, ustore.VertexRoleSource, arena)
	if err != nil {
		log.Fatal(err)
	}
	fmt.Println(edges.Edges(0)[0].Target == titanic)
	// Output: "\x89PNG" 2009 true
}
