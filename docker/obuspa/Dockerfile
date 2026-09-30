# OB-USP-Agent (Broadband Forum reference USP Agent), used as a second test agent.
FROM ubuntu:24.04
ARG OBUSPA_REF=master
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential autoconf automake libtool pkg-config git ca-certificates \
      libssl-dev libcurl4-openssl-dev libsqlite3-dev libz-dev libmosquitto-dev \
      libwebsockets-dev libprotobuf-c-dev \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch ${OBUSPA_REF} https://github.com/BroadbandForum/obuspa.git /obuspa
WORKDIR /obuspa
RUN autoreconf -fi && ./configure && make -j"$(nproc)" && make install
COPY factory_reset.txt /etc/obuspa/factory_reset.txt
# -p: log to stdout, -r: factory reset file, -f: database (recreated on each start)
ENTRYPOINT ["sh", "-c", "rm -f /tmp/usp.db && exec obuspa -p -v 3 -r /etc/obuspa/factory_reset.txt -f /tmp/usp.db -i eth0"]
