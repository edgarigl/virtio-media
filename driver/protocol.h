/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0+ */

/*
 * Definitions of virtio-media protocol structures.
 *
 * Copyright (c) 2024-2025 Google LLC.
 */

#ifndef __VIRTIO_MEDIA_PROTOCOL_H
#define __VIRTIO_MEDIA_PROTOCOL_H

#include <linux/videodev2.h>

/*
 * Virtio protocol definition.
 */

/*
 * VIRTIO_MEDIA_F_GNTREF - Device can provide Xen grant references for MMAP.
 *
 * When negotiated, VIRTIO_MEDIA_CMD_MMAP responses include grant references
 * so the guest can map buffers as normal RAM without relying on BAR mappings.
 */
#define VIRTIO_MEDIA_F_GNTREF 63

/*
 * VIRTIO_MEDIA_F_EXPORT_IMPORT - Device supports brokered export/import of
 * capture buffers through explicit commands.
 */
#define VIRTIO_MEDIA_F_EXPORT_IMPORT 62

/*
 * VIRTIO_MEDIA_F_SHARE_FENCE - Device supports explicit fence metadata for
 * shared buffers.
 *
 * This bit is currently negotiated only for forward compatibility.
 */
#define VIRTIO_MEDIA_F_SHARE_FENCE 61

/*
 * VIRTIO_MEDIA_F_PEER_GREF_IMPORT - Device can broker grant refs for a
 * specific peer Xen domid when importing a shared handle.
 */
#define VIRTIO_MEDIA_F_PEER_GREF_IMPORT 60

/**
 * struct virtio_media_cmd_header - Header for all virtio-media commands.
 * @cmd: one of VIRTIO_MEDIA_CMD_*.
 * @__reserved: must be set to zero by the driver.
 *
 * This header starts all commands from the driver to the device on the
 * commandq.
 */
struct virtio_media_cmd_header {
	u32 cmd;
	u32 __reserved;
};

/**
 * struct virtio_media_resp_header - Header for all virtio-media responses.
 * @status: 0 if the command was successful, or one of the standard Linux error
 * codes.
 * @__reserved: must be set to zero by the device.
 *
 * This header starts all responses from the device to the driver on the
 * commandq.
 */
struct virtio_media_resp_header {
	u32 status;
	u32 __reserved;
};

/**
 * VIRTIO_MEDIA_CMD_OPEN - Command for creating a new session.
 *
 * This is the equivalent of calling `open` on a V4L2 device node. Upon
 * success, a session id is returned which can be used to perform other
 * commands on the session, notably ioctls.
 */
#define VIRTIO_MEDIA_CMD_OPEN 1

/**
 * struct virtio_media_cmd_open - Driver command for VIRTIO_MEDIA_CMD_OPEN.
 * @hdr: header with cmd member set to VIRTIO_MEDIA_CMD_OPEN.
 */
struct virtio_media_cmd_open {
	struct virtio_media_cmd_header hdr;
};

/**
 * struct virtio_media_resp_open - Device response for VIRTIO_MEDIA_CMD_OPEN.
 * @hdr: header containing the status of the command.
 * @session_id: if hdr.status == 0, contains the id of the newly created session.
 * @__reserved: must be set to zero by the device.
 */
struct virtio_media_resp_open {
	struct virtio_media_resp_header hdr;
	u32 session_id;
	u32 __reserved;
};

/**
 * VIRTIO_MEDIA_CMD_CLOSE - Command for closing an active session.
 *
 * This is the equivalent of calling `close` on a previously opened V4L2
 * session. All resources associated with this session will be freed and the
 * session ID shall not be used again after queueing this command.
 *
 * This command does not require a response from the device.
 */
#define VIRTIO_MEDIA_CMD_CLOSE 2

/**
 * struct virtio_media_cmd_close - Driver command for VIRTIO_MEDIA_CMD_CLOSE.
 * @hdr: header with cmd member set to VIRTIO_MEDIA_CMD_CLOSE.
 * @session_id: id of the session to close.
 * @__reserved: must be set to zero by the driver.
 */
