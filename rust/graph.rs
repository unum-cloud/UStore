//! Graphs stored as an inverted index from every vertex to its edges - the Rust twin of
//! `include/ustore/graph.h`.
//!
//! An [`Edge`] is laid out as the three keys the ABI puts on its tapes, so a slice of edges crosses as the
//! source, target and identifier columns of a write, and the tape a lookup exports reads back as a slice of
//! edges without a copy.
//!
//! ```no_run
//! use ustore::{Arena, RAMOptions, Database, Durability, Edge, Key, Modality, Role, Roles};
//!
//! let database = Database::open_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new();
//! let follows = database.create_collection("follows", Modality::Graph)?;
//!
//! let [ada, grace, kent] = [1u64, 2, 3].map(Key::from);
//! let edges = [Edge::new(ada, grace), Edge::new(ada, kent), Edge::new(grace, kent)];
//! database.head().upsert_edges(follows, &edges, Durability::Buffered)?;
//!
//! let found = database.head().find_edges(&mut arena, follows, &[ada, kent], Roles::Every(Role::Any))?;
//! assert_eq!(found.degrees(), &[2, 2]);
//! assert_eq!(found.edges()[0], Edge::new(ada, grace));
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The edges a lookup found live in the arena, so another lookup through that arena does not compile while
//! they are still read:
//!
//! ```compile_fail
//! use ustore::{Arena, CollectionID, RAMOptions, Database, Durability, Key, Role, Roles};
//!
//! let database = Database::open_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new();
//! let vertices = [Key::from(1u64)];
//! let found = database.head().find_edges(&mut arena, CollectionID::main(), &vertices, Roles::Every(Role::Any))?;
//! let degrees = database.head().degrees(&mut arena, CollectionID::main(), &vertices, Roles::Every(Role::Any))?;
//! database.head().remove_edges(CollectionID::main(), found.edges(), Durability::Buffered)?;
//! # Ok::<(), ustore::Error>(())
//! ```

use std::iter::FusedIterator;
use std::mem::offset_of;
use std::ptr;
use std::slice;

use crate::db::ffi::{
    ustore_arena_t, ustore_collection_id_t, ustore_completion_t, ustore_database_t, ustore_durability_t, ustore_enum_t,
    ustore_executor_t, ustore_key_t, ustore_outcome_t, ustore_size_t, ustore_transaction_t, ustore_view_t,
    ustore_watch_t,
};
use crate::db::{check, exported_slice, fail, grown, Arena, Collections, Durability, Key, Result, Status, View};

// region: Raw Declarations

/// One-to-one mirror of `include/ustore/graph.h`.
mod ffi {
    #![allow(non_camel_case_types)]

    use super::*;

    pub type ustore_vertex_degree_t = u32;

    #[repr(C)]
    pub struct ustore_graph_find_edges_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: *const ustore_collection_id_t,
        pub collections_stride: ustore_size_t,
        pub vertices: *const ustore_key_t,
        pub vertices_stride: ustore_size_t,
        pub roles: *const ustore_enum_t,
        pub roles_stride: ustore_size_t,
        pub degrees_per_vertex: *mut ustore_vertex_degree_t,
        pub degrees_per_vertex_stride: ustore_size_t,
        pub edges_per_vertex: *mut *mut ustore_key_t,
        pub arena: *mut ustore_arena_t,
    }

    #[repr(C)]
    pub struct ustore_graph_upsert_edges_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: *const ustore_collection_id_t,
        pub collections_stride: ustore_size_t,
        pub edges: *const ustore_key_t,
        pub edges_stride: ustore_size_t,
        pub sources: *const ustore_key_t,
        pub sources_stride: ustore_size_t,
        pub targets: *const ustore_key_t,
        pub targets_stride: ustore_size_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    /// The header declares the same members under a second name.
    pub type ustore_graph_remove_edges_t = ustore_graph_upsert_edges_t;

    #[repr(C)]
    pub struct ustore_graph_upsert_vertices_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: *const ustore_collection_id_t,
        pub collections_stride: ustore_size_t,
        pub vertices: *const ustore_key_t,
        pub vertices_stride: ustore_size_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_graph_remove_vertices_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: *const ustore_collection_id_t,
        pub collections_stride: ustore_size_t,
        pub vertices: *const ustore_key_t,
        pub vertices_stride: ustore_size_t,
        pub roles: *const ustore_enum_t,
        pub roles_stride: ustore_size_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    extern "C" {
        pub static ustore_default_edge_id_k: ustore_key_t;
        pub static ustore_vertex_degree_missing_k: ustore_vertex_degree_t;

        pub fn ustore_graph_find_edges(arguments: *mut ustore_graph_find_edges_t);
        pub fn ustore_graph_upsert_edges(arguments: *mut ustore_graph_upsert_edges_t);
        pub fn ustore_graph_remove_edges(arguments: *mut ustore_graph_remove_edges_t);
        pub fn ustore_graph_upsert_vertices(arguments: *mut ustore_graph_upsert_vertices_t);
        pub fn ustore_graph_remove_vertices(arguments: *mut ustore_graph_remove_vertices_t);
    }
}

