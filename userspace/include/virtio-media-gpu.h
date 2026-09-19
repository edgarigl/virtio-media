/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef VIRTIO_MEDIA_GPU_H
#define VIRTIO_MEDIA_GPU_H

#include <stdint.h>
#include <time.h>
#include <linux/videodev2.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMEDIA_GPU_ASSIGN_UUID (1U << 0)

/*
 * Export a capture slot through the stock virtio-gpu DRM interface.
 * video_fd and drm_fd are borrowed, never closed. The caller selects the GPU.
 * buffer: type/index/plane/flags as for EXPBUF; zero reserved fields.
 * Supported fd flags: O_RDONLY or O_RDWR, optionally O_CLOEXEC.
 * options: zero or VMEDIA_GPU_ASSIGN_UUID (requires CROSS_DEVICE support).
 * Returns 0 and sets buffer->fd, or -1 with errno; leaves buffer unchanged
 * on error. The caller owns the returned fd.
 *
 * Requires the local media export-info ioctl and QEMU external-media importer.
 * UUID assignment does not prove common-UUID support on the host.
 * Calls may block in the kernel. Serialize against media queue teardown and
 * closing either input fd. Complete GPU work before requeue/recovery.
 * No enforced read-only GPU access is implied by O_RDONLY.
 */
int vmedia_gpu_expbuf(int video_fd, int drm_fd,
                     struct v4l2_exportbuffer *buffer, uint32_t options);

#ifdef __cplusplus
}
#endif
#endif
