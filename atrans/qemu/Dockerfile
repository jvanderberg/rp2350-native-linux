# qemu-arm user mode for host tests of ATRANS programs (test-qemu.sh)
FROM debian:bookworm
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    qemu-user-static file procps binutils-arm-linux-gnueabihf \
    && rm -rf /var/lib/apt/lists/*