struct virtio_media_cmd_close {
	struct virtio_media_cmd_header hdr;
	u32 session_id;
	u32 __reserved;
};

/**
 * VIRTIO_MEDIA_CMD_IOCTL - Driver command for executing an ioctl.
 *
 * This command asks the device to run one of the `VIDIOC_*` ioctls on the
 * active session.
 *
 * The code of the ioctl is extracted from the VIDIOC_* definitions in
 * `videodev2.h`, and consists of the second argument of the `_IO*` macro.
 *
 * Each ioctl has a payload, which is defined by the third argument of the
 * `_IO*` macro defining it. It can be writable by the driver (`_IOW`), the
 * device (`_IOR`), or both (`_IOWR`).
 *
 * If an ioctl is writable by the driver, it must be followed by a
 * driver-writable descriptor containing the payload.
 *
 * If an ioctl is writable by the device, it must be followed by a
 * device-writable descriptor of the size of the payload that the device will
 * write into.
 *
 */
#define VIRTIO_MEDIA_CMD_IOCTL 3

/**
 * struct virtio_media_cmd_ioctl - Driver command for VIRTIO_MEDIA_CMD_IOCTL.
 * @hdr: header with cmd member set to VIRTIO_MEDIA_CMD_IOCTL.
 * @session_id: id of the session to run the ioctl on.
 * @code: code of the ioctl to run.
 */
struct virtio_media_cmd_ioctl {
	struct virtio_media_cmd_header hdr;
	u32 session_id;
	u32 code;
};

/**
 * struct virtio_media_resp_ioctl - Device response for VIRTIO_MEDIA_CMD_IOCTL.
 * @hdr: header containing the status of the ioctl.
 */
struct virtio_media_resp_ioctl {
	struct virtio_media_resp_header hdr;
};

/**
 * struct virtio_media_sg_entry - Description of part of a scattered guest memory.
 * @start: start guest address of the memory segment.
 * @len: length of this memory segment.
 * @__reserved: must be set to zero by the driver.
 */
struct virtio_media_sg_entry {
	u64 start;
	u32 len;
	u32 __reserved;
};

/**
 * enum virtio_media_memory - Memory types supported by virtio-media.
 * @VIRTIO_MEDIA_MMAP: memory allocated and managed by device. Can be mapped
 * into the guest using VIRTIO_MEDIA_CMD_MMAP.
 * @VIRTIO_MEDIA_SHARED_PAGES: memory allocated by the driver. Passed to the
 * device using virtio_media_sg_entry.
 * @VIRTIO_MEDIA_OBJECT: memory backed by a virtio object.
 */
enum virtio_media_memory {
	VIRTIO_MEDIA_MMAP = V4L2_MEMORY_MMAP,
	VIRTIO_MEDIA_SHARED_PAGES = V4L2_MEMORY_USERPTR,
	VIRTIO_MEDIA_OBJECT = V4L2_MEMORY_DMABUF,
};

#define VIRTIO_MEDIA_MMAP_FLAG_RW (1 << 0)

/**
 * VIRTIO_MEDIA_CMD_MMAP - Command for mapping a MMAP buffer into the driver's
 * address space.
 *
 */
#define VIRTIO_MEDIA_CMD_MMAP 4

/**
 * struct virtio_media_cmd_mmap - Driver command for VIRTIO_MEDIA_CMD_MMAP.
 * @hdr: header with cmd member set to VIRTIO_MEDIA_CMD_MMAP.
 * @session_id: ID of the session we are mapping for.
 * @flags: combination of VIRTIO_MEDIA_MMAP_FLAG_*.
 * @offset: mem_offset field of the plane to map, as returned by VIDIOC_QUERYBUF.
 */
struct virtio_media_cmd_mmap {
	struct virtio_media_cmd_header hdr;
	u32 session_id;
	u32 flags;
	u32 offset;
};

