/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_VIRTIO_MEDIA_GPU_H
#define _UAPI_LINUX_VIRTIO_MEDIA_GPU_H

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/videodev2.h>

/*
 * Local virtio-media extension, not an upstream virtio or V4L2 interface.
 * Input: version, type, index, plane. Zero flags and reserved fields.
 * Output: blob_id and size for the selected, already allocated capture slot.
 * The token is local to this QEMU and expires when the media queue is released.
 * Serialize this operation and GPU resource creation against queue teardown.
 * It neither dequeues a frame nor grants read-only access.
 */
#define VIRTIO_MEDIA_GPU_EXPORT_VERSION 1
struct virtio_media_gpu_export {
	__u32 version;
	__u32 type;
	__u32 index;
	__u32 plane;
	__u32 flags;
	__u32 reserved[3];
	__aligned_u64 blob_id;
	__aligned_u64 size;
};

#define VIDIOC_VIRTIO_MEDIA_EXPORT_GPU \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 3, struct virtio_media_gpu_export)

#endif
