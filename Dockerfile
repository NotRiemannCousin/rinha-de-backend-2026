# Single Dockerfile for both binaries (Api and LoadBalancer), selected via
# `docker build --target ...` / compose's `build.target:`.
#
# The original project had two near-identical files (Dockerfile.api,
# Dockerfile.lb) that each: installed the same toolchain, COPY'd the *entire*
# source tree (including the other binary's sources), and ran their own
# `cmake -B build -G Ninja ...` configure from scratch. That last part is the
# expensive one: CMake's configure step (CPM dependency resolution graph,
# Ninja file generation) ran twice per build even though the CPM *source*
# cache was already shared between them. It also meant editing a
# LoadBalancer-only file busted Docker's layer cache for the Api image too,
# since both COPY'd `./src` and `./include` wholesale.
#
# Here, `deps` installs the toolchain and configures the CMake project
# exactly once; `api-builder`/`lb-builder` each just run
# `cmake --build build --target <X>` against that shared configuration, and
# `api-runtime`/`lb-runtime` are the same minimal images as before.

FROM debian:sid-slim AS deps
WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build gcc-16 g++-16 git libssl-dev liburing-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV CC=gcc-16 CXX=g++-16 \
    CPM_SOURCE_CACHE=/root/.cache/CPM

COPY CMakeLists.txt CMakeLists.txt
COPY ./cmake/ ./cmake/
COPY ./src/ ./src/
COPY ./include/ ./include/

# Configure once. Both Api and LoadBalancer are declared in the same
# CMakeLists, so this one build/ tree can build either target - use the
# SAME cache mount id here and in both builder stages below so Ninja's
# incremental state (and Hermes' compiled objects, which both binaries
# link) actually persists across configure + both `--build --target`
# invocations, instead of being silently discarded and duplicated per
# target the way two separate cache ids did before.
RUN --mount=type=cache,id=main-build,target=/src/build \
    --mount=type=cache,target=/root/.cache/CPM \
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release


FROM deps AS api-builder
RUN mkdir /dist
RUN --mount=type=cache,id=main-build,target=/src/build \
    --mount=type=cache,target=/root/.cache/CPM \
    cmake --build build --target Api -j $(nproc) && \
    cp build/Api /dist/Api

FROM deps AS lb-builder
RUN mkdir /dist
RUN --mount=type=cache,id=main-build,target=/src/build \
    --mount=type=cache,target=/root/.cache/CPM \
    cmake --build build --target LoadBalancer -j $(nproc) && \
    cp build/LoadBalancer /dist/LoadBalancer


FROM debian:sid-slim AS api-runtime
WORKDIR /app
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 liburing2 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=api-builder /dist/Api /app/Api
ENTRYPOINT ["/app/Api"]

FROM debian:sid-slim AS lb-runtime
WORKDIR /app
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 liburing2 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=lb-builder /dist/LoadBalancer /app/LoadBalancer
ENTRYPOINT ["/app/LoadBalancer"]
