FROM ubuntu:focal as builder
ENV DEBIAN_FRONTEND="noninteractive" TZ="Europe/London"

RUN ln -s /usr/bin/dpkg-split /usr/sbin/dpkg-split && \
    ln -s /usr/bin/dpkg-deb /usr/sbin/dpkg-deb && \
    ln -s /bin/rm /usr/sbin/rm && \
    ln -s /bin/tar /usr/sbin/tar && \
    ln -s /bin/as /usr/sbin/as

RUN apt-get update -y && \
    apt-get install -y apt-utils 2>&1 | grep -v "debconf: delaying package configuration, since apt-utils is not installed" && \
    apt-get install -y --no-install-recommends cmake git libssl-dev build-essential zlib1g zlib1g-dev python3 python3-dev

COPY . /usr/src/ukv
WORKDIR /usr/src/ukv
RUN rm -rf build _deps build_debug CMakeCache.txt CMakeFiles wheelhouse Makefile bin tmp/*

RUN git config --global http.sslVerify "false"
RUN cmake \
    -DUKV_BUILD_TESTS=0 \
    -DUKV_BUILD_BENCHMARKS=0 \
    -DUKV_BUILD_ENGINE_UMEM=1 \
    -DUKV_BUILD_ENGINE_LEVELDB=1 \
    -DUKV_BUILD_ENGINE_ROCKSDB=1 \
    -DUKV_BUILD_API_FLIGHT_CLIENT=0 \
    -DUKV_BUILD_API_FLIGHT_SERVER=1 . && \
    make -j32 \
        ukv_flight_server_umem \
        ukv_flight_server_leveldb \
        ukv_flight_server_rocksdb

## TODO: Optimize: https://github.com/docker-slim/docker-slim
FROM ubuntu:focal

RUN apt-get update -y && \
    apt-get install -y apt-utils 2>&1 | grep -v "debconf: delaying package configuration, since apt-utils is not installed" && \
    apt-get install -y --no-install-recommends libssl-dev

# Add Tini
ENV TINI_VERSION v0.19.0
ADD https://github.com/krallin/tini/releases/download/${TINI_VERSION}/tini /tini
RUN chmod +x /tini

WORKDIR /var/lib/ukv/

COPY --from=builder /usr/src/ukv/build/bin/ukv_flight_server_umem ./umem_server
COPY ./assets/config_umem.json ./umem/config_umem.json

COPY --from=builder /usr/src/ukv/build/bin/ukv_flight_server_leveldb ./leveldb_server
COPY ./assets/config_leveldb.json ./leveldb/config_leveldb.json

COPY --from=builder /usr/src/ukv/build/bin/ukv_flight_server_rocksdb ./rocksdb_server
COPY ./assets/config_rocksdb.ini ./rocksdb/config_rocksdb.ini

EXPOSE 38709
ENTRYPOINT ["/tini", "--"]
CMD ["./umem_server"]
