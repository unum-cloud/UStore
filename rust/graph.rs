//! Graphs stored as an inverted index from every vertex to its edges - the Rust twin of `c/ustore/graph.h`.
//!
//! An [`Edge`] is laid out as the three keys the ABI puts on its tapes, so a slice of edges crosses as the
//! source, target and identifier columns of a write, and the tape a lookup exports reads back as a slice of
//! edges without a copy.
//!
//! ```no_run
//! use ustore::{Arena, RAMOptions, Database, Durability, Edge, Key, Modality, Role, Roles};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new()?;
//! let follows = database.collection_create("follows", Modality::Graph)?;
//!
//! let [ada, grace, kent] = [1u64, 2, 3].map(Key::from);
//! let edges = [Edge::new(ada, grace), Edge::new(ada, kent), Edge::new(grace, kent)];
//! database.head().graph_write_edges(follows, &edges, Durability::Buffered)?;
//!
//! let found = database.head().graph_read_edges(follows, [ada, kent], Roles::Every(Role::Any), &mut arena)?;
//! assert_eq!(found.degrees(), &[2, 2]);
//! assert_eq!(found.edges()[0], Edge::new(ada, grace));
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The edges a lookup found live in the arena, so another lookup through that arena does not compile while
//! they are still read:
//!
//! ```compile_fail,E0499
//! use ustore::{Arena, CollectionID, RAMOptions, Database, Durability, Key, Role, Roles};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new()?;
//! let vertices = [Key::from(1u64)];
//! let main = CollectionID::main();
//! let found = database.head().graph_read_edges(main, &vertices, Roles::Every(Role::Any), &mut arena)?;
//! let degrees = database.head().graph_degrees(main, &vertices, Roles::Every(Role::Any), &mut arena)?;
//! database.head().graph_remove_edges(main, found.edges(), Durability::Buffered)?;
//! # Ok::<(), ustore::Error>(())
//! ```

use std::ffi::c_char;
use std::fmt;
use std::iter::{FusedIterator, Zip};
use std::mem::offset_of;
use std::ptr;
use std::slice;
use std::str::FromStr;

use crate::db::ffi::{
    ustore_arena_t, ustore_bytes_view_t, ustore_collections_view_t, ustore_completion_t, ustore_database_t,
    ustore_durability_t, ustore_enum_t, ustore_executor_t, ustore_key_t, ustore_keys_view_t, ustore_length_t,
    ustore_outcome_t, ustore_size_t, ustore_strided_view_t, ustore_transaction_t, ustore_view_t, ustore_watch_t,
};
use crate::db::sealed::{Accepts, Calling, Writable};
use crate::db::{
    exported_slice, fail, key_unknown, read_spelling, spelling, Address, Arena, Collections, Durability, Error, Key,
    Result, Status, View,
};

// region: Raw Declarations

/// One-to-one mirror of `c/ustore/graph.h`.
mod ffi {
    #![allow(non_camel_case_types)]

    use super::*;

    pub type ustore_vertex_degree_t = u32;
    pub type ustore_vertex_roles_view_t = ustore_strided_view_t<ustore_enum_t>;

    #[repr(C)]
    pub struct ustore_graph_read_edges_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub vertices: ustore_keys_view_t,
        pub roles: ustore_vertex_roles_view_t,
        pub degrees_per_vertex: *mut *const ustore_vertex_degree_t,
        pub edges_offsets: *mut *const ustore_length_t,
        pub edges_per_vertex: *mut *const ustore_key_t,
        pub arena: ustore_arena_t,
    }

    #[repr(C)]
    pub struct ustore_graph_write_edges_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub edges: ustore_keys_view_t,
        pub sources: ustore_keys_view_t,
        pub targets: ustore_keys_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    /// The header declares the same members under a second name.
    pub type ustore_graph_remove_edges_t = ustore_graph_write_edges_t;

    #[repr(C)]
    pub struct ustore_graph_write_vertices_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub vertices: ustore_keys_view_t,
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
        pub collections: ustore_collections_view_t,
        pub vertices: ustore_keys_view_t,
        pub roles: ustore_vertex_roles_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    extern "C" {
        pub fn ustore_graph_read_edges(arguments: *mut ustore_graph_read_edges_t);
        pub fn ustore_graph_write_edges(arguments: *mut ustore_graph_write_edges_t);
        pub fn ustore_graph_remove_edges(arguments: *mut ustore_graph_remove_edges_t);
        pub fn ustore_graph_write_vertices(arguments: *mut ustore_graph_write_vertices_t);
        pub fn ustore_graph_remove_vertices(arguments: *mut ustore_graph_remove_vertices_t);
        pub fn ustore_vertex_role_name(vertex_role: ustore_enum_t) -> *const c_char;
        pub fn ustore_vertex_role_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
    }
}

