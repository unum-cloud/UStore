//! A deferred call allocates nothing on the Rust side: its outcome, arguments, owned inputs and
//! output slots live inline in the `Deferred` its caller pins, whether it is waited on or awaited.
//!
//! File: rust/tests/deferred_allocations.rs
//! Author: Ash Vardanian
//! Date: November 18, 2026

use std::alloc::{GlobalAlloc, Layout, System};
use std::future::Future;
use std::pin::pin;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::task::{Context, Poll, Waker};

use ustore::{Arena, BlobsVerbs, Database, Executor, Key, Modality, RAMOptions};

/// The system allocator, counting every allocation the binding asks of it.
struct Counting;

static ALLOCATIONS: AtomicUsize = AtomicUsize::new(0);

unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        unsafe { System.alloc(layout) }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) }
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        unsafe { System.realloc(pointer, layout, size) }
    }
}

#[global_allocator]
static GLOBAL: Counting = Counting;

/// Polls `future` to completion on this thread with a no-op waker.
fn block_on<F: Future>(future: F) -> F::Output {
    let mut future = pin!(future);
    let mut context = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(output) = future.as_mut().poll(&mut context) {
            return output;
        }
    }
}

fn main() -> ustore::Result<()> {
    let database = Database::init_ram(&RAMOptions::default())?;
    let executor = Executor::for_ram(&[], 1)?;
    let mut arena = Arena::new_in(&executor)?;
    let people = database.collection_create("people", Modality::Blobs)?;
    let keys = [Key::from(1u64), Key::from(2u64)];

    let mut allocations = 0;
    for round in 0..2 {
        let before = ALLOCATIONS.load(Ordering::Relaxed);
        for _ in 0..100 {
            let waited = database.head().deferred(&executor).read(people, keys, &mut arena)?.wait()?;
            assert_eq!(waited.len(), 2);
            let awaited = block_on(database.head().deferred(&executor).read(people, keys, &mut arena)?)?;
            assert_eq!(awaited.len(), 2);
        }
        allocations = ALLOCATIONS.load(Ordering::Relaxed) - before;
        println!("round {round}: 200 deferred calls, {allocations} allocations");
    }
    assert_eq!(allocations, 0, "a warm deferred call allocates nothing");
    Ok(())
}
