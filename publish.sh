#!/bin/sh
set -e

version=$(cat VERSION)
rev=$(git rev-list HEAD --count)
version="${version%.*}.$rev"
echo -n $version > VERSION
echo -e "------ \e[104mStarting UKV - $version Build\e[0m ------"

###

# Clean up
echo -e "------ \e[93mClean up\e[0m ------"
rm -rf CMakeCache.txt CMakeFiles wheelhouse Makefile bin tmp/*

# Build and Test
echo -e "------ \e[93mBuild\e[0m ------"
cmake -DCMAKE_BUILD_TYPE=Release -DUKV_BUILD_TESTS=1 -B ./build_release \
      -DUKV_BUILD_ENGINE_UMEM=1 -DUKV_BUILD_ENGINE_LEVELDB=1 -DUKV_BUILD_ENGINE_ROCKSDB=1 -DUKV_BUILD_API_FLIGHT=0 \
      .
make -j 32 -C ./build_release
echo -e "------ \e[92mBuild Passing\e[0m ------"

for test in $(ls ./build_release/build/bin/*test_units*); do
    echo -e "------ \e[93mRunning $test\e[0m ------";
    $test
done
echo -e "------ \e[92mTests Passing!\e[0m ------"

# Build and Test Python
pip install cibuildwheel twine
echo -e "------ \e[93mBuild and Test Python\e[0m ------"
CIBW_BUILD="cp37-*" cibuildwheel --platform linux &
CIBW_BUILD="cp38-*" cibuildwheel --platform linux &
CIBW_BUILD="cp39-*" cibuildwheel --platform linux &
CIBW_BUILD="cp310-*" cibuildwheel --platform linux
echo -e "------ \e[92mPython Tests Passing!\e[0m ------"

# Publish Python
read -p "Publish to PyPi? " -n 1 -r
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo -e "------ \e[93mPublishing Python to PyPi\e[0m ------"
    twine upload wheelhouse/*.whl --verbose
    echo -e "------ \e[92mPython Published!\e[0m ------"
fi

# Build and Test Java
echo -e "------ \e[93mBuild and Test Java\e[0m ------"
export JAVA_HOME="/usr/lib/jvm/java-11-openjdk-amd64"
bash java/pack.sh
echo -e "------ \e[92mJava Tests Passing!\e[0m ------"

# Publish Java
read -p "Publish to oss.sonatype.org? " -n 1 -r
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo -e "------ \e[93mPublishing Java to oss.sonatype.org\e[0m ------"
    ./java/gradlew publish
    echo -e "------ \e[92mJava Published!\e[0m ------"
fi

# Build Go
echo -e "------ \e[93mBuild GO\e[0m ------"
bash go-ukv/pack.sh
echo -e "------ \e[92mGo Built!\e[0m ------"

# Test And Publish Go
read -p "Publish Go? " -n 1 -r
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo -e "------ \e[93mPublishing Go to go-ukv\e[0m ------"
    cd go-ukv && bash publish.sh && cd ../
    echo -e "------ \e[92mGo Published!\e[0m ------"
fi

# Build Docker
echo -e "\n------ \e[93mBuilding Docker\e[0m ------"
docker buildx build --platform=linux/amd64,linux/arm64 . --file docker/Dockerfile --tag unum/ukv:$version-focal
echo -e "------ \e[92mDocker Built!\e[0m ------"

read -p "Publish to DockerHub? " -n 1 -r
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo -e "------ \e[93mPublishing to DockerHub\e[0m ------"
    docker login && docker push unum/ukv:$version-focal
    echo -e "------ \e[92mDocker Publish!\e[0m ------"
fi

echo "Bye!"