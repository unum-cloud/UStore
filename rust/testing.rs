//! A directory the suites open persistent databases in, removed when the test that took it ends, and the one-thread
//! executor their `async` blocks run on.

use std::fs;
use std::future::Future;
use std::pin::pin;
use std::task::{Context, Poll, Waker};

use crate::db::Key;

/// Polls `task` on the calling thread until it is ready, over a waker that does nothing, since a deferred call that
/// is not final asks to be polled again.
pub fn block_on<Output>(task: impl Future<Output = Output>) -> Output {
    let mut task = pin!(task);
    let mut context = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(output) = task.as_mut().poll(&mut context) {
            return output;
        }
    }
}

/// The key that spells `integer`, which the suites number their keys with.
pub fn key(integer: u64) -> Key {
    Key::from(integer)
}

/// The keys that spell `integers`, in order.
pub fn keys<const COUNT: usize>(integers: [u64; COUNT]) -> [Key; COUNT] {
    integers.map(Key::from)
}

/// A directory under the build tree, created on the way in and removed on the way out.
pub struct TemporaryDirectory {
    path: String,
}

impl TemporaryDirectory {
    /// The directory a database opens at.
    pub fn path(&self) -> &str {
        &self.path
    }
}

impl Drop for TemporaryDirectory {
    fn drop(&mut self) {
        let removed = fs::remove_dir_all(&self.path);
        debug_assert!(removed.is_ok(), "the suite could not remove {}", self.path);
    }
}

/// An empty directory named after the test that takes it, which no other process shares.
pub fn temporary_directory(name: &str) -> TemporaryDirectory {
    let path = format!(
        "{}/target/rust_tests/{name}-{}",
        env!("CARGO_MANIFEST_DIR"),
        std::process::id()
    );
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).expect("the suite needs a directory to open a database in");
    TemporaryDirectory { path }
}