// endregion: Raw Declarations

// region: Vocabulary

/// The number of edges a vertex connects to in the roles a lookup asked for.
pub type Degree = ffi::ustore_vertex_degree_t;

/// The degree a lookup reports for a vertex the graph does not hold.
pub fn degree_missing() -> Degree {
    // SAFETY: the library defines the constant once and never writes it.
    unsafe { ffi::ustore_vertex_degree_missing_k }
}

/// The identifier an edge carries when it was given none.
pub fn default_edge_id() -> Key {
    // SAFETY: the library defines the constant once and never writes it.
    unsafe { ffi::ustore_default_edge_id_k }
}

/// The role a vertex plays in the edges a call addresses.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Role {
    /// The vertex the edges leave.
    Source = 1,
    /// The vertex the edges enter.
    Target = 2,
    /// Either end, which is the only role an undirected graph uses.
    #[default]
    Any = 3,
}

/// The roles of a batch of vertices: one for all of them, or one each.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum Roles<'roles> {
    /// Every vertex plays the same role.
    Every(Role),
    /// The vertex at each position plays the role at the same position.
    Each(&'roles [Role]),
}

impl Roles<'_> {
    /// The strided run of roles for `count` vertices, refusing a run of another length.
    fn strided(&self, count: usize) -> Result<(*const ustore_enum_t, ustore_size_t)> {
        match self {
            Roles::Every(role) => Ok(((role as *const Role).cast(), 0)),
            Roles::Each(roles) if roles.len() == count => {
                Ok((roles.as_ptr().cast(), size_of::<Role>() as ustore_size_t))
            }
            Roles::Each(_) => Err(fail(Status::InvalidArgument, "the batch holds a role per vertex")),
        }
    }
}

/// An edge from its source to its target, laid out as the three keys every edge takes on a tape.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug, Hash)]
pub struct Edge {
    /// The vertex the edge leaves.
    pub source: Key,
    /// The vertex the edge enters.
    pub target: Key,
    /// The identifier telling apart several edges between the same vertices.
    pub id: Key,
}

impl Edge {
    /// An edge carrying the default identifier.
    pub fn new(source: Key, target: Key) -> Self {
        Edge {
            source,
            target,
            id: default_edge_id(),
        }
    }

    /// An edge carrying its own identifier.
    pub fn with_id(source: Key, target: Key, id: Key) -> Self {
        Edge { source, target, id }
    }

    /// The end of the edge opposite `vertex`, or `None` when `vertex` is neither end.
    pub fn neighbor_of(&self, vertex: Key) -> Option<Key> {
        match vertex {
            _ if vertex == self.source => Some(self.target),
            _ if vertex == self.target => Some(self.source),
            _ => None,
        }
    }
}

// endregion: Vocabulary

// region: Outputs

/// The degree and the edges of every vertex one lookup addressed, in the order they were asked for.
///
/// The edges of one vertex run consecutively: sorted by neighbor within one role, with the outgoing edges
/// ahead of the incoming ones when both roles were asked for.
#[derive(Clone, Copy, Debug)]
pub struct Neighborhoods<'arena> {
    degrees: &'arena [Degree],
    edges: &'arena [Edge],
}

