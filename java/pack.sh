#!/usr/bin/env bash
set -e

### Set JAVA_HOME!
# Must have `incldue` dir inside for jni headers
# Depending on your platform, Java may be installed at different paths.
# Linux:
#       apt-get install default-jdk
#       export JAVA_HOME="/usr/lib/jvm/java-11-openjdk-amd64"
# MacOS:
#       brew install java
#       export JAVA_HOME="/usr/libexec/java_home -v 1.8"

cmake \
    -DUSTORE_BUILD_ENGINE_UCSET=1 \
    -DUSTORE_BUILD_ENGINE_ROCKSDB=1 \
    -DUSTORE_BUILD_ENGINE_LEVELDB=1 \
    -DUSTORE_BUILD_BUNDLES=1 \
    -DUSTORE_BUILD_TESTS=0 \
    -DUSTORE_BUILD_BENCHMARKS=0 \
    . 
make -j16
./java/gradlew build #--info