// endregion: Raw Declarations

// region: Vocabulary

/// The number of edges a vertex connects to in the roles a lookup asked for.
pub type Degree = ffi::ustore_vertex_degree_t;

/// The degree a lookup reports for a vertex the graph does not hold.
pub const fn degree_missing() -> Degree {
    Degree::MAX
}

/// The identifier an edge carries when it was given none, every bit of it set like [`key_unknown`].
pub const fn default_edge_id() -> Key {
    key_unknown()
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

impl Role {
    fn from_raw(raw: ustore_enum_t) -> Option<Self> {
        match raw {
            1 => Some(Role::Source),
            2 => Some(Role::Target),
            3 => Some(Role::Any),
            _ => None,
        }
    }
}

impl fmt::Display for Role {
    /// Calls `ustore_vertex_role_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_vertex_role_name, *self as ustore_enum_t))
    }
}

impl FromStr for Role {
    type Err = Error;

    /// Calls `ustore_vertex_role_from_name`, refusing "unknown", which no call takes.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_vertex_role_from_name,
            ffi::ustore_vertex_role_name,
            Role::from_raw,
        )
    }
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
    /// The strided run of roles for `count` vertices, refusing a run of another length; `Role` is an `i32`, as the
    /// ABI enum is.
    fn view(&self, count: usize) -> Result<ffi::ustore_vertex_roles_view_t> {
        match self {
            Roles::Every(role) => Ok(ustore_strided_view_t {
                data: ptr::from_ref(role).cast(),
                stride: 0,
            }),
            Roles::Each(roles) if roles.len() == count => Ok(ustore_strided_view_t {
                data: roles.as_ptr().cast(),
                stride: size_of::<Role>() as ustore_size_t,
            }),
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
    offsets: &'arena [ustore_length_t],
    edges: &'arena [Edge],
}

impl<'arena> Neighborhoods<'arena> {
    /// The degrees, the offsets and the edges a lookup of `count` vertices exported.
    ///
    /// # Safety
    /// A successful lookup of `count` vertices exported a degree per vertex at `degrees`, `count` plus one offsets
    /// counted in edges at `offsets`, and three keys per edge at `tape`, allocated at the alignment of a key and alive
    /// for `'arena`.
    unsafe fn exported(
        count: usize,
        degrees: Address<Degree>,
        offsets: Address<ustore_length_t>,
        tape: Address<ustore_key_t>,
    ) -> Self {
        let offsets = exported_slice(offsets.0, count + 1);
        let edges_count = offsets.last().map_or(0, |&end| end as usize);
        // `Edge` is the three keys of an edge in tape order
        let edges = exported_slice(tape.0.cast::<Edge>(), edges_count);
        Neighborhoods {
            degrees: exported_slice(degrees.0, count),
            offsets,
            edges,
        }
    }

    /// The degree of every vertex, with [`degree_missing`] for a vertex the graph does not hold.
    pub fn degrees(&self) -> &'arena [Degree] {
        self.degrees
    }

    /// One more offset than vertices into [`Neighborhoods::edges`], counted in edges, bounding the run of every
    /// vertex, where a vertex the graph does not hold spans none.
    pub fn offsets(&self) -> &'arena [u64] {
        self.offsets
    }

    /// The edges of every vertex, one run after another.
    pub fn edges(&self) -> &'arena [Edge] {
        self.edges
    }

    /// The edges of every vertex in order, or `None` for a vertex the graph does not hold.
    pub fn iter(&self) -> EdgeRuns<'arena> {
        EdgeRuns {
            spans: self.degrees.iter().zip(self.offsets.windows(2)),
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
    spans: Zip<slice::Iter<'arena, Degree>, slice::Windows<'arena, ustore_length_t>>,
    edges: &'arena [Edge],
}

impl<'arena> Iterator for EdgeRuns<'arena> {
    type Item = Option<&'arena [Edge]>;

