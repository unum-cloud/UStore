//! Builds the engine libraries through CMake and links the in-memory engine.
//!
//! No C or C++ is compiled here: CMake owns the engines, their dependencies and their flags, and this
//! script installs its libraries into `OUT_DIR` and links them. Every engine library defines the same
//! `ustore_*` symbols until `c/lib.cpp` dispatches between them, so the `rocksdb` and `leveldb` features
//! compile and install their engines while `ram` stays the one linked.

use std::env;
use std::error::Error;
use std::path::PathBuf;

/// Every CMake option this crate sets, paired with the Cargo feature naming it - `None` stays off, since no
/// feature turns it on.
const KNOWN_OPTIONS: &[(&str, Option<&str>)] = &[
    ("USTORE_USE_ROCKSDB", Some("rocksdb")),
    ("USTORE_USE_LEVELDB", Some("leveldb")),
    ("USTORE_USE_SPDK", None),
    ("USTORE_BUILD_TEST_CPP", None),
    ("USTORE_BUILD_BENCH_CPP", None),
    ("USTORE_BUILD_TOOLS_CPP", None),
    ("USTORE_BUILD_LIB_PYTHON", None),
    ("USTORE_BUILD_UDISK", None),
    ("USTORE_BUILD_CUDA", None),
];

/// The static archives `ustore_embedded_ram` links beside itself, in link order - mirrors its
/// `target_link_libraries` call in `CMakeLists.txt`.
const DEPENDENCY_ARCHIVES: &[&str] = &["yyjson", "forkunion_static", "stringzilla_static"];

/// The paths whose contents rebuild the libraries.
const SOURCES: &[&str] = &[
    "CMakeLists.txt",
    "VERSION",
    "cmake",
    "include",
    "c",
    "src/helpers",
    "src/executor.hpp",
    "src/ram",
    "src_oracles/leveldb",
    "src_oracles/rocksdb",
    "smashtable/CMakeLists.txt",
    "smashtable/cmake",
    "smashtable/include",
    "stringzilla/CMakeLists.txt",
    "stringzilla/cmake",
    "stringzilla/c/stringzilla",
    "stringzilla/include",
    "forkunion/CMakeLists.txt",
    "forkunion/c",
    "forkunion/include",
];

/// Whether the named Cargo feature is enabled for this build.
fn feature_enabled(name: &str) -> bool {
    env::var(format!("CARGO_FEATURE_{}", name.to_uppercase())).is_ok()
}

fn main() -> Result<(), Box<dyn Error>> {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);

    let mut build = cmake::Config::new(&manifest);
    build
        .define("USTORE_BUILD_LIB_C", "ON")
        .define("USTORE_INSTALL", "ON")
        .define("CMAKE_INSTALL_LIBDIR", "lib")
        .always_configure(false);
    for (option, feature) in KNOWN_OPTIONS {
        let enabled = feature.is_some_and(feature_enabled);
        build.define(*option, if enabled { "ON" } else { "OFF" });
    }

    // fmt suffixes its Debug archive with `d`.
    let fmt = if build.get_profile() == "Debug" {
        "fmtd"
    } else {
        "fmt"
    };

    let prefix = build.build();
    let lib_dir = prefix.join("lib").join("ustore");
    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=static=ustore_embedded_ram");
    println!("cargo:rustc-link-lib=static={fmt}");
    for archive in DEPENDENCY_ARCHIVES {
        println!("cargo:rustc-link-lib=static={archive}");
    }

    // The archives are C++, so the standard library they were compiled against comes along.
    let target_os = env::var("CARGO_CFG_TARGET_OS")?;
    let target_env = env::var("CARGO_CFG_TARGET_ENV")?;
    match (target_os.as_str(), target_env.as_str()) {
        (_, "msvc") => {}
        ("macos" | "ios" | "freebsd", _) => println!("cargo:rustc-link-lib=dylib=c++"),
        _ => println!("cargo:rustc-link-lib=dylib=stdc++"),
    }

    // Cargo scans a watched directory recursively.
    for source in SOURCES {
        println!("cargo:rerun-if-changed={}", manifest.join(source).display());
    }
    Ok(())
}