impl<'arena> Neighborhoods<'arena> {
    /// The degree of every vertex, with [`degree_missing`] for a vertex the graph does not hold.
    pub fn degrees(&self) -> &'arena [Degree] {
        self.degrees
    }

    /// The edges of every vertex, one run after another.
    pub fn edges(&self) -> &'arena [Edge] {
        self.edges
    }

    /// The edges of every vertex in order, or `None` for a vertex the graph does not hold.
    pub fn iter(&self) -> EdgeRuns<'arena> {
        EdgeRuns {
            degrees: self.degrees.iter(),
            edges: self.edges,
        }
    }
}

impl<'arena> IntoIterator for Neighborhoods<'arena> {
    type Item = Option<&'arena [Edge]>;
    type IntoIter = EdgeRuns<'arena>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'arena> IntoIterator for &Neighborhoods<'arena> {
    type Item = Option<&'arena [Edge]>;
    type IntoIter = EdgeRuns<'arena>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

/// The edges of every vertex of one lookup in order, or `None` for a vertex the graph does not hold.
#[derive(Clone, Debug)]
pub struct EdgeRuns<'arena> {
    degrees: slice::Iter<'arena, Degree>,
    edges: &'arena [Edge],
}

impl<'arena> Iterator for EdgeRuns<'arena> {
    type Item = Option<&'arena [Edge]>;

    fn next(&mut self) -> Option<Self::Item> {
        let degree = *self.degrees.next()?;
        if degree == degree_missing() {
            return Some(None);
        }
        let Some((run, rest)) = self.edges.split_at_checked(degree as usize) else {
            return Some(None);
        };
        self.edges = rest;
        Some(Some(run))
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        self.degrees.size_hint()
    }
}

impl ExactSizeIterator for EdgeRuns<'_> {}

impl FusedIterator for EdgeRuns<'_> {}

// endregion: Outputs

// region: Verbs