    fn next(&mut self) -> Option<Self::Item> {
        let (&degree, span) = self.spans.next()?;
        match degree == degree_missing() {
            true => Some(None),
            false => Some(self.edges.get(span[0] as usize..span[1] as usize)),
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        self.spans.size_hint()
    }
}

impl ExactSizeIterator for EdgeRuns<'_> {}

impl FusedIterator for EdgeRuns<'_> {}

// endregion: Outputs

// region: Verbs

impl<Parent: Writable, Completion: crate::db::sealed::Completion> View<'_, Parent, Completion> {
    /// Inserts edges between existing or new vertices, keeping an edge the graph already holds, and hands the edges
    /// back.
    ///
    /// A transaction flushes at its commit, so [`Durability::Flushed`] is refused inside one.
    ///
    /// Calls `ustore_graph_write_edges`.
    pub fn graph_write_edges<'collections, Edges: AsRef<[Edge]>>(
        &self,
        collections: impl Into<Collections<'collections>>,
        edges: Edges,
        durability: Durability,
    ) -> Completion::Answer<GraphWriteEdges<'collections, Edges>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Edges>,
    {
        let verb = GraphWriteEdges {
            collections: collections.into(),
            edges,
            durability,
        };
        Completion::answer(self, verb)
    }

    /// Removes edges by source, target and identifier, whether or not the graph holds them, and hands the edges back.
    ///
    /// Calls `ustore_graph_remove_edges`.
    pub fn graph_remove_edges<'collections, Edges: AsRef<[Edge]>>(
        &self,
        collections: impl Into<Collections<'collections>>,
        edges: Edges,
        durability: Durability,
    ) -> Completion::Answer<GraphRemoveEdges<'collections, Edges>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Edges>,
    {
        let verb = GraphRemoveEdges {
            collections: collections.into(),
            edges,
            durability,
        };
        Completion::answer(self, verb)
    }

    /// Inserts vertices without edges, keeping every vertex the graph already holds as it is, and hands the vertices
    /// back.
    ///
    /// Calls `ustore_graph_write_vertices`.
    pub fn graph_write_vertices<'collections, Keys: AsRef<[Key]>>(
        &self,
        collections: impl Into<Collections<'collections>>,
        vertices: Keys,
        durability: Durability,
    ) -> Completion::Answer<GraphWriteVertices<'collections, Keys>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Keys>,
    {
        let verb = GraphWriteVertices {
            collections: collections.into(),
            vertices,
            durability,
        };
        Completion::answer(self, verb)
    }

    /// Removes vertices together with every edge they play `roles` in, which only a joining graph needs, and hands
    /// the vertices back.
    ///
    /// Calls `ustore_graph_remove_vertices`.
    pub fn graph_remove_vertices<'inputs, Keys: AsRef<[Key]>>(
        &self,
        collections: impl Into<Collections<'inputs>>,
        vertices: Keys,
        roles: Roles<'inputs>,
        durability: Durability,
    ) -> Completion::Answer<GraphRemoveVertices<'inputs, Keys>>
    where
        Completion: Accepts<Collections<'inputs>> + Accepts<Keys> + Accepts<Roles<'inputs>>,
    {
        let verb = GraphRemoveVertices {
            collections: collections.into(),
            vertices,
            roles,
            durability,
        };
        Completion::answer(self, verb)
    }
}

