#!/usr/bin/env bash

### Set JAVA_HOME!
# Must have `incldue` dir inside for jni headers
# Depending on your platform, Java may be installed at different paths.
# Linux:
#       apt-get install default-jdk
#       export JAVA_HOME="/usr/lib/jvm/java-11-openjdk-amd64"
# MacOS:
#       brew install java
#       export JAVA_HOME="/usr/libexec/java_home -v 1.8"

./java/gradlew build
