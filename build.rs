//! Builds the one UStore library through CMake and links it with every engine the features enable.
//!
//! No C or C++ is compiled here: CMake owns the engines, their dependencies and their flags, and this
//! script installs its libraries into `OUT_DIR` and links the static one beside the archives of the
//! storage engines. The sibling crates hand CMake their trees, and ForkUnion and StringZilla link their
//! own archives, so the UStore archive leaves their objects out. The `ram` engine always builds; the
//! `rocksdb`, `leveldb` and `nvme` features add theirs, and a missing one refuses to open at run time.
//!
//! File: build.rs
//! Author: Ash Vardanian
//! Date: September 16, 2026

use std::env;
use std::error::Error;
use std::path::{Path, PathBuf};

/// Every CMake option a Cargo feature turns on, paired with that feature.
const KNOWN_OPTIONS: &[(&str, &str)] = &[
    ("USTORE_WITH_ROCKSDB", "rocksdb"),
    ("USTORE_WITH_LEVELDB", "leveldb"),
    ("USTORE_WITH_NVME", "nvme"),
];

/// The paths whose contents rebuild the libraries.
const SOURCES: &[&str] = &[
    "CMakeLists.txt",
    "VERSION",
    "c",
    "cmake",
    "cpp",
    "include",
    "src",
    "src_oracles/leveldb",
    "src_oracles/rocksdb",
];

/// Whether the named Cargo feature is enabled for this build.
fn feature_enabled(name: &str) -> bool {
    env::var(format!("CARGO_FEATURE_{}", name.to_uppercase())).is_ok()
}

/// The tree a sibling crate reports through its `links` metadata.
fn sibling_source(links: &str) -> Result<String, String> {
    let variable = format!("DEP_{}_SOURCE", links.to_uppercase());
    env::var(&variable).map_err(|_| {
        format!("{variable} is unset: this `{links}` crate predates reporting its tree")
    })
}

fn main() -> Result<(), Box<dyn Error>> {
    if env::var_os("DOCS_RS").is_some() {
        return Ok(());
    }
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let target_os = env::var("CARGO_CFG_TARGET_OS")?;
    let target_env = env::var("CARGO_CFG_TARGET_ENV")?;

    // A directory holding `ustore_static` and the engine archives beside it, like the `lib/` of a
    // prefix CMake installed with `USTORE_BUNDLE_SIBLINGS=OFF`, skips the build here.
    println!("cargo:rerun-if-env-changed=USTORE_LIBRARY_DIR");
    let library = match env::var_os("USTORE_LIBRARY_DIR") {
        Some(directory) => {
            let directory = PathBuf::from(directory);
            let archive = match target_env.as_str() {
                "msvc" => "ustore_static.lib",
                _ => "libustore_static.a",
            };
            println!(
                "cargo:rerun-if-changed={}",
                directory.join(archive).display()
            );
            directory
        }
        None => build_library(&manifest)?.join("lib"),
    };
    println!("cargo:rustc-link-search=native={}", library.display());
    println!("cargo:rustc-link-lib=static=ustore_static");
    if feature_enabled("rocksdb") {
        println!("cargo:rustc-link-lib=static=rocksdb");
        // RocksDB reads through io_uring on Linux, from the liburing archive installed beside it.
        if target_os == "linux" {
            println!("cargo:rustc-link-lib=static=uring");
        }
    }
    if feature_enabled("leveldb") {
        println!("cargo:rustc-link-lib=static=leveldb");
    }
    // Collection identifiers draw their entropy from `BCryptGenRandom` on Windows.
    if target_os == "windows" {
        println!("cargo:rustc-link-lib=dylib=bcrypt");
    }

    // The archives are C++, so the standard library they were compiled against comes along.
    match (target_os.as_str(), target_env.as_str()) {
        (_, "msvc") => {}
        ("macos" | "ios" | "freebsd", _) => println!("cargo:rustc-link-lib=dylib=c++"),
        _ => println!("cargo:rustc-link-lib=dylib=stdc++"),
    }
    Ok(())
}

/// Installs the libraries through CMake into `OUT_DIR`, answering the prefix.
fn build_library(manifest: &Path) -> Result<PathBuf, Box<dyn Error>> {
    let mut build = cmake::Config::new(manifest);
    build
        .define("USTORE_BUILD_SHARED", "ON")
        .define("USTORE_INSTALL", "ON")
        .define("CMAKE_INSTALL_LIBDIR", "lib")
        .define("USTORE_BUNDLE_SIBLINGS", "OFF")
        .always_configure(false);
    for (option, feature) in KNOWN_OPTIONS {
        build.define(*option, if feature_enabled(feature) { "ON" } else { "OFF" });
    }
    for (variable, links) in [
        ("FETCHCONTENT_SOURCE_DIR_FORKUNION", "forkunion"),
        ("FETCHCONTENT_SOURCE_DIR_SMASHTABLE", "smashtable"),
        ("FETCHCONTENT_SOURCE_DIR_STRINGZILLA", "stringzilla"),
    ] {
        build.define(variable, sibling_source(links)?);
    }

    // Cargo scans a watched directory recursively.
    for source in SOURCES {
        println!("cargo:rerun-if-changed={}", manifest.join(source).display());
    }
    Ok(build.build())
}
