//! Builds the one UStore library through CMake and links it with every engine the features enable.
//!
//! No C or C++ is compiled here: CMake owns the engines, their dependencies and their flags, and this
//! script installs its libraries into `OUT_DIR` and links them. The `ram` engine always builds; the
//! `rocksdb`, `leveldb` and `nvme` features add theirs, and a missing one refuses to open at run time.

use std::env;
use std::error::Error;
use std::path::PathBuf;

/// Every CMake option this crate sets, paired with the Cargo feature naming it - `None` stays off, since no
/// feature turns it on.
const KNOWN_OPTIONS: &[(&str, Option<&str>)] = &[
    ("USTORE_WITH_ROCKSDB", Some("rocksdb")),
    ("USTORE_WITH_LEVELDB", Some("leveldb")),
    ("USTORE_WITH_SPDK", None),
    ("USTORE_BUILD_TEST", None),
    ("USTORE_BUILD_BENCH", None),
    ("USTORE_BUILD_TOOLS", None),
    ("USTORE_BUILD_LIB_PYTHON", None),
    ("USTORE_BUILD_NVME", Some("nvme")),
    ("USTORE_BUILD_CUDA", None),
];

/// The static archives `ustore` links beside itself whatever the features, in link order - mirrors its
/// `target_link_libraries` call in `CMakeLists.txt`.
const DEPENDENCY_ARCHIVES: &[&str] = &["yyjson", "forkunion_static", "stringzilla_static"];

/// The paths whose contents rebuild the libraries.
const SOURCES: &[&str] = &[
    "CMakeLists.txt",
    "VERSION",
    "cmake",
    "include",
    "c",
    "src",
    "src_oracles/leveldb",
    "src_oracles/rocksdb",
    "smashtable/CMakeLists.txt",
    "smashtable/cmake",
    "smashtable/include",
    "stringzilla/CMakeLists.txt",
    "stringzilla/cmake",
    "stringzilla/c/stringzilla",
    "stringzilla/probes",
    "stringzilla/include",
    "forkunion/CMakeLists.txt",
    "forkunion/cmake",
    "forkunion/c",
    "forkunion/probes",
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
    let target_os = env::var("CARGO_CFG_TARGET_OS")?;
    println!("cargo:rustc-link-lib=static=ustore");
    if feature_enabled("rocksdb") {
        println!("cargo:rustc-link-lib=static=rocksdb");
        // RocksDB reads through io_uring on Linux, from the liburing CMake builds beside it.
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
    println!("cargo:rustc-link-lib=static={fmt}");
    for archive in DEPENDENCY_ARCHIVES {
        println!("cargo:rustc-link-lib=static={archive}");
    }

    // The archives are C++, so the standard library they were compiled against comes along.
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
