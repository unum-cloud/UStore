//! A directory the suites open persistent databases in, removed when the test that took it ends.

use std::fs;

use crate::db::Key;

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