/**
 * struct virtio_media_resp_mmap - Device response for VIRTIO_MEDIA_CMD_MMAP.
 * @hdr: header containing the status of the command.
 * @driver_addr: offset into SHM region 0 of the start of the mapping.
 * @len: length of the mapping.
 * @gref_count: number of grant refs following @gref_ids (if negotiated).
 * @gref_page_size: page size used for each grant ref (bytes).
 * @gref_domid: domain ID that granted the pages (Xen backend domid).
 * @__pad: padding for alignment.
 * @gref_ids: variable-length array of grant references.
 */
struct virtio_media_resp_mmap {
	struct virtio_media_resp_header hdr;
	u64 driver_addr;
	u64 len;
	u32 gref_count;
	u32 gref_page_size;
	u32 gref_domid;
	u32 __pad;
	u32 gref_ids[0];
};

/**
 * VIRTIO_MEDIA_CMD_MUNMAP - Unmap a MMAP buffer previously mapped using
 * VIRTIO_MEDIA_CMD_MMAP.
 */
#define VIRTIO_MEDIA_CMD_MUNMAP 5

/**
 * struct virtio_media_cmd_munmap - Driver command for VIRTIO_MEDIA_CMD_MUNMAP.
 * @hdr: header with cmd member set to VIRTIO_MEDIA_CMD_MUNMAP.
 * @driver_addr: offset into SHM region 0 at which the buffer has been previously
 * mapped.
 */
struct virtio_media_cmd_munmap {
	struct virtio_media_cmd_header hdr;
	u64 driver_addr;
};

/**
 * struct virtio_media_resp_munmap - Device response for VIRTIO_MEDIA_CMD_MUNMAP.
 * @hdr: header containing the status of the command.
 */
struct virtio_media_resp_munmap {
	struct virtio_media_resp_header hdr;
};

/*
 * VIRTIO_MEDIA_CMD_EXPORT_BUFFER - Export a local capture buffer and obtain an
 * opaque handle that can be shared with another guest.
 */
#define VIRTIO_MEDIA_CMD_EXPORT_BUFFER 6

struct virtio_media_cmd_export_buffer {
	struct virtio_media_cmd_header hdr;
	u32 session_id;
	u32 queue_type;
	u32 buffer_index;
	u32 plane_index;
	u32 flags;
	u32 __reserved;
};

struct virtio_media_resp_export_buffer {
	struct virtio_media_resp_header hdr;
	u64 handle_id;
	u64 len;
	u32 plane_count;
	u32 __reserved;
};

/*
 * VIRTIO_MEDIA_CMD_IMPORT_BUFFER - Import an opaque handle previously exported
 * by another guest and retrieve mapping metadata.
 */
#define VIRTIO_MEDIA_CMD_IMPORT_BUFFER 7

struct virtio_media_cmd_import_buffer {
	struct virtio_media_cmd_header hdr;
	u32 session_id;
	u32 flags;
	u64 handle_id;
};

struct virtio_media_resp_import_buffer {
	struct virtio_media_resp_header hdr;
	u64 driver_addr;
	u64 len;
	u32 gref_count;
	u32 gref_page_size;
	u32 gref_domid;
	u32 __pad;
	u32 gref_ids[0];
};

/*
 * VIRTIO_MEDIA_CMD_RELEASE_HANDLE - Release a previously exported handle.
 */
#define VIRTIO_MEDIA_CMD_RELEASE_HANDLE 8

struct virtio_media_cmd_release_handle {
	struct virtio_media_cmd_header hdr;
	u64 handle_id;
};

struct virtio_media_resp_release_handle {
	struct virtio_media_resp_header hdr;
};

#define VIRTIO_MEDIA_EVT_ERROR 0
#define VIRTIO_MEDIA_EVT_DQBUF 1
#define VIRTIO_MEDIA_EVT_EVENT 2

/**
 * struct virtio_media_event_header - Header for events on the eventq.
 * @event: one of VIRTIO_MEDIA_EVT_*
 * @session_id: ID of the session the event applies to.
 */
