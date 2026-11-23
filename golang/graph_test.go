// Graphs: edges, degrees, neighborhoods and vertex removals.
//
// File: golang/graph_test.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore_test

import (
	"testing"

	ustore "github.com/unum-cloud/ustore/golang"
)

func TestGraphsReadNeighborhoods(t *testing.T) {
	database, _, arena, _ := fixture(t)
	follows, err := database.CollectionCreate("follows", ustore.ModalityGraph)
	if err != nil {
		t.Fatalf("the collection must be created: %v", err)
	}
	vertices := keys(1, 2, 3, 7)
	first, second, third, seventh := vertices[0], vertices[1], vertices[2], vertices[3]
	edges := []ustore.Edge{
		{Source: first, Target: second, ID: ustore.EdgeIDDefault},
		{Source: first, Target: third, ID: ustore.EdgeIDDefault},
		{Source: second, Target: third, ID: seventh},
	}
	if err := database.GraphWriteEdges(follows, edges, nil); err != nil {
		t.Fatalf("the edges must be written: %v", err)
	}
	found, err := database.GraphReadEdges(follows, []ustore.Key{first, third}, ustore.VertexRoleAny, arena)
	if err != nil {
		t.Fatalf("the edges must read: %v", err)
	}
	if degree, ok := found.Degree(0); !ok || degree != 2 {
		t.Fatalf("the first vertex has degree %d, %v", degree, ok)
	}
	incoming := found.Edges(1)
	if len(incoming) != 2 || incoming[1].Neighbor(third) != second || incoming[1].ID != seventh {
		t.Fatalf("the third vertex meets %v", incoming)
	}
	if err := database.GraphRemoveVertices(follows, []ustore.Key{first}, nil); err != nil {
		t.Fatalf("the vertex must be removed: %v", err)
	}
	found, err = database.GraphReadEdges(follows, []ustore.Key{first, third}, ustore.VertexRoles{ustore.VertexRoleSource, ustore.VertexRoleTarget}, arena)
	if err != nil {
		t.Fatalf("the edges must read again: %v", err)
	}
	if _, ok := found.Degree(0); ok {
		t.Fatal("a removed vertex still has a degree")
	}
	if degree, _ := found.Degree(1); degree != 1 {
		t.Fatalf("the third vertex kept %d incoming edges", degree)
	}
}
