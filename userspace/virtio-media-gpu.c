/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include "virtio-media-gpu.h"
#include <linux/virtio_media_gpu.h>
#include <drm.h>
#include <virtgpu_drm.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

static int require_param(int fd, uint64_t id)
{
    int value = 0;
    struct drm_virtgpu_getparam param = {
        .param = id, .value = (uintptr_t)&value,
    };
    if (xioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &param))
        return -1;
    if (!value) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return 0;
}

static int check_gpu(int fd, uint32_t options)
{
    char name[32] = {0};
    struct drm_version version = {
        .name_len = sizeof(name) - 1, .name = name,
    };
    if (xioctl(fd, DRM_IOCTL_VERSION, &version))
        return -1;
    if (version.name_len != strlen("virtio_gpu") ||
        strcmp(name, "virtio_gpu")) {
        errno = ENODEV;
        return -1;
    }
    if (require_param(fd, VIRTGPU_PARAM_3D_FEATURES) ||
        require_param(fd, VIRTGPU_PARAM_RESOURCE_BLOB) ||
        require_param(fd, VIRTGPU_PARAM_HOST_VISIBLE))
        return -1;
    if ((options & VMEDIA_GPU_ASSIGN_UUID) &&
        require_param(fd, VIRTGPU_PARAM_CROSS_DEVICE))
        return -1;
    return 0;
}

int vmedia_gpu_expbuf(int video_fd, int drm_fd,
                     struct v4l2_exportbuffer *buffer, uint32_t options)
{
    struct virtio_media_gpu_export token = {
        .version = VIRTIO_MEDIA_GPU_EXPORT_VERSION,
    };
    struct drm_virtgpu_resource_create_blob blob = {
        .blob_mem = VIRTGPU_BLOB_MEM_HOST3D,
        .blob_flags = VIRTGPU_BLOB_FLAG_USE_SHAREABLE |
                      VIRTGPU_BLOB_FLAG_USE_MAPPABLE,
    };
    struct drm_prime_handle prime = {0};
    struct drm_gem_close gem = {0};
    void *mapping;
    long page_size;
    int saved, ret;

    if (!buffer || (options & ~VMEDIA_GPU_ASSIGN_UUID)) {
        errno = EINVAL;
        return -1;
    }
    if ((buffer->flags & ~(O_CLOEXEC | O_ACCMODE)) ||
        ((buffer->flags & O_ACCMODE) != O_RDONLY &&
         (buffer->flags & O_ACCMODE) != O_RDWR) ||
        (buffer->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
         buffer->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < sizeof(buffer->reserved) /
                                sizeof(buffer->reserved[0]); i++) {
        if (buffer->reserved[i]) {
            errno = EINVAL;
            return -1;
        }
    }
    if (check_gpu(drm_fd, options))
        return -1;
    token.type = buffer->type;
    token.index = buffer->index;
    token.plane = buffer->plane;
    if (xioctl(video_fd, VIDIOC_VIRTIO_MEDIA_EXPORT_GPU, &token))
        return -1;
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || token.size == 0 || token.size > SIZE_MAX ||
        token.size % (uint64_t)page_size ||
        token.blob_id < UINT64_C(0xfffe000000000000) ||
        token.blob_id == UINT64_MAX) {
        errno = EPROTO;
        return -1;
    }
    blob.blob_id = token.blob_id;
    blob.size = token.size;
    if (options & VMEDIA_GPU_ASSIGN_UUID)
        blob.blob_flags |= VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE;
    if (xioctl(drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &blob))
        return -1;
    gem.handle = blob.bo_handle;
    prime.handle = blob.bo_handle;
    if (buffer->flags & O_CLOEXEC)
        prime.flags |= DRM_CLOEXEC;
    if ((buffer->flags & O_ACCMODE) == O_RDWR)
        prime.flags |= DRM_RDWR;
    ret = xioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
    if (ret)
        goto out_gem;

    /*
     * CREATE_BLOB queues host work asynchronously. A full-size mmap waits
     * for the stock driver's host mapping result and rejects a failed import.
     * No pixel access occurs here. Partial mappings are rejected by v6.14.
     */
    mapping = mmap(NULL, (size_t)token.size, PROT_READ, MAP_SHARED, prime.fd, 0);
    if (mapping == MAP_FAILED) {
        ret = -1;
        goto out_fd;
    }
    ret = munmap(mapping, (size_t)token.size);
    if (ret)
        goto out_fd;
    if (xioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gem)) {
        saved = errno;
        close(prime.fd);
        errno = saved;
        return -1;
    }
    buffer->fd = prime.fd;
    return 0;

out_fd:
    saved = errno;
    close(prime.fd);
    errno = saved;
out_gem:
    saved = errno;
    xioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gem);
    errno = saved;
    return ret;
}