struct virtio_media_event_header {
	u32 event;
	u32 session_id;
};

/**
 * struct virtio_media_event_error - Unrecoverable device-side error.
 * @hdr: header for the event.
 * @errno: error code describing the kind of error that occurred.
 * @__reserved: must to set to zero by the device.
 *
 * Upon receiving this event, the session mentioned in the header is considered
 * corrupted and closed.
 *
 */
struct virtio_media_event_error {
	struct virtio_media_event_header hdr;
	u32 errno;
	u32 __reserved;
};

#define VIRTIO_MEDIA_MAX_PLANES VIDEO_MAX_PLANES

/**
 * struct virtio_media_event_dqbuf - Dequeued buffer event.
 * @hdr: header for the event.
 * @buffer: struct v4l2_buffer describing the buffer that has been dequeued.
 * @planes: plane information for the dequeued buffer.
 *
 * This event is used to signal that a buffer is not being used anymore by the
 * device and is returned to the driver.
 */
struct virtio_media_event_dqbuf {
	struct virtio_media_event_header hdr;
	struct v4l2_buffer buffer;
	struct v4l2_plane planes[VIRTIO_MEDIA_MAX_PLANES];
};

/**
 * struct virtio_media_event_event - V4L2 event.
 * @hdr: header for the event.
 * @event: description of the event that occurred.
 *
 * This event signals that a V4L2 event has been emitted for a session.
 */
struct virtio_media_event_event {
	struct virtio_media_event_header hdr;
	struct v4l2_event event;
};

/*
 * Userspace prototype ioctls for brokered sharing.
 *
 * These commands are private to this out-of-tree driver.
 */
#define VIRTIO_MEDIA_MAX_IMPORT_GREFS 512

struct virtio_media_ioc_export_buffer {
	u32 queue_type;
	u32 buffer_index;
	u32 plane_index;
	u32 flags;
	u64 handle_id;
	u64 len;
	u32 plane_count;
	s32 dmabuf_fd;
};

struct virtio_media_ioc_import_buffer {
	u64 handle_id;
	u32 flags;
	u32 gref_count;
	u32 gref_page_size;
	u32 gref_domid;
	u32 __reserved;
	u64 driver_addr;
	u64 len;
	s32 dmabuf_fd;
	u32 __pad;
	u32 gref_ids[VIRTIO_MEDIA_MAX_IMPORT_GREFS];
};

/*
 * When set by userspace on VIDIOC_VIRTIO_MEDIA_IMPORT_BUFFER, import grefs
 * directly from the ioctl payload instead of resolving handle_id through QEMU.
 * This is used for cross-guest sharing where handle namespaces differ.
 */
#define VIRTIO_MEDIA_IMPORT_F_DIRECT_GREFS (1U << 0)
/*
 * Request QEMU to return grant refs targeted to a peer domid encoded in the
 * import flags. This path still sends VIRTIO_MEDIA_CMD_IMPORT_BUFFER.
 */
#define VIRTIO_MEDIA_IMPORT_F_TARGET_DOMID (1U << 1)
#define VIRTIO_MEDIA_IMPORT_DOMID_SHIFT 16
#define VIRTIO_MEDIA_IMPORT_DOMID_MASK 0xffffU

struct virtio_media_ioc_release_handle {
	u64 handle_id;
};

#define VIDIOC_VIRTIO_MEDIA_EXPORT_BUFFER \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 0, \
	      struct virtio_media_ioc_export_buffer)
#define VIDIOC_VIRTIO_MEDIA_IMPORT_BUFFER \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 1, \
	      struct virtio_media_ioc_import_buffer)
#define VIDIOC_VIRTIO_MEDIA_RELEASE_HANDLE \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 2, \
	      struct virtio_media_ioc_release_handle)

/* Maximum size of an event. We will queue descriptors of this size on the eventq. */
#define VIRTIO_MEDIA_EVENT_MAX_SIZE sizeof(struct virtio_media_event_dqbuf)

#endif // __VIRTIO_MEDIA_PROTOCOL_H
