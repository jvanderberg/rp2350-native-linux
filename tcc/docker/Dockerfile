# Host test image for the Thumb-2 tcc: a native gcc to build tcc,
# arm-none-eabi-gcc to build the test musl sysroot (and its Thumb
# ARMv7-M libgcc), binutils for objdump, qemu-arm to run the results,
# and genromfs for the board test image (board-image.sh).
FROM debian:bookworm
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential ca-certificates curl xz-utils file python3 make rsync \
    qemu-user gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi \
    libncurses-dev genromfs \
    && rm -rf /var/lib/apt/lists/*
RUN useradd -m -u 1000 builder
USER builder
WORKDIR /home/builder
