FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends gcc make pkg-config libusb-1.0-0-dev libc6-dev util-linux && rm -rf /var/lib/apt/lists/*
WORKDIR /src