impl<Parent, Completion: crate::db::sealed::Completion> View<'_, Parent, Completion> {
    /// The degree of every vertex in `roles`, exported into the arena without a single edge.
    ///
    /// Calls `ustore_graph_read_edges`.
    pub fn graph_degrees<'arena, 'inputs, Keys: AsRef<[Key]>>(
        &self,
        collections: impl Into<Collections<'inputs>>,
        vertices: Keys,
        roles: Roles<'inputs>,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<GraphDegrees<'arena, 'inputs, Keys>>
    where
        Completion: Accepts<Collections<'inputs>> + Accepts<Keys> + Accepts<Roles<'inputs>>,
    {
        let verb = GraphDegrees {
            collections: collections.into(),
            vertices,
            roles,
            arena,
            degrees: Address::default(),
        };
        Completion::answer(self, verb)
    }

    /// The degree and the edges of every vertex in `roles`, exported into the arena as [`Neighborhoods`].
    ///
    /// Calls `ustore_graph_read_edges`.
    pub fn graph_read_edges<'arena, 'inputs, Keys: AsRef<[Key]>>(
        &self,
        collections: impl Into<Collections<'inputs>>,
        vertices: Keys,
        roles: Roles<'inputs>,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<GraphReadEdges<'arena, 'inputs, Keys>>
    where
        Completion: Accepts<Collections<'inputs>> + Accepts<Keys> + Accepts<Roles<'inputs>>,
    {
        let verb = GraphReadEdges {
            collections: collections.into(),
            vertices,
            roles,
            arena,
            degrees: Address::default(),
            offsets: Address::default(),
            tape: Address::default(),
        };
        Completion::answer(self, verb)
    }
}

/// The arguments an edge upsert or removal takes over `edges`, whose keys sit one `Edge` apart, which is what
/// `#[repr(C)]` on `Edge` promises.
fn edges_arguments<Parent, Completion>(
    view: &View<'_, Parent, Completion>,
    outcome: *mut ustore_outcome_t,
    completion: ustore_completion_t,
    collections: &Collections<'_>,
    edges: &[Edge],
    durability: Durability,
) -> Result<Option<ffi::ustore_graph_write_edges_t>> {
    let stride = size_of::<Edge>() as ustore_size_t;
    let first = edges.as_ptr();
    let column = |offset: usize| ustore_strided_view_t {
        data: first.wrapping_byte_add(offset).cast::<ustore_key_t>(),
        stride,
    };
    Ok(Some(ffi::ustore_graph_write_edges_t {
        database: view.database(),
        executor: view.executor(),
        outcome,
        completion,
        transaction: view.transaction(),
        count: edges.len() as ustore_size_t,
        collections: collections.view(edges.len())?,
        edges: column(offset_of!(Edge, id)),
        sources: column(offset_of!(Edge, source)),
        targets: column(offset_of!(Edge, target)),
        durability,
        watch: view.watch(),
    }))
}

/// An upsert of edges; see [`View::graph_write_edges`].
#[derive(Debug)]
pub struct GraphWriteEdges<'collections, Edges> {
    collections: Collections<'collections>,
    edges: Edges,
    durability: Durability,
}

impl<Edges: AsRef<[Edge]>> Calling for GraphWriteEdges<'_, Edges> {
    type Arguments = ffi::ustore_graph_write_edges_t;
    type Output = Edges;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let edges = self.edges.as_ref();
        edges_arguments(view, outcome, completion, &self.collections, edges, self.durability)
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_graph_write_edges(arguments)
    }

    fn finish(self) -> Result<Edges> {
        Ok(self.edges)
    }
}

/// A removal of edges; see [`View::graph_remove_edges`].
#[derive(Debug)]
pub struct GraphRemoveEdges<'collections, Edges> {
    collections: Collections<'collections>,
    edges: Edges,
    durability: Durability,
}

impl<Edges: AsRef<[Edge]>> Calling for GraphRemoveEdges<'_, Edges> {
    type Arguments = ffi::ustore_graph_remove_edges_t;
    type Output = Edges;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let edges = self.edges.as_ref();
        edges_arguments(view, outcome, completion, &self.collections, edges, self.durability)
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_graph_remove_edges(arguments)
    }

    fn finish(self) -> Result<Edges> {
        Ok(self.edges)
    }
}

/// An upsert of bare vertices; see [`View::graph_write_vertices`].
#[derive(Debug)]
pub struct GraphWriteVertices<'collections, Keys> {
    collections: Collections<'collections>,
    vertices: Keys,
    durability: Durability,
}

impl<Keys: AsRef<[Key]>> Calling for GraphWriteVertices<'_, Keys> {
    type Arguments = ffi::ustore_graph_write_vertices_t;
    type Output = Keys;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let vertices = self.vertices.as_ref();
        Ok(Some(ffi::ustore_graph_write_vertices_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: vertices.len() as ustore_size_t,
            collections: self.collections.view(vertices.len())?,
            vertices: ustore_strided_view_t::packed(vertices),
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_graph_write_vertices(arguments)
    }

    fn finish(self) -> Result<Keys> {
        Ok(self.vertices)
    }
}

/// A removal of vertices with their edges; see [`View::graph_remove_vertices`].
#[derive(Debug)]
pub struct GraphRemoveVertices<'inputs, Keys> {
    collections: Collections<'inputs>,
    vertices: Keys,
    roles: Roles<'inputs>,
    durability: Durability,
}