impl View<'_> {
    /// Inserts edges between existing or new vertices, keeping an edge the graph already holds.
    ///
    /// A transaction flushes at its commit, so [`Durability::Flushed`] is refused inside one.
    pub fn upsert_edges<'collections>(
        &self,
        collections: impl Into<Collections<'collections>>,
        edges: &[Edge],
        durability: Durability,
    ) -> Result<()> {
        let collections = collections.into();
        let mut arguments = self.edges_arguments(&collections, edges, durability)?;
        let mut outcome = ustore_outcome_t::default();
        arguments.outcome = &mut outcome;
        // SAFETY: the edges and the collections outlive the call, and the keys of every edge sit one
        // `Edge` apart, which is what `#[repr(C)]` on `Edge` promises.
        unsafe { ffi::ustore_graph_upsert_edges(&mut arguments) };
        check(outcome)
    }

    /// Removes edges by source, target and identifier, whether or not the graph holds them.
    pub fn remove_edges<'collections>(
        &self,
        collections: impl Into<Collections<'collections>>,
        edges: &[Edge],
        durability: Durability,
    ) -> Result<()> {
        let collections = collections.into();
        let mut arguments = self.edges_arguments(&collections, edges, durability)?;
        let mut outcome = ustore_outcome_t::default();
        arguments.outcome = &mut outcome;
        // SAFETY: the edges and the collections outlive the call, and the keys of every edge sit one
        // `Edge` apart, which is what `#[repr(C)]` on `Edge` promises.
        unsafe { ffi::ustore_graph_remove_edges(&mut arguments) };
        check(outcome)
    }

    /// Inserts vertices without edges, keeping every vertex the graph already holds as it is.
    pub fn upsert_vertices<'collections>(
        &self,
        collections: impl Into<Collections<'collections>>,
        vertices: &[Key],
        durability: Durability,
    ) -> Result<()> {
        let transaction = self.transaction()?;
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(vertices.len())?;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_graph_upsert_vertices_t {
            database: self.database(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            transaction,
            count: vertices.len() as ustore_size_t,
            collections,
            collections_stride,
            vertices: vertices.as_ptr(),
            vertices_stride: size_of::<Key>() as ustore_size_t,
            durability,
            watch: self.watch(),
        };
        // SAFETY: the vertices and the collections outlive the call.
        unsafe { ffi::ustore_graph_upsert_vertices(&mut arguments) };
        check(outcome)
    }

    /// Removes vertices together with every edge they play `roles` in, which only a joining graph needs.
    pub fn remove_vertices<'collections>(
        &self,
        collections: impl Into<Collections<'collections>>,
        vertices: &[Key],
        roles: Roles<'_>,
        durability: Durability,
    ) -> Result<()> {
        let transaction = self.transaction()?;
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(vertices.len())?;
        let (roles, roles_stride) = roles.strided(vertices.len())?;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_graph_remove_vertices_t {
            database: self.database(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            transaction,
            count: vertices.len() as ustore_size_t,
            collections,
            collections_stride,
            vertices: vertices.as_ptr(),
            vertices_stride: size_of::<Key>() as ustore_size_t,
            roles,
            roles_stride,
            durability,
            watch: self.watch(),
        };
        // SAFETY: the vertices, the collections and the roles outlive the call, and `Role` is an `i32`
        // as the ABI enum is.
        unsafe { ffi::ustore_graph_remove_vertices(&mut arguments) };
        check(outcome)
    }

    /// The degree of every vertex in `roles`, without exporting a single edge.
    pub fn degrees<'arena, 'collections>(
        &self,
        arena: &'arena mut Arena,
        collections: impl Into<Collections<'collections>>,
        vertices: &[Key],
        roles: Roles<'_>,
    ) -> Result<&'arena [Degree]> {
        self.find(arena, collections.into(), vertices, roles, ptr::null_mut())
    }

    /// The degree and the edges of every vertex in `roles`.
    pub fn find_edges<'arena, 'collections>(
        &self,
        arena: &'arena mut Arena,
        collections: impl Into<Collections<'collections>>,
        vertices: &[Key],
        roles: Roles<'_>,
    ) -> Result<Neighborhoods<'arena>> {
        let mut tape: *mut ustore_key_t = ptr::null_mut();
        let degrees = self.find(arena, collections.into(), vertices, roles, &mut tape)?;
        let missing = degree_missing();
        let edges_count = degrees
            .iter()
            .filter(|&&degree| degree != missing)
            .map(|&degree| degree as usize)
            .sum();
        // SAFETY: the tape holds three keys per edge of every present vertex, allocated at the alignment of a
        // key, and `Edge` is those three keys in tape order.
        let edges = unsafe { exported_slice(tape.cast::<Edge>(), edges_count) };
        Ok(Neighborhoods { degrees, edges })
    }

    /// Runs one lookup into the arena's degrees, exporting the edges too when `tape` is not null.
    fn find<'arena>(
        &self,
        arena: &'arena mut Arena,
        collections: Collections<'_>,
        vertices: &[Key],
        roles: Roles<'_>,
        tape: *mut *mut ustore_key_t,
    ) -> Result<&'arena [Degree]> {
        let (collections, collections_stride) = collections.strided(vertices.len())?;
        let (roles, roles_stride) = roles.strided(vertices.len())?;
        let Arena { handle, degrees, .. } = arena;
        let degrees = grown(degrees, vertices.len())?;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_graph_find_edges_t {
            database: self.database(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            view: self.view(),
            count: vertices.len() as ustore_size_t,
            collections,
            collections_stride,
            vertices: vertices.as_ptr(),
            vertices_stride: size_of::<Key>() as ustore_size_t,
            roles,
            roles_stride,
            degrees_per_vertex: degrees.as_mut_ptr(),
            degrees_per_vertex_stride: size_of::<Degree>() as ustore_size_t,
            edges_per_vertex: tape,
            arena: handle,
        };
        // SAFETY: the vertices, the collections, the roles and both outputs outlive the call, and `Role` is
        // an `i32` as the ABI enum is.
        unsafe { ffi::ustore_graph_find_edges(&mut arguments) };
        check(outcome)?;
        Ok(degrees)
    }

    /// The arguments an edge upsert or removal takes, borrowing the collection and the edges from the caller,
    /// whose outcome slot the caller points at its own outcome.
    fn edges_arguments(
        &self,
        collections: &Collections<'_>,
        edges: &[Edge],
        durability: Durability,
    ) -> Result<ffi::ustore_graph_upsert_edges_t> {
        let transaction = self.transaction()?;
        let (collections, collections_stride) = collections.strided(edges.len())?;
        let stride = size_of::<Edge>() as ustore_size_t;
        let first = edges.as_ptr();
        Ok(ffi::ustore_graph_upsert_edges_t {
            database: self.database(),
            executor: ptr::null_mut(),
            outcome: ptr::null_mut(),
            completion: ustore_completion_t::Blocking,
            transaction,
            count: edges.len() as ustore_size_t,
            collections,
            collections_stride,
            edges: first.wrapping_byte_add(offset_of!(Edge, id)).cast::<ustore_key_t>(),
            edges_stride: stride,
            sources: first.wrapping_byte_add(offset_of!(Edge, source)).cast::<ustore_key_t>(),
            sources_stride: stride,
            targets: first.wrapping_byte_add(offset_of!(Edge, target)).cast::<ustore_key_t>(),
            targets_stride: stride,
            durability,
            watch: self.watch(),
        })
    }
}

