# Linux build and benchmark image: docker build -t kvlog-dev docker/
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++-14 clang-18 lld-18 libclang-rt-18-dev \
        cmake ninja-build git ca-certificates curl \
        iproute2 iputils-ping procps time python3 util-linux \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100
WORKDIR /src