impl<Keys: AsRef<[Key]>> Calling for GraphRemoveVertices<'_, Keys> {
    type Arguments = ffi::ustore_graph_remove_vertices_t;
    type Output = Keys;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let vertices = self.vertices.as_ref();
        Ok(Some(ffi::ustore_graph_remove_vertices_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: vertices.len() as ustore_size_t,
            collections: self.collections.view(vertices.len())?,
            vertices: ustore_strided_view_t::packed(vertices),
            roles: self.roles.view(vertices.len())?,
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_graph_remove_vertices(arguments)
    }

    fn finish(self) -> Result<Keys> {
        Ok(self.vertices)
    }
}

/// The arguments of one lookup into `arena`, exporting the degrees at `degrees`, and the edges too, with their offsets,
/// when `offsets` and `tape` are not null.
#[allow(clippy::too_many_arguments)]
fn read_edges_arguments<Parent, Completion>(
    view: &View<'_, Parent, Completion>,
    outcome: *mut ustore_outcome_t,
    completion: ustore_completion_t,
    collections: &Collections<'_>,
    vertices: &[Key],
    roles: &Roles<'_>,
    arena: ustore_arena_t,
    degrees: *mut *const Degree,
    offsets: *mut *const ustore_length_t,
    tape: *mut *const ustore_key_t,
) -> Result<Option<ffi::ustore_graph_read_edges_t>> {
    Ok(Some(ffi::ustore_graph_read_edges_t {
        database: view.database(),
        executor: view.executor(),
        outcome,
        completion,
        view: view.view(),
        count: vertices.len() as ustore_size_t,
        collections: collections.view(vertices.len())?,
        vertices: ustore_strided_view_t::packed(vertices),
        roles: roles.view(vertices.len())?,
        degrees_per_vertex: degrees,
        edges_offsets: offsets,
        edges_per_vertex: tape,
        arena,
    }))
}

/// A lookup of the degrees of vertices; see [`View::graph_degrees`].
#[derive(Debug)]
pub struct GraphDegrees<'arena, 'inputs, Keys> {
    collections: Collections<'inputs>,
    vertices: Keys,
    roles: Roles<'inputs>,
    arena: &'arena mut Arena,
    degrees: Address<Degree>,
}

impl<'arena, Keys: AsRef<[Key]>> Calling for GraphDegrees<'arena, '_, Keys> {
    type Arguments = ffi::ustore_graph_read_edges_t;
    type Output = &'arena [Degree];

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        read_edges_arguments(
            view,
            outcome,
            completion,
            &self.collections,
            self.vertices.as_ref(),
            &self.roles,
            self.arena.handle,
            &mut self.degrees.0,
            ptr::null_mut(),
            ptr::null_mut(),
        )
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_graph_read_edges(arguments)
    }

    fn finish(self) -> Result<&'arena [Degree]> {
        // SAFETY: the lookup succeeded over every vertex, and the arena stays borrowed for `'arena`.
        Ok(unsafe { exported_slice(self.degrees.0, self.vertices.as_ref().len()) })
    }
}

/// A lookup of the degrees and the edges of vertices; see [`View::graph_read_edges`].
#[derive(Debug)]
pub struct GraphReadEdges<'arena, 'inputs, Keys> {
    collections: Collections<'inputs>,
    vertices: Keys,
    roles: Roles<'inputs>,
    arena: &'arena mut Arena,
    degrees: Address<Degree>,
    offsets: Address<ustore_length_t>,
    tape: Address<ustore_key_t>,
}

impl<'arena, Keys: AsRef<[Key]>> Calling for GraphReadEdges<'arena, '_, Keys> {
    type Arguments = ffi::ustore_graph_read_edges_t;
    type Output = Neighborhoods<'arena>;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        read_edges_arguments(
            view,
            outcome,
            completion,
            &self.collections,
            self.vertices.as_ref(),
            &self.roles,
            self.arena.handle,
            &mut self.degrees.0,
            &mut self.offsets.0,
            &mut self.tape.0,
        )
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_graph_read_edges(arguments)
    }

    fn finish(self) -> Result<Neighborhoods<'arena>> {
        // SAFETY: the lookup succeeded over every vertex, and the arena stays borrowed for `'arena`.
        Ok(unsafe { Neighborhoods::exported(self.vertices.as_ref().len(), self.degrees, self.offsets, self.tape) })
    }
}

