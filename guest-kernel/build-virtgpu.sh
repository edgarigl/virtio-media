#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 VIRTGPU_SOURCE_DIR KERNEL_BUILD_DIR NEW_OUTPUT_DIR" >&2
    exit 1
fi
gpu_source=$(realpath "$1")
gpu_build=$(realpath "$2")
gpu_output=$(realpath -m "$3")
helper_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Work only in a fresh directory; leave the kernel source and installed module alone.
mkdir "$gpu_output"
cp "$gpu_source/"*.c "$gpu_source/"*.h "$gpu_source/Makefile" "$gpu_output/"
cp "$helper_dir/virtgpu_media.c" "$gpu_output/"
sed -i 's@^#define TRACE_INCLUDE_PATH .*@#define TRACE_INCLUDE_PATH .@' \
    "$gpu_output/virtgpu_trace.h"
printf '\nvirtio-gpu-y += virtgpu_media.o\nccflags-y += -I$(src)\n' \
    >> "$gpu_output/Makefile"
make -C "$gpu_build" M="$gpu_output" -j"${FANOUT_BUILD_JOBS:-2}" modules
