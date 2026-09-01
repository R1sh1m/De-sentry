# syntax=docker/dockerfile:1
# De-Sentry peer daemon image.
#
# Build stage compiles the engine from source (CMake + GCC + OpenSSL).
# The final stage ships the `desentryd` binary plus a config-generating
# entrypoint so the same image can run any node in a cluster by passing
# environment variables (see docker-entrypoint.sh and docker-compose.yml).

# ---------------------------------------------------------------------------
# Builder
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        pkg-config \
        libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy only the source first (keeps the build layer cached unless sources change).
COPY CMakeLists.txt ./
COPY include/ include/
COPY src/ src/
COPY apps/ apps/
COPY clients/ clients/
COPY config/ config/
COPY tests/ tests/

RUN mkdir -p build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo .. \
    && cmake --build . --parallel "$(nproc)"

# ---------------------------------------------------------------------------
# Runtime
# ---------------------------------------------------------------------------
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 \
        ca-certificates \
        python3 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system desentry \
    && useradd --system --gid desentry --create-home desentry

WORKDIR /opt/desentry

# Engine binary + Python client + entrypoint that renders node.json from env.
COPY --from=builder /src/build/desentryd /usr/local/bin/desentryd
COPY --from=builder /src/build/desentry_cli /usr/local/bin/desentry_cli
COPY --from=builder /src/build/crdt_test /usr/local/bin/crdt_test
COPY --from=builder /src/build/crypto_test /usr/local/bin/crypto_test
COPY --from=builder /src/build/storage_test /usr/local/bin/storage_test
COPY --from=builder /src/build/network_test /usr/local/bin/network_test
COPY --from=builder /src/clients/python/desentry_client.py /opt/desentry/clients/python/desentry_client.py
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

# Per-node data lives here; mounted as a volume in compose for durability.
RUN mkdir -p /data && chown -R desentry:desentry /data /opt/desentry
USER desentry

EXPOSE 7701 7801 7901
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["desentryd"]
