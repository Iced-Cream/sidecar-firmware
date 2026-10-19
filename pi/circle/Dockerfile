FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git ca-certificates curl xz-utils make python3 dosfstools mtools \
    && rm -rf /var/lib/apt/lists/*
ARG GNU_VER=13.3.rel1
RUN curl -fsSL -o /tmp/tc.tar.xz \
    https://developer.arm.com/-/media/Files/downloads/gnu/${GNU_VER}/binrel/arm-gnu-toolchain-${GNU_VER}-aarch64-aarch64-none-elf.tar.xz \
    && mkdir /opt/tc && tar -xJf /tmp/tc.tar.xz -C /opt/tc --strip-components=1 && rm /tmp/tc.tar.xz
ENV PATH=/opt/tc/bin:$PATH
WORKDIR /work
