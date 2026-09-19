/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include "virtio-media-gpu.h"
#include <linux/virtio_media_gpu.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s: %s\n", __LINE__, #x, strerror(errno)); \
    exit(1); } } while (0)
struct uuid_query { int32_t fd; uint8_t uuid[16]; };
#define UUID_QUERY _IOWR('U', 0, struct uuid_query)

int main(int argc, char **argv)
{
    int fds[64][2], video, gpu, probe = -1;
    struct uuid_query first[64];
    struct v4l2_requestbuffers req = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
        .count = 8,
    };
    CHECK(argc == 3 || argc == 4);
    alarm(60);
    video = open(argv[1], O_RDWR | O_CLOEXEC);
    gpu = open(argv[2], O_RDWR | O_CLOEXEC);
    CHECK(video >= 0 && gpu >= 0);
    if (argc == 4) {
        probe = open(argv[3], O_RDWR | O_CLOEXEC);
        CHECK(probe >= 0);
    }
    CHECK(!ioctl(video, VIDIOC_REQBUFS, &req));
    CHECK(req.count > 0 && req.count <= 64);
    unsigned count = req.count;
    struct virtio_media_gpu_export info = {
        .version = VIRTIO_MEDIA_GPU_EXPORT_VERSION, .type = req.type,
    };
    info.version++;
    CHECK(ioctl(video, VIDIOC_VIRTIO_MEDIA_EXPORT_GPU, &info) == -1 &&
          errno == EINVAL);
    info.version--;
    info.reserved[0] = 1;
    CHECK(ioctl(video, VIDIOC_VIRTIO_MEDIA_EXPORT_GPU, &info) == -1 &&
          errno == EINVAL);
    info.reserved[0] = 0;
    info.flags = 1;
    CHECK(ioctl(video, VIDIOC_VIRTIO_MEDIA_EXPORT_GPU, &info) == -1 &&
          errno == EINVAL);
    info.flags = 0;
    info.index = count;
    CHECK(ioctl(video, VIDIOC_VIRTIO_MEDIA_EXPORT_GPU, &info) == -1 &&
          errno == EINVAL);
    info.index = 0;
    CHECK(!ioctl(video, VIDIOC_VIRTIO_MEDIA_EXPORT_GPU, &info));
    for (unsigned i = 0; i < count; i++) {
        for (unsigned n = 0; n < 4; n++) {
            int flags = (n & 1 ? O_RDWR : O_RDONLY) |
                        (n & 2 ? O_CLOEXEC : 0);
            struct v4l2_exportbuffer b = {
                .type = req.type, .index = i, .flags = flags, .fd = -1,
            };
            CHECK(!vmedia_gpu_expbuf(video, gpu, &b,
                                    probe >= 0 ? VMEDIA_GPU_ASSIGN_UUID : 0));
            CHECK((fcntl(b.fd, F_GETFL) & O_ACCMODE) == (flags & O_ACCMODE));
            CHECK(!!(fcntl(b.fd, F_GETFD) & FD_CLOEXEC) ==
                  !!(flags & O_CLOEXEC));
            if (n < 2) {
                fds[i][n] = b.fd;
                if (probe >= 0) {
                    struct uuid_query q = { .fd = b.fd };
                    CHECK(!ioctl(probe, UUID_QUERY, &q));
                    if (!n) {
                        first[i] = q;
                        for (unsigned j = 0; j < i; j++)
                            CHECK(memcmp(first[j].uuid, q.uuid, 16));
                    } else {
                        CHECK(!memcmp(first[i].uuid, q.uuid, 16));
                    }
                }
            } else {
                close(b.fd);
            }
        }
    }
    req.count = 0;
    CHECK(!ioctl(video, VIDIOC_REQBUFS, &req));
    info.index = 0;
    CHECK(ioctl(video, VIDIOC_VIRTIO_MEDIA_EXPORT_GPU, &info) == -1 &&
          errno == EINVAL);
    close(video);
    close(gpu);
    for (unsigned i = 0; i < count; i++) {
        close(fds[i][0]);
        off_t size = lseek(fds[i][1], 0, SEEK_END);
        CHECK(size > 0);
        void *map = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, fds[i][1], 0);
        CHECK(map != MAP_FAILED);
        CHECK(!munmap(map, (size_t)size));
        if (probe >= 0) {
            struct uuid_query q = { .fd = fds[i][1] };
            CHECK(!ioctl(probe, UUID_QUERY, &q));
            CHECK(!memcmp(first[i].uuid, q.uuid, 16));
            printf("slot=%u uuid=", i);
            for (unsigned j = 0; j < 16; j++) printf("%02x", q.uuid[j]);
            puts(" retained=1");
        }
        close(fds[i][1]);
    }
    if (probe >= 0) close(probe);
    puts("PASS export ABI, fd flags, aliases and lifetime");
    return 0;
}
