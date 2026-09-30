FROM ubuntu:24.04 AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake libprotobuf-dev protobuf-compiler libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY proto ./proto
COPY src ./src
COPY tools ./tools
COPY tests ./tests
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" \
    && ./build/unit_tests

FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libprotobuf32t64 libssl3t64 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home usp
COPY --from=build /src/build/usp-controller /src/build/usp-fake-agent /usr/local/bin/
COPY config/controller.conf /etc/usp/controller.conf
USER usp
ENTRYPOINT ["usp-controller", "--config", "/etc/usp/controller.conf"]