// endregion: Verbs

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::blobs::Entry;
    use crate::db::{CollectionID, Collections, Database, Executor, Modality, RAMOptions, Watch};
    use crate::testing::{key, keys};

    /// A directed graph of one to two, one to three and two to three, in a collection of its own.
    fn populated() -> (Database, Arena, CollectionID) {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let graph = database.collection_create("graph", Modality::Graph).unwrap();
        let edges = [
            Edge::new(key(1), key(2)),
            Edge::new(key(1), key(3)),
            Edge::new(key(2), key(3)),
        ];
        database
            .head()
            .graph_write_edges(graph, &edges, Durability::Buffered)
            .unwrap();
        (database, Arena::new().unwrap(), graph)
    }

    #[test]
    fn lookups_find_the_edges_of_every_role() {
        let (database, mut arena, graph) = populated();
        let outgoing = database
            .head()
            .graph_read_edges(graph, &keys([1]), Roles::Every(Role::Source), &mut arena)
            .unwrap();
        assert_eq!(outgoing.degrees(), &[2]);
        assert_eq!(
            outgoing.edges(),
            &[Edge::new(key(1), key(2)), Edge::new(key(1), key(3))]
        );

        let incoming = database
            .head()
            .graph_read_edges(graph, &keys([3]), Roles::Every(Role::Target), &mut arena)
            .unwrap();
        assert_eq!(
            incoming.edges(),
            &[Edge::new(key(1), key(3)), Edge::new(key(2), key(3))]
        );

        // Both roles list the outgoing edges ahead of the incoming ones.
        let both = database
            .head()
            .graph_read_edges(graph, &keys([2]), Roles::Every(Role::Any), &mut arena)
            .unwrap();
        assert_eq!(both.edges(), &[Edge::new(key(2), key(3)), Edge::new(key(1), key(2))]);
    }

    #[test]
    fn neighborhoods_split_the_tape_by_vertex() {
        let (database, mut arena, graph) = populated();
        let found = database
            .head()
            .graph_read_edges(graph, &keys([3, 42, 1]), Roles::Every(Role::Any), &mut arena)
            .unwrap();
        assert_eq!(found.degrees(), &[2, degree_missing(), 2]);
        assert_eq!(
            found.offsets(),
            &[0, 2, 2, 4],
            "a vertex the graph does not hold spans none"
        );
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
            .graph_degrees(graph, &keys([1, 2, 3, 4]), Roles::Every(Role::Source), &mut arena)
            .unwrap();
        assert_eq!(degrees, &[2, 1, 0, degree_missing()]);
    }

    #[test]
    fn roles_differ_per_vertex_and_match_the_batch() {
        let (database, mut arena, graph) = populated();
        let roles = [Role::Source, Role::Target];
        let degrees = database
            .head()
            .graph_degrees(graph, &keys([1, 1]), Roles::Each(&roles), &mut arena)
            .unwrap();
        assert_eq!(degrees, &[2, 0]);

        let failure = database
            .head()
            .graph_read_edges(graph, &keys([1, 2, 3]), Roles::Each(&roles), &mut arena)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
        let failure = database
            .head()
            .graph_remove_vertices(graph, &keys([1]), Roles::Each(&roles), Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn identifiers_tell_parallel_edges_apart() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let graph = database.collection_create("graph", Modality::Graph).unwrap();
        let edges = [
            Edge::with_id(key(1), key(2), key(10)),
            Edge::with_id(key(1), key(2), key(20)),
            Edge::with_id(key(1), key(2), key(10)),
        ];
        database
            .head()
            .graph_write_edges(graph, &edges, Durability::Buffered)
            .unwrap();
        let degrees = database
            .head()
            .graph_degrees(graph, &keys([1]), Roles::Every(Role::Source), &mut arena)
            .unwrap();
        assert_eq!(degrees, &[2], "an edge upserted twice is stored once");

        database
            .head()
            .graph_remove_edges(graph, &[Edge::with_id(key(1), key(2), key(10))], Durability::Buffered)
            .unwrap();
        let found = database
            .head()
            .graph_read_edges(graph, &keys([2]), Roles::Every(Role::Target), &mut arena)
            .unwrap();
        assert_eq!(found.edges(), &[Edge::with_id(key(1), key(2), key(20))]);

        // Removing an edge the graph never held changes nothing.
        database
            .head()
            .graph_remove_edges(graph, &[Edge::new(key(5), key(6))], Durability::Buffered)
            .unwrap();
    }

    #[test]
    fn vertices_are_upserted_bare_and_removed_with_their_edges() {
        let (database, mut arena, graph) = populated();
        database
            .head()
            .graph_write_vertices(graph, &keys([1, 7]), Durability::Buffered)
            .unwrap();
        let degrees = database
            .head()
            .graph_degrees(graph, &keys([1, 7]), Roles::Every(Role::Any), &mut arena)
            .unwrap();
        assert_eq!(degrees, &[2, 0], "an upsert keeps the edges a vertex already has");

        database
            .head()
            .graph_remove_vertices(graph, &keys([3]), Roles::Every(Role::Any), Durability::Buffered)
            .unwrap();
        let degrees = database
            .head()
            .graph_degrees(graph, &keys([1, 2, 3]), Roles::Every(Role::Any), &mut arena)
            .unwrap();
        assert_eq!(degrees, &[1, 1, degree_missing()]);
    }

    #[test]
    fn a_vertex_removed_as_a_source_leaves_its_targets_consistent() {
        let (database, mut arena, graph) = populated();
        database
            .head()
            .graph_remove_vertices(graph, &keys([1]), Roles::Every(Role::Source), Durability::Buffered)
            .unwrap();
        let found = database
            .head()
            .graph_read_edges(graph, &keys([3]), Roles::Every(Role::Target), &mut arena)
            .unwrap();
        assert_eq!(found.edges(), &[Edge::new(key(2), key(3))]);
    }

    #[test]
    fn transactions_hold_their_edges_until_commit() {
        let (database, mut arena, graph) = populated();
        let mut transaction = database.transaction().unwrap();
        let edges = [Edge::new(key(3), key(1))];
        transaction
            .view(Watch::Keys)
            .graph_write_edges(graph, &edges, Durability::Buffered)
            .unwrap();
        let flushed = transaction
            .view(Watch::Keys)
            .graph_write_edges(graph, &edges, Durability::Flushed)
            .unwrap_err();
        assert_eq!(flushed.status, Status::InvalidArgument);

        let inside = transaction
            .view(Watch::Keys)
            .graph_degrees(graph, &keys([3]), Roles::Every(Role::Source), &mut arena)
            .unwrap();
        assert_eq!(inside, &[1]);
        let outside = database
            .head()
            .graph_degrees(graph, &keys([3]), Roles::Every(Role::Source), &mut arena)
            .unwrap();
        assert_eq!(outside, &[0]);

        transaction.commit(Durability::Buffered).unwrap();
        let committed = database
            .head()
            .graph_degrees(graph, &keys([3]), Roles::Every(Role::Source), &mut arena)
            .unwrap();
        assert_eq!(committed, &[1]);
    }

    #[test]
    fn snapshots_keep_the_graph_they_were_taken_at() {
        let (database, mut arena, graph) = populated();
        let snapshot = database.snapshot().unwrap();
        database
            .head()
            .graph_remove_edges(graph, &[Edge::new(key(1), key(2))], Durability::Buffered)
            .unwrap();
        let pinned = snapshot
            .view()
            .graph_degrees(graph, &keys([1]), Roles::Every(Role::Source), &mut arena)
            .unwrap();
        assert_eq!(pinned, &[2]);
    }

    #[test]
    fn a_collection_of_values_refuses_the_graph_verbs() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let blobs = [
            Entry::new(key(1), b"abc"),
            Entry::new(key(2), br#"{"name":"Ada Lovelace","born":1815}"#),
        ];
        database
            .head()
            .write(CollectionID::main(), &blobs, Durability::Buffered)
            .unwrap();
        for vertex in keys([1, 2]) {
            let failure = database
                .head()
                .graph_read_edges(CollectionID::main(), [vertex], Roles::Every(Role::Any), &mut arena)
                .unwrap_err();
            assert_eq!(failure.status, Status::InvalidArgument);
            let failure = database
                .head()
                .graph_write_edges(CollectionID::main(), [Edge::new(vertex, key(3))], Durability::Buffered)
                .unwrap_err();
            assert_eq!(failure.status, Status::InvalidArgument);
        }
    }

    #[test]
    fn one_batch_spans_several_graph_collections() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let follows = database.collection_create("follows", Modality::Graph).unwrap();
        let blocks = database.collection_create("blocks", Modality::Graph).unwrap();
        let spanned = [follows, blocks];

        let edges = [Edge::new(key(1), key(2)), Edge::new(key(1), key(3))];
        database
            .head()
            .graph_write_edges(spanned.as_slice(), &edges, Durability::Buffered)
            .unwrap();

        let degrees = database
            .head()
            .graph_degrees(
                Collections::Each(&spanned),
                &keys([1, 1]),
                Roles::Every(Role::Source),
                &mut arena,
            )
            .unwrap();
        assert_eq!(degrees, &[1, 1], "each edge landed in the collection of its own task");
        let inside = database
            .head()
            .graph_read_edges(blocks, &keys([1]), Roles::Every(Role::Source), &mut arena)
            .unwrap();
        assert_eq!(inside.edges(), &[Edge::new(key(1), key(3))]);

        let failure = database
            .head()
            .graph_degrees(
                Collections::Each(&spanned),
                &keys([1]),
                Roles::Every(Role::Any),
                &mut arena,
            )
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn empty_batches_reach_no_engine() {
        let (database, mut arena, graph) = populated();
        let head = database.head();
        head.graph_write_edges(graph, [], Durability::Buffered).unwrap();
        head.graph_remove_edges(graph, [], Durability::Buffered).unwrap();
        head.graph_write_vertices(graph, [], Durability::Buffered).unwrap();
        head.graph_remove_vertices(graph, &[], Roles::Each(&[]), Durability::Buffered)
            .unwrap();
        assert!(head
            .graph_degrees(graph, &[], Roles::Every(Role::Any), &mut arena)
            .unwrap()
            .is_empty());
        let found = head
            .graph_read_edges(graph, &[], Roles::Every(Role::Any), &mut arena)
            .unwrap();
        assert!(found.degrees().is_empty() && found.edges().is_empty());
        assert_eq!(found.iter().count(), 0);
    }

    #[test]
    fn graph_verbs_block_or_defer_on_an_executor() {
        let (database, _, graph) = populated();
        let executor = Executor::for_ram(&[], 2, 0).unwrap();
        let mut arena = Arena::new_in(&executor).unwrap();
        let blocking = database.head().on(&executor);
        let deferred = database.head().deferred(&executor);
        let buffered = Durability::Buffered;
        let [source, target, any] = [Role::Source, Role::Target, Role::Any].map(Roles::Every);

        // Writes and removals hand their inputs back, whichever way the call finishes.
        let edges = blocking
            .graph_write_edges(graph, vec![Edge::new(key(3), key(4))], buffered)
            .unwrap();
        let edges = deferred
            .graph_remove_edges(graph, edges, buffered)
            .unwrap()
            .wait()
            .unwrap();
        let edges = deferred
            .graph_write_edges(graph, edges, buffered)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(edges, [Edge::new(key(3), key(4))]);
        let vertices = blocking.graph_write_vertices(graph, keys([7]), buffered).unwrap();
        let vertices = deferred
            .graph_write_vertices(graph, vertices, buffered)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(vertices, keys([7]));

        // Lookups borrow one arena on either view, and each call rewinds it for the next.
        let found = blocking.graph_read_edges(graph, keys([3]), source, &mut arena).unwrap();
        assert_eq!(found.edges(), &[Edge::new(key(3), key(4))]);
        let found = deferred
            .graph_read_edges(graph, vec![key(1), key(7)], any, &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(found.degrees(), &[2, 0]);

        let counted = blocking.graph_degrees(graph, keys([1, 3]), source, &mut arena).unwrap();
        assert_eq!(counted, &[2, 1]);
        let counted = deferred
            .graph_degrees(graph, keys([4]), target, &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(counted, &[1]);

        blocking.graph_remove_vertices(graph, keys([7]), any, buffered).unwrap();
        deferred
            .graph_remove_vertices(graph, vec![key(4), key(3)], any, buffered)
            .unwrap()
            .wait()
            .unwrap();
        let counted = blocking.graph_degrees(graph, keys([7, 4, 3]), any, &mut arena).unwrap();
        assert_eq!(counted, &[degree_missing(); 3]);
    }

    #[test]
    fn edges_carry_the_default_identifier_and_name_their_neighbors() {
        assert_eq!(Edge::new(key(1), key(2)).id, default_edge_id());
        assert_eq!(Edge::new(key(1), key(2)).neighbor_of(key(2)), Some(key(1)));
        assert_eq!(Edge::new(key(1), key(2)).neighbor_of(key(3)), None);
        assert_eq!(Edge::new(key(4), key(4)).neighbor_of(key(4)), Some(key(4)));
    }
}

// endregion: Tests
