// SPDX-License-Identifier: GPL-2.0-only
/* Query two GPU aliases per V4L2 slot before and after video queue teardown. */
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
struct uuid_query { int32_t fd; uint8_t uuid[16]; };
#define UUID_QUERY _IOWR('U', 0, struct uuid_query)
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s: %s\n", \
    __LINE__, #c, strerror(errno)); exit(1); } } while (0)
int main(int argc, char **argv)
{
    struct v4l2_requestbuffers req = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP, .count = 8 };
    struct uuid_query first[64], alias[64];
    int video, query;
    CHECK(argc >= 2 && argc <= 3);
    alarm(30);
    video = open(argv[1], O_RDWR | O_CLOEXEC);
    query = open("/dev/fanout-uuid-probe", O_RDWR | O_CLOEXEC);
    CHECK(video >= 0 && query >= 0);
    CHECK(ioctl(video, VIDIOC_REQBUFS, &req) == 0 && req.count > 0 && req.count <= 64);
    unsigned count = req.count;
    for (unsigned step = 0; step < count; step++) {
        unsigned i = argc == 3 ? count - 1 - step : step;
        for (unsigned n = 0; n < 2; n++) {
            struct v4l2_exportbuffer exp = { .type = req.type, .index = i, .flags = O_CLOEXEC | O_RDONLY };
            struct uuid_query *q = n ? &alias[i] : &first[i];
            CHECK(ioctl(video, VIDIOC_EXPBUF, &exp) == 0);
            q->fd = exp.fd;
            CHECK(ioctl(query, UUID_QUERY, q) == 0);
        }
        CHECK(!memcmp(first[i].uuid, alias[i].uuid, 16));
        for (unsigned j = 0; j < step; j++) {
            unsigned previous = argc == 3 ? count - 1 - j : j;
            CHECK(memcmp(first[i].uuid, first[previous].uuid, 16));
        }
    }
    req.count = 0;
    CHECK(ioctl(video, VIDIOC_REQBUFS, &req) == 0);
    close(video);
    for (unsigned i = 0; i < count; i++) {
        close(first[i].fd);
        struct uuid_query retained = { .fd = alias[i].fd };
        CHECK(ioctl(query, UUID_QUERY, &retained) == 0);
        CHECK(!memcmp(first[i].uuid, retained.uuid, 16));
        printf("slot=%u uuid=", i);
        for (unsigned b = 0; b < 16; b++) printf("%02x", retained.uuid[b]);
        puts(" retained=1");
        close(alias[i].fd);
    }
    close(query);
    puts("PASS guest UUID identity and alias lifetime");
    return 0;
}
