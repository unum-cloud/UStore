#!/usr/bin/env bash
# Writes the symbol bundle unum.cloud renders: each binding's own tool output, or its sources where no tool emits one.
#
#     .github/scripts/build-docs.sh        # → build/ustore_docs_<version>.tar.gz
set -euo pipefail
version=$(cat VERSION)
name=ustore_docs_$version out=build/ustore_docs_$version
# Rustdoc's JSON is unstable, and `rust-version` needs 1.100, which reaches stable on 2026-11-12.
toolchain=${USTORE_RUST_TOOLCHAIN:-beta-2026-10-18}
rm -rf "$out" && mkdir -p "$out"/{doxygen,headers,python,rust,mojo} build/docs-scratch/library

rm -rf build/xml && (cd docs && doxygen conf.dox) && cp -r build/xml/. "$out/doxygen"
git ls-files 'c/ustore/*.h' 'cpp/ustore/*.hpp' | xargs cp --parents -t "$out/headers"
griffe dump ustore --search python --no-inspection --docstyle google --full --output "$out/python/ustore.json"
cp python/ustore/lib.pyi "$out/python/"
USTORE_LIBRARY_DIR=$PWD/build/docs-scratch/library RUSTC_BOOTSTRAP=1 \
    cargo +"$toolchain" rustdoc --lib -- -Z unstable-options --output-format json
cp "${CARGO_TARGET_DIR:-target}/doc/ustore.json" "$out/rust/"
# `mojo doc` needs only the compiler, not the environment `pixi.toml` builds the library into.
pixi exec -c conda-forge -c https://conda.modular.com/max --spec 'mojo>=1.2.0,<1.3' \
    mojo doc -o "$out/mojo/ustore.json" mojo/ustore
cp mojo/ustore/__init__.mojo "$out/mojo/"
# griffe, rustdoc, Mojo and Doxygen's body locations hold absolute paths, which would leak the build machine's
# directories.
sed -i "s|$PWD/||g; s|$PWD|.|g; s|$HOME|~|g" "$out/python/ustore.json" "$out/rust/ustore.json" \
    "$out/mojo/ustore.json" "$out"/doxygen/*.xml
printf '{"project":"ustore","version":"%s","commit":"%s"}\n' "$version" "$(git rev-parse HEAD)" > "$out/manifest.json"
tar -czf "build/$name.tar.gz" -C build "$name"
