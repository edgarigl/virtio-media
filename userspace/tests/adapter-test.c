/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include "virtio-media-gpu.h"
#include <linux/virtio_media_gpu.h>
#include <drm.h>
#include <virtgpu_drm.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned long fail_request;
static int fail_map, missing_param, wrong_driver, bad_size, bad_token;
static int gems, fds, mappings, uuid_flag, prime_flags, interrupts;
static unsigned char storage[4096];

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;
    (void)fd;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (interrupts) {
        interrupts--;
        errno = EINTR;
        return -1;
    }
    if (request == fail_request) {
        errno = EIO;
        return -1;
    }
    switch (request) {
    case DRM_IOCTL_VERSION: {
        struct drm_version *v = arg;
        const char *name = wrong_driver ? "amdgpu" : "virtio_gpu";
        strcpy(v->name, name);
        v->name_len = strlen(name);
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_GETPARAM: {
        struct drm_virtgpu_getparam *p = arg;
        *(int *)(uintptr_t)p->value = p->param != (uint64_t)missing_param;
        return 0;
    }
    case VIDIOC_VIRTIO_MEDIA_EXPORT_GPU: {
        struct virtio_media_gpu_export *e = arg;
        assert(e->version == 1 && e->type == V4L2_BUF_TYPE_VIDEO_CAPTURE);
        assert(e->index == 2 && e->plane == 0 && !e->flags);
        e->blob_id = bad_token ? 1 : UINT64_C(0xfffe000000000007);
        e->size = bad_size ? 1 : (uint64_t)sysconf(_SC_PAGESIZE);
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB: {
        struct drm_virtgpu_resource_create_blob *b = arg;
        assert(b->blob_id == UINT64_C(0xfffe000000000007));
        assert(b->blob_mem == VIRTGPU_BLOB_MEM_HOST3D);
        assert(b->blob_flags & VIRTGPU_BLOB_FLAG_USE_SHAREABLE);
        assert(b->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE);
        uuid_flag = !!(b->blob_flags & VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE);
        b->bo_handle = 12;
        gems++;
        return 0;
    }
    case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
        struct drm_prime_handle *p = arg;
        assert(p->handle == 12);
        prime_flags = p->flags;
        p->fd = 100;
        fds++;
        return 0;
    }
    case DRM_IOCTL_GEM_CLOSE:
        assert(((struct drm_gem_close *)arg)->handle == 12 && gems == 1);
        gems--;
        return 0;
    default:
        assert(!"unexpected ioctl");
        return -1;
    }
}

void *__wrap_mmap(void *addr, size_t size, int prot, int flags, int fd,
                  off_t offset)
{
    assert(!addr && size == (size_t)sysconf(_SC_PAGESIZE));
    assert(prot == PROT_READ && flags == MAP_SHARED && fd == 100 && !offset);
    if (fail_map) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    mappings++;
    return storage;
}

int __wrap_munmap(void *addr, size_t size)
{
    assert(addr == storage && size == (size_t)sysconf(_SC_PAGESIZE));
    assert(mappings == 1);
    mappings--;
    return 0;
}

int __wrap_close(int fd)
{
    assert(fd == 100 && fds == 1);
    fds--;
    return 0;
}

static struct v4l2_exportbuffer buffer(void)
{
    return (struct v4l2_exportbuffer){
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = 2,
        .flags = O_RDONLY | O_CLOEXEC, .fd = -99,
    };
}

static void expect_failure(int error)
{
    struct v4l2_exportbuffer b = buffer(), old = b;
    assert(vmedia_gpu_expbuf(3, 4, &b, VMEDIA_GPU_ASSIGN_UUID) == -1);
    assert(errno == error && !memcmp(&b, &old, sizeof(b)));
    assert(!gems && !fds && !mappings);
}

int main(void)
{
    unsigned long failures[] = {
        DRM_IOCTL_VERSION, DRM_IOCTL_VIRTGPU_GETPARAM,
        VIDIOC_VIRTIO_MEDIA_EXPORT_GPU,
        DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB,
        DRM_IOCTL_PRIME_HANDLE_TO_FD,
    };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        fail_request = failures[i];
        expect_failure(EIO);
    }
    fail_request = 0;
    fail_map = 1;
    expect_failure(EINVAL); /* Host rejection must not return a usable fd. */
    fail_map = 0;
    wrong_driver = 1;
    expect_failure(ENODEV);
    wrong_driver = 0;
    missing_param = VIRTGPU_PARAM_CROSS_DEVICE;
    expect_failure(EOPNOTSUPP);
    missing_param = 0;
    bad_size = 1;
    expect_failure(EPROTO);
    bad_size = 0;
    bad_token = 1;
    expect_failure(EPROTO);
    bad_token = 0;
    for (int rw = 0; rw < 2; rw++) {
        for (int cloexec = 0; cloexec < 2; cloexec++) {
            struct v4l2_exportbuffer b = buffer();
            b.flags = (rw ? O_RDWR : O_RDONLY) | (cloexec ? O_CLOEXEC : 0);
            interrupts = 1;
            assert(!vmedia_gpu_expbuf(3, 4, &b, rw ? VMEDIA_GPU_ASSIGN_UUID : 0));
            assert(b.fd == 100 && !gems && fds == 1 && !mappings);
            assert(prime_flags == ((rw ? DRM_RDWR : 0) |
                                   (cloexec ? DRM_CLOEXEC : 0)));
            assert(uuid_flag == rw);
            __wrap_close(b.fd);
        }
    }
    struct v4l2_exportbuffer b = buffer();
    b.flags = O_WRONLY;
    assert(vmedia_gpu_expbuf(3, 4, &b, 0) == -1 && errno == EINVAL);
    b = buffer();
    b.reserved[0] = 1;
    assert(vmedia_gpu_expbuf(3, 4, &b, 0) == -1 && errno == EINVAL);
    b = buffer();
    assert(vmedia_gpu_expbuf(3, 4, &b, 2) == -1 && errno == EINVAL);
    assert(vmedia_gpu_expbuf(3, 4, NULL, 0) == -1 && errno == EINVAL);
    assert(!gems && !fds && !mappings);
    puts("PASS adapter flags, capabilities, retry, malformed tokens and rollback");
    return 0;
}
