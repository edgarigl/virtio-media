// SPDX-License-Identifier: GPL-2.0-only
/* Optional virtio-gpu helper for the experimental virtio-media GPU export. */
#include <linux/dma-buf.h>
#include <linux/module.h>
#include <drm/drm_drv.h>
#include <drm/drm_prime.h>
#include "virtgpu_drv.h"

struct dma_buf *virtio_gpu_export_host_blob(struct virtio_device *vdev,
					   u64 blob_id, u64 size, int flags);

struct dma_buf *virtio_gpu_export_host_blob(struct virtio_device *vdev,
					   u64 blob_id, u64 size, int flags)
{
	struct virtio_gpu_object_params params = {
		.blob = true,
		.blob_mem = VIRTGPU_BLOB_MEM_HOST3D,
		.blob_flags = VIRTGPU_BLOB_FLAG_USE_SHAREABLE |
			      VIRTGPU_BLOB_FLAG_USE_MAPPABLE,
		.blob_id = blob_id,
		.size = size,
	};
	struct virtio_gpu_device *vgdev;
	struct virtio_gpu_object *bo;
	struct drm_device *dev;
	struct dma_buf *dbuf;
	int ret, idx;

	if (!size || !IS_ALIGNED(size, PAGE_SIZE) || size > SIZE_MAX ||
	    blob_id < 0xfffe000000000000ULL || vdev->id.device != VIRTIO_ID_GPU)
		return ERR_PTR(-EINVAL);
	device_lock(&vdev->dev);
	dev = vdev->priv;
	if (!vdev->dev.driver || !dev || !drm_dev_enter(dev, &idx)) {
		dbuf = ERR_PTR(-ENODEV);
		goto out_unlock;
	}
	vgdev = dev->dev_private;
	if (!vgdev->has_resource_blob || !vgdev->has_virgl_3d) {
		dbuf = ERR_PTR(-EOPNOTSUPP);
		goto out_exit;
	}
	/* QEMU imports the fd without a renderer context; consumers attach later. */
	ret = virtio_gpu_vram_create(vgdev, &params, &bo);
	if (ret) {
		dbuf = ERR_PTR(ret);
		goto out_exit;
	}
	bo->host3d_blob = true;
	bo->blob_mem = params.blob_mem;
	bo->blob_flags = params.blob_flags;
	dbuf = virtgpu_gem_prime_export(&bo->base.base, flags);
	drm_gem_object_put(&bo->base.base);
	virtio_gpu_notify(vgdev);
out_exit:
	drm_dev_exit(idx);
out_unlock:
	device_unlock(&vdev->dev);
	return dbuf;
}
EXPORT_SYMBOL_GPL(virtio_gpu_export_host_blob);