// endregion: Verbs

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::blobs::Entry;
    use crate::db::{CollectionID, Collections, Conflicts, Database, Modality, RAMOptions};
    use crate::testing::{key, keys};

    /// A directed graph of one to two, one to three and two to three, in a collection of its own.
    fn populated() -> (Database, Arena, CollectionID) {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let graph = database.create_collection("graph", Modality::Graph).unwrap();
        let edges = [
            Edge::new(key(1), key(2)),
            Edge::new(key(1), key(3)),
            Edge::new(key(2), key(3)),
        ];
        database
            .head()
            .upsert_edges(graph, &edges, Durability::Buffered)
            .unwrap();
        (database, Arena::new(), graph)
    }

    #[test]
    fn lookups_find_the_edges_of_every_role() {
        let (database, mut arena, graph) = populated();
        let outgoing = database
            .head()
            .find_edges(&mut arena, graph, &keys([1]), Roles::Every(Role::Source))
            .unwrap();
        assert_eq!(outgoing.degrees(), &[2]);
        assert_eq!(
            outgoing.edges(),
            &[Edge::new(key(1), key(2)), Edge::new(key(1), key(3))]
        );

        let incoming = database
            .head()
            .find_edges(&mut arena, graph, &keys([3]), Roles::Every(Role::Target))
            .unwrap();
        assert_eq!(
            incoming.edges(),
            &[Edge::new(key(1), key(3)), Edge::new(key(2), key(3))]
        );

        // Both roles list the outgoing edges ahead of the incoming ones.
        let both = database
            .head()
            .find_edges(&mut arena, graph, &keys([2]), Roles::Every(Role::Any))
            .unwrap();
        assert_eq!(both.edges(), &[Edge::new(key(2), key(3)), Edge::new(key(1), key(2))]);
    }

    #[test]
    fn neighborhoods_split_the_tape_by_vertex() {
        let (database, mut arena, graph) = populated();
        let found = database
            .head()
            .find_edges(&mut arena, graph, &keys([3, 42, 1]), Roles::Every(Role::Any))
            .unwrap();
        assert_eq!(found.degrees(), &[2, degree_missing(), 2]);
        assert_eq!(found.edges().len(), 4);
        let runs: Vec<Option<&[Edge]>> = found.iter().collect();
        assert_eq!(
            runs[0],
            Some([Edge::new(key(1), key(3)), Edge::new(key(2), key(3))].as_slice())
        );
        assert_eq!(runs[1], None, "a vertex the graph does not hold is missing");
        assert_eq!(
            runs[2],
            Some([Edge::new(key(1), key(2)), Edge::new(key(1), key(3))].as_slice())
        );

        let neighbors: Vec<Key> = runs[2]
            .unwrap()
            .iter()
            .filter_map(|edge| edge.neighbor_of(key(1)))
            .collect();
        assert_eq!(neighbors, keys([2, 3]));
    }

    #[test]
    fn degrees_come_without_edges() {
        let (database, mut arena, graph) = populated();
        let degrees = database
            .head()
            .degrees(&mut arena, graph, &keys([1, 2, 3, 4]), Roles::Every(Role::Source))
            .unwrap();
        assert_eq!(degrees, &[2, 1, 0, degree_missing()]);
    }

    #[test]
    fn roles_differ_per_vertex_and_match_the_batch() {
        let (database, mut arena, graph) = populated();
        let roles = [Role::Source, Role::Target];
        let degrees = database
            .head()
            .degrees(&mut arena, graph, &keys([1, 1]), Roles::Each(&roles))
            .unwrap();
        assert_eq!(degrees, &[2, 0]);

        let failure = database
            .head()
            .find_edges(&mut arena, graph, &keys([1, 2, 3]), Roles::Each(&roles))
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
        let failure = database
            .head()
            .remove_vertices(graph, &keys([1]), Roles::Each(&roles), Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn identifiers_tell_parallel_edges_apart() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let graph = database.create_collection("graph", Modality::Graph).unwrap();
        let edges = [
            Edge::with_id(key(1), key(2), key(10)),
            Edge::with_id(key(1), key(2), key(20)),
            Edge::with_id(key(1), key(2), key(10)),
        ];
        database
            .head()
            .upsert_edges(graph, &edges, Durability::Buffered)
            .unwrap();
        let degrees = database
            .head()
            .degrees(&mut arena, graph, &keys([1]), Roles::Every(Role::Source))
            .unwrap();
        assert_eq!(degrees, &[2], "an edge upserted twice is stored once");

        database
            .head()
            .remove_edges(graph, &[Edge::with_id(key(1), key(2), key(10))], Durability::Buffered)
            .unwrap();
        let found = database
            .head()
            .find_edges(&mut arena, graph, &keys([2]), Roles::Every(Role::Target))
            .unwrap();
        assert_eq!(found.edges(), &[Edge::with_id(key(1), key(2), key(20))]);

        // Removing an edge the graph never held changes nothing.
        database
            .head()
            .remove_edges(graph, &[Edge::new(key(5), key(6))], Durability::Buffered)
            .unwrap();
    }

    #[test]
    fn vertices_are_upserted_bare_and_removed_with_their_edges() {
        let (database, mut arena, graph) = populated();
        database
            .head()
            .upsert_vertices(graph, &keys([1, 7]), Durability::Buffered)
            .unwrap();
        let degrees = database
            .head()
            .degrees(&mut arena, graph, &keys([1, 7]), Roles::Every(Role::Any))
            .unwrap();
        assert_eq!(degrees, &[2, 0], "an upsert keeps the edges a vertex already has");

        database
            .head()
            .remove_vertices(graph, &keys([3]), Roles::Every(Role::Any), Durability::Buffered)
            .unwrap();
        let degrees = database
            .head()
            .degrees(&mut arena, graph, &keys([1, 2, 3]), Roles::Every(Role::Any))
            .unwrap();
        assert_eq!(degrees, &[1, 1, degree_missing()]);
    }

    #[test]
    fn a_vertex_removed_as_a_source_leaves_its_targets_consistent() {
        let (database, mut arena, graph) = populated();
        database
            .head()
            .remove_vertices(graph, &keys([1]), Roles::Every(Role::Source), Durability::Buffered)
            .unwrap();
        let found = database
            .head()
            .find_edges(&mut arena, graph, &keys([3]), Roles::Every(Role::Target))
            .unwrap();
        assert_eq!(found.edges(), &[Edge::new(key(2), key(3))]);
    }

    #[test]
    fn transactions_hold_their_edges_until_commit() {
        let (database, mut arena, graph) = populated();
        let mut transaction = database.transaction().unwrap();
        let edges = [Edge::new(key(3), key(1))];
        transaction
            .view(Conflicts::Watched)
            .upsert_edges(graph, &edges, Durability::Buffered)
            .unwrap();
        let flushed = transaction
            .view(Conflicts::Watched)
            .upsert_edges(graph, &edges, Durability::Flushed)
            .unwrap_err();
        assert_eq!(flushed.status, Status::InvalidArgument);

        let inside = transaction
            .view(Conflicts::Watched)
            .degrees(&mut arena, graph, &keys([3]), Roles::Every(Role::Source))
            .unwrap();
        assert_eq!(inside, &[1]);
        let outside = database
            .head()
            .degrees(&mut arena, graph, &keys([3]), Roles::Every(Role::Source))
            .unwrap();
        assert_eq!(outside, &[0]);

        transaction.commit(Durability::Buffered).unwrap();
        let committed = database
            .head()
            .degrees(&mut arena, graph, &keys([3]), Roles::Every(Role::Source))
            .unwrap();
        assert_eq!(committed, &[1]);
    }

    #[test]
    fn snapshots_keep_the_graph_they_were_taken_at() {
        let (database, mut arena, graph) = populated();
        let snapshot = database.snapshot().unwrap();
        database
            .head()
            .remove_edges(graph, &[Edge::new(key(1), key(2))], Durability::Buffered)
            .unwrap();
        let pinned = snapshot
            .view()
            .degrees(&mut arena, graph, &keys([1]), Roles::Every(Role::Source))
            .unwrap();
        assert_eq!(pinned, &[2]);
    }

    #[test]
    fn a_collection_of_values_refuses_the_graph_verbs() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let blobs = [
            Entry::new(key(1), b"abc").unwrap(),
            Entry::new(key(2), br#"{"name":"Ada Lovelace","born":1815}"#).unwrap(),
        ];
        database
            .head()
            .write(CollectionID::main(), &blobs, Durability::Buffered)
            .unwrap();
        for vertex in keys([1, 2]) {
            let failure = database
                .head()
                .find_edges(&mut arena, CollectionID::main(), &[vertex], Roles::Every(Role::Any))
                .unwrap_err();
            assert_eq!(failure.status, Status::InvalidArgument);
            let failure = database
                .head()
                .upsert_edges(CollectionID::main(), &[Edge::new(vertex, key(3))], Durability::Buffered)
                .unwrap_err();
            assert_eq!(failure.status, Status::InvalidArgument);
        }
    }

    #[test]
    fn one_batch_spans_several_graph_collections() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let follows = database.create_collection("follows", Modality::Graph).unwrap();
        let blocks = database.create_collection("blocks", Modality::Graph).unwrap();
        let spanned = [follows, blocks];

        let edges = [Edge::new(key(1), key(2)), Edge::new(key(1), key(3))];
        database
            .head()
            .upsert_edges(spanned.as_slice(), &edges, Durability::Buffered)
            .unwrap();

        let degrees = database
            .head()
            .degrees(
                &mut arena,
                Collections::Each(&spanned),
                &keys([1, 1]),
                Roles::Every(Role::Source),
            )
            .unwrap();
        assert_eq!(degrees, &[1, 1], "each edge landed in the collection of its own task");
        let inside = database
            .head()
            .find_edges(&mut arena, blocks, &keys([1]), Roles::Every(Role::Source))
            .unwrap();
        assert_eq!(inside.edges(), &[Edge::new(key(1), key(3))]);

        let failure = database
            .head()
            .degrees(
                &mut arena,
                Collections::Each(&spanned),
                &keys([1]),
                Roles::Every(Role::Any),
            )
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn empty_batches_reach_no_engine() {
        let (database, mut arena, graph) = populated();
        let head = database.head();
        head.upsert_edges(graph, &[], Durability::Buffered).unwrap();
        head.remove_edges(graph, &[], Durability::Buffered).unwrap();
        head.upsert_vertices(graph, &[], Durability::Buffered).unwrap();
        head.remove_vertices(graph, &[], Roles::Each(&[]), Durability::Buffered)
            .unwrap();
        assert!(head
            .degrees(&mut arena, graph, &[], Roles::Every(Role::Any))
            .unwrap()
            .is_empty());
        let found = head
            .find_edges(&mut arena, graph, &[], Roles::Every(Role::Any))
            .unwrap();
        assert!(found.degrees().is_empty() && found.edges().is_empty());
        assert_eq!(found.iter().count(), 0);
    }

    #[test]
    fn edges_cross_as_three_keys_in_tape_order() {
        assert_eq!(size_of::<Edge>(), 3 * size_of::<Key>());
        assert_eq!(offset_of!(Edge, source), 0);
        assert_eq!(offset_of!(Edge, target), size_of::<Key>());
        assert_eq!(offset_of!(Edge, id), 2 * size_of::<Key>());
        assert_eq!(size_of::<Role>(), size_of::<ustore_enum_t>());
        assert_eq!(Edge::new(key(1), key(2)).id, default_edge_id());
        assert_eq!(Edge::new(key(1), key(2)).neighbor_of(key(2)), Some(key(1)));
        assert_eq!(Edge::new(key(1), key(2)).neighbor_of(key(3)), None);
        assert_eq!(Edge::new(key(4), key(4)).neighbor_of(key(4)), Some(key(4)));
    }
}

// endregion: Tests
