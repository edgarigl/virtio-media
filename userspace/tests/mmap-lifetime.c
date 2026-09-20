/* SPDX-License-Identifier: BSD-3-Clause */
/* Destructive on an unfixed kernel: use only after installing the VMA fix. */
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { perror(#c); return 1; } } while (0)
int main(int argc, char **argv)
{
    int fd = open(argc > 1 ? argv[1] : "/dev/video0", O_RDWR | O_CLOEXEC);
    struct v4l2_requestbuffers request = {
        .count = 3, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    long page = sysconf(_SC_PAGESIZE);
    CHECK(fd >= 0 && page > 0);
    CHECK(ioctl(fd, VIDIOC_REQBUFS, &request) == 0 && request.count);
    CHECK(ioctl(fd, VIDIOC_QUERYBUF, &buffer) == 0);
    size_t len = (buffer.length + page - 1) / page * page;
    CHECK(len >= 3 * (size_t)page);
    char *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, buffer.m.offset);
    CHECK(map != MAP_FAILED);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        /* exit_mmap must drop only the child's reference, even after fd close. */
        close(fd);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    volatile unsigned char sample = *(volatile unsigned char *)map;
    /* A hole splits the original VMA; each surviving half needs a reference. */
    CHECK(munmap(map + page, page) == 0);
    CHECK(munmap(map, page) == 0);
    sample ^= *(volatile unsigned char *)(map + 2 * page);
    CHECK(munmap(map + 2 * page, len - 2 * page) == 0);
    /* The oversize error must not free mapping metadata twice. */
    errno = 0;
    map = mmap(NULL, len + page, PROT_READ, MAP_SHARED, fd, buffer.m.offset);
    CHECK(map == MAP_FAILED && errno == EINVAL);
    request.count = 0;
    CHECK(ioctl(fd, VIDIOC_REQBUFS, &request) == 0);
    close(fd);
    printf("mmap lifetime fork/split/oversize pass (sample=%u)\n", sample);
    return 0;
}
