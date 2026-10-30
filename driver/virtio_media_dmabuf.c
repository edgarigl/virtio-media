// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0+
/* Optional local DMA-BUF backing; remote mapping belongs to the provider. */
#include <linux/dma-buf.h>
#include "virtio_media.h"
#include "session.h"
#ifdef VIRTIO_MEDIA_SHM_EXPORT
#include <linux/virtio_shm_export.h>
#endif

bool virtio_media_can_import(struct virtio_media *vv)
{
#ifdef VIRTIO_MEDIA_SHM_EXPORT
	return virtio_shm_can_import(vv->virtio_dev, 0);
#else
	return false;
#endif
}

void virtio_media_complete_imports(struct virtio_media_buffer *buffer, int error)
{
#ifdef VIRTIO_MEDIA_SHM_EXPORT
	unsigned int i;

	for (i = 0; i < VIDEO_MAX_PLANES; i++)
		if (buffer->imports[i])
			buffer->imports[i]->complete(buffer->imports[i], error);
#endif
}

void virtio_media_release_imports(struct virtio_media_buffer *buffer)
{
#ifdef VIRTIO_MEDIA_SHM_EXPORT
	unsigned int i;

	for (i = 0; i < VIDEO_MAX_PLANES; i++) {
		if (buffer->imports[i]) {
			/* The provider retains failed revocations, including fences. */
			buffer->imports[i]->release(buffer->imports[i]);
			buffer->imports[i] = NULL;
		}
	}
#endif
}

int virtio_media_prepare_imports(struct virtio_media *vv,
		struct virtio_media_buffer *buffer, struct v4l2_buffer *b)
{
#ifdef VIRTIO_MEDIA_SHM_EXPORT
	unsigned int count = V4L2_TYPE_IS_MULTIPLANAR(b->type) ? b->length : 1;
	unsigned int i, j;
	int ret;

	if (!V4L2_TYPE_IS_CAPTURE(b->type) || !count || count > VIDEO_MAX_PLANES)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		bool multi = V4L2_TYPE_IS_MULTIPLANAR(b->type);
		int fd = multi ? b->m.planes[i].m.fd : b->m.fd;
		u64 length = multi ? b->m.planes[i].length : b->length;
		struct dma_buf *dbuf;
		struct virtio_shm_import *import = buffer->imports[i];

		if (!length || (multi && b->m.planes[i].data_offset)) {
			ret = -EINVAL;
			goto fail;
		}
		dbuf = dma_buf_get(fd);
		if (IS_ERR(dbuf)) {
			ret = PTR_ERR(dbuf);
			goto fail;
		}
		/* One independent reservation per plane in this version. */
		for (j = 0; j < i; j++) {
			if (buffer->imports[j]->buffer == dbuf) {
				dma_buf_put(dbuf);
				ret = -EINVAL;
				goto fail;
			}
		}
		if (import && (import->buffer != dbuf || import->length != length)) {
			ret = import->release(import);
			buffer->imports[i] = NULL;
			import = NULL;
			if (ret) {
				dma_buf_put(dbuf);
				goto fail;
			}
		}
		if (!import)
			import = virtio_shm_import(vv->virtio_dev, 0, dbuf, length);
		dma_buf_put(dbuf);
		if (IS_ERR(import)) {
			ret = PTR_ERR(import);
			goto fail;
		}
		buffer->imports[i] = import;
		memcpy(buffer->uuids[i], import->uuid, 16);
	}
	for (i = 0; i < count; i++) {
		ret = buffer->imports[i]->prepare(buffer->imports[i]);
		if (ret) {
			while (i--)
				buffer->imports[i]->complete(buffer->imports[i], -ECANCELED);
			return ret;
		}
	}
	return 0;
fail:
	virtio_media_release_imports(buffer);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}
