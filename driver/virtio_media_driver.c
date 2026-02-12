// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0+

/*
 * Virtio-media driver.
 *
 * Copyright (c) 2024-2025 Google LLC.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <xen/grant_table.h>

#include <media/frame_vector.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-memops.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>

#include "protocol.h"
#include "session.h"
#include "virtio_media.h"

#ifndef VIRTIO_ID_MEDIA
#define VIRTIO_ID_MEDIA 48
#endif

#define VIRTIO_MEDIA_NUM_EVENT_BUFS 16

/* ID of the SHM region into which MMAP buffer will be mapped. */
#define VIRTIO_MEDIA_SHM_MMAP 0

/*
 * Name of the driver to expose to user-space.
 *
 * This is configurable because v4l2-compliance has workarounds specific to
 * some drivers. When proxying these directly from the host, this allows it to
 * apply them as needed.
 */
char *virtio_media_driver_name;
module_param_named(driver_name, virtio_media_driver_name, charp, 0660);

/*
 * Whether USERPTR buffers are allowed.
 *
 * This is disabled by default as USERPTR buffers are dangerous, but the option
 * is left to enable them if desired.
 */
bool virtio_media_allow_userptr;
module_param_named(allow_userptr, virtio_media_allow_userptr, bool, 0660);

/**
 * virtio_media_session_alloc - Allocate a new session.
 * @vv: virtio-media device the session belongs to.
 * @id: ID of the session.
 * @nonblocking_dequeue: whether dequeuing of buffers should be blocking or
 * not.
 *
 * The ``id`` and ``list`` fields must still be set by the caller.
 */
static struct virtio_media_session *
virtio_media_session_alloc(struct virtio_media *vv, u32 id,
			   struct file *file)
{
	struct virtio_media_session *session;
	int i;
	int ret;

	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session)
		goto err_session;

	session->shadow_buf = kzalloc(VIRTIO_SHADOW_BUF_SIZE, GFP_KERNEL);
	if (!session->shadow_buf)
		goto err_shadow_buf;

	ret = sg_alloc_table(&session->command_sgs, DESC_CHAIN_MAX_LEN,
			     GFP_KERNEL);
	if (ret)
		goto err_payload_sgs;

	session->id = id;
	session->nonblocking_dequeue = file->f_flags & O_NONBLOCK;

	INIT_LIST_HEAD(&session->list);
	v4l2_fh_init(&session->fh, &vv->video_dev);
	virtio_media_session_fh_add(session, file);

	for (i = 0; i <= VIRTIO_MEDIA_LAST_QUEUE; i++)
		INIT_LIST_HEAD(&session->queues[i].pending_dqbufs);
	mutex_init(&session->queues_lock);

	init_waitqueue_head(&session->dqbuf_wait);

	mutex_lock(&vv->sessions_lock);
	list_add_tail(&session->list, &vv->sessions);
	mutex_unlock(&vv->sessions_lock);

	return session;

err_payload_sgs:
	kfree(session->shadow_buf);
err_shadow_buf:
	kfree(session);
err_session:
	return ERR_PTR(-ENOMEM);
}

/**
 * virtio_media_session_free - Free all resources of a session.
 * @vv: virtio-media device the session belongs to.
 * @session: session to destroy.
 *
 * All the resources of @sesssion, as well as the backing memory of @session
 * itself, are freed.
 */
static void virtio_media_session_free(struct virtio_media *vv,
				      struct virtio_media_session *session)
{
	int i;

	mutex_lock(&vv->sessions_lock);
	list_del(&session->list);
	mutex_unlock(&vv->sessions_lock);

	virtio_media_session_fh_del(session);
	v4l2_fh_exit(&session->fh);

	sg_free_table(&session->command_sgs);

	for (i = 0; i <= VIRTIO_MEDIA_LAST_QUEUE; i++)
		vfree(session->queues[i].buffers);

	kfree(session->shadow_buf);
	kfree(session);
}

/**
 * virtio_media_session_close - Close and free a session.
 * @vv: virtio-media device the session belongs to.
 * @session: session to close and destroy.
 *
 * This send the ``VIRTIO_MEDIA_CMD_CLOSE`` command to the device, and frees
 * all resources used by @session.
 */
static int virtio_media_session_close(struct virtio_media *vv,
				      struct virtio_media_session *session)
{
	struct virtio_media_cmd_close *cmd_close = &session->cmd.close;
	struct scatterlist cmd_sg = {};
	struct scatterlist *sgs[1] = { &cmd_sg };
	int ret;

	mutex_lock(&vv->vlock);

	cmd_close->hdr.cmd = VIRTIO_MEDIA_CMD_CLOSE;
	cmd_close->session_id = session->id;

	sg_set_buf(&cmd_sg, cmd_close, sizeof(*cmd_close));
	sg_mark_end(&cmd_sg);

	ret = virtio_media_send_command(vv, sgs, 1, 0, 0, NULL);
	mutex_unlock(&vv->vlock);
	if (ret < 0)
		return ret;

	virtio_media_session_free(vv, session);

	return 0;
}

/**
 * virtio_media_find_session - Lookup for the session with a given ID.
 * @vv: virtio-media device to lookup the session from.
 * @id: ID of the session to lookup.
 */
static struct virtio_media_session *
virtio_media_find_session(struct virtio_media *vv, u32 id)
{
	struct list_head *p;
	struct virtio_media_session *session = NULL;

	mutex_lock(&vv->sessions_lock);
	list_for_each(p, &vv->sessions) {
		struct virtio_media_session *s =
			list_entry(p, struct virtio_media_session, list);
		if (s->id == id) {
			session = s;
			break;
		}
	}
	mutex_unlock(&vv->sessions_lock);

	return session;
}

/**
 * struct virtio_media_cmd_callback_param - Callback parameters to the virtio command queue.
 * @vv: virtio-media device in use.
 * @done: flag to be switched once the command is completed.
 * @resp_len: length of the received response from the command. Only valid
 * after @done_flag has switched to ``true``.
 */
struct virtio_media_cmd_callback_param {
	struct virtio_media *vv;
	bool done;
	size_t resp_len;
};

/**
 * commandq_callback: Callback for the command queue.
 * @queue: command virtqueue.
 *
 * This just wakes up the thread that was waiting on the command to complete.
 */
static void commandq_callback(struct virtqueue *queue)
{
	unsigned int len;
	struct virtio_media_cmd_callback_param *param;

process_bufs:
	while ((param = virtqueue_get_buf(queue, &len))) {
		param->done = true;
		param->resp_len = len;
		wake_up(&param->vv->wq);
	}

	if (!virtqueue_enable_cb(queue)) {
		virtqueue_disable_cb(queue);
		goto process_bufs;
	}
}

/**
 * virtio_media_kick_command - send a command to the commandq.
 * @vv: virtio-media device in use.
 * @sgs: descriptor chain to send.
 * @out_sgs: number of device-readable descriptors in @sgs.
 * @in_sgs: number of device-writable descriptors in @sgs.
 * @resp_len: output parameter. Upon success, contains the size of the response
 * in bytes.
 *
 */
static int virtio_media_kick_command(struct virtio_media *vv,
				     struct scatterlist **sgs,
				     const size_t out_sgs, const size_t in_sgs,
				     size_t *resp_len)
{
	struct virtio_media_cmd_callback_param cb_param = {
		.vv = vv,
		.done = false,
		.resp_len = 0,
	};
	struct virtio_media_resp_header *resp_header;
	int ret;

	ret = virtqueue_add_sgs(vv->commandq, sgs, out_sgs, in_sgs, &cb_param,
				GFP_ATOMIC);
	if (ret) {
		v4l2_err(&vv->v4l2_dev,
			 "failed to add sgs to command virtqueue\n");
		return ret;
	}

	if (!virtqueue_kick(vv->commandq)) {
		v4l2_err(&vv->v4l2_dev, "failed to kick command virtqueue\n");
		return -EINVAL;
	}

	/* Wait for the response. */
	ret = wait_event_timeout(vv->wq, cb_param.done, 5 * HZ);
	if (ret == 0) {
		v4l2_err(&vv->v4l2_dev,
			 "timed out waiting for response to command\n");
		return -ETIMEDOUT;
	}

	if (resp_len)
		*resp_len = cb_param.resp_len;

	if (in_sgs > 0) {
		/*
		 * If we expect a response, make sure we have at least a
		 * response header - anything shorter is invalid.
		 */
		if (cb_param.resp_len < sizeof(*resp_header)) {
			v4l2_err(&vv->v4l2_dev,
				 "received response header is too short\n");
			return -EINVAL;
		}

		resp_header = sg_virt(sgs[out_sgs]);
		if (resp_header->status)
			/* Host returns a positive error code. */
			return -resp_header->status;
	}

	return 0;
}

/**
 * virtio_media_send_command - Send a command to the device and wait for its
 * response.
 * @vv: virtio-media device in use.
 * @sgs: descriptor chain to send.
 * @out_sgs: number of device-readable descriptors in @sgs.
 * @in_sgs: number of device-writable descriptors in @sgs.
 * @minimum_resp_len: minimum length of the response expected by the caller
 * when the command is successful. Anything shorter than that will result in
 * ``-EINVAL`` being returned.
 * @resp_len: output parameter. Upon success, contains the size of the response
 * in bytes.
 */
int virtio_media_send_command(struct virtio_media *vv, struct scatterlist **sgs,
			      const size_t out_sgs, const size_t in_sgs,
			      size_t minimum_resp_len, size_t *resp_len)
{
	size_t local_resp_len = resp_len ? *resp_len : 0;
	int ret = virtio_media_kick_command(vv, sgs, out_sgs, in_sgs,
					    &local_resp_len);
	if (resp_len)
		*resp_len = local_resp_len;

	/* If the host could not process the command, there is no valid response */
	if (ret < 0)
		return ret;

	/* Make sure the host wrote a complete reply. */
	if (local_resp_len < minimum_resp_len) {
		v4l2_err(
			&vv->v4l2_dev,
			"received response is too short: received %zu, expected at least %zu\n",
			local_resp_len, minimum_resp_len);
		return -EINVAL;
	}

	return 0;
}

/**
 * virtio_media_send_event_buffer() - Sends an event buffer to the host so it
 * can return it with an event.
 * @vv: virtio-media device in use.
 * @event_buffer: pointer to the event buffer to send to the device.
 */
static int virtio_media_send_event_buffer(struct virtio_media *vv,
					  void *event_buffer)
{
	struct scatterlist *sgs[1], vresp;
	int ret;

	sg_init_one(&vresp, event_buffer, VIRTIO_MEDIA_EVENT_MAX_SIZE);
	sgs[0] = &vresp;

	ret = virtqueue_add_sgs(vv->eventq, sgs, 0, 1, event_buffer,
				GFP_ATOMIC);
	if (ret) {
		v4l2_err(&vv->v4l2_dev,
			 "failed to add sgs to event virtqueue\n");
		return ret;
	}

	if (!virtqueue_kick(vv->eventq)) {
		v4l2_err(&vv->v4l2_dev, "failed to kick event virtqueue\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * eventq_callback() - Callback for the event queue.
 * @queue: event virtqueue.
 *
 * This just schedules for event work to be run.
 */
static void eventq_callback(struct virtqueue *queue)
{
	struct virtio_media *vv = queue->vdev->priv;
	static unsigned int evt_cb_log;

	if ((evt_cb_log++ < 20) || (evt_cb_log % 5000) == 0)
		v4l2_info(&vv->v4l2_dev, "eventq_callback\n");
	schedule_work(&vv->eventq_work);
}

/**
 * virtio_media_process_dqbuf_event() - Process a dequeued event for a session.
 * @vv: virtio-media device in use.
 * @session: session the event is addressed to.
 * @dqbuf_evt: the dequeued event to process.
 *
 * Invalid events are ignored with an error log.
 */
static void
virtio_media_process_dqbuf_event(struct virtio_media *vv,
				 struct virtio_media_session *session,
				 struct virtio_media_event_dqbuf *dqbuf_evt)
{
	static unsigned int dqbuf_evt_log;
	struct virtio_media_buffer *dqbuf;
	const enum v4l2_buf_type queue_type = dqbuf_evt->buffer.type;
	struct virtio_media_queue_state *queue;
	typeof(dqbuf->buffer.m) buffer_m;
	typeof(dqbuf->buffer.m.planes[0].m) plane_m;
	int i;

	if (queue_type >= ARRAY_SIZE(session->queues)) {
		v4l2_err(&vv->v4l2_dev,
			 "unmanaged queue %d passed to dqbuf event",
			 dqbuf_evt->buffer.type);
		return;
	}
	queue = &session->queues[queue_type];

	if (dqbuf_evt->buffer.index >= queue->allocated_bufs) {
		v4l2_err(&vv->v4l2_dev,
			 "invalid buffer ID %d for queue %d in dqbuf event",
			 dqbuf_evt->buffer.index, dqbuf_evt->buffer.type);
		return;
	}

	dqbuf = &queue->buffers[dqbuf_evt->buffer.index];
	if ((dqbuf_evt_log++ < 20) || (dqbuf_evt_log % 5000) == 0)
		v4l2_info(&vv->v4l2_dev,
			  "dqbuf event: session=%u type=%u idx=%u queued_bufs=%zu\n",
			  session->id, dqbuf_evt->buffer.type,
			  dqbuf_evt->buffer.index, queue->queued_bufs);

	/*
	 * Preserve the 'm' union that was passed to us during QBUF so userspace
	 * gets back the information it submitted.
	 */
	buffer_m = dqbuf->buffer.m;
	memcpy(&dqbuf->buffer, &dqbuf_evt->buffer, sizeof(dqbuf->buffer));
	dqbuf->buffer.m = buffer_m;
	if (V4L2_TYPE_IS_MULTIPLANAR(dqbuf->buffer.type)) {
		if (dqbuf->buffer.length > VIDEO_MAX_PLANES) {
			v4l2_err(
				&vv->v4l2_dev,
				"invalid number of planes received from host for a multiplanar buffer\n");
			return;
		}
		for (i = 0; i < dqbuf->buffer.length; i++) {
			plane_m = dqbuf->planes[i].m;
			memcpy(&dqbuf->planes[i], &dqbuf_evt->planes[i],
			       sizeof(struct v4l2_plane));
			dqbuf->planes[i].m = plane_m;
		}
	}

	/* Set the DONE flag as the buffer is waiting for being dequeued. */
	dqbuf->buffer.flags |= V4L2_BUF_FLAG_DONE;

	mutex_lock(&session->queues_lock);
	if (!list_empty(&dqbuf->list)) {
		v4l2_warn(&vv->v4l2_dev,
			  "dqbuf event: buffer already pending (session=%u type=%u idx=%u)\n",
			  session->id, dqbuf_evt->buffer.type,
			  dqbuf_evt->buffer.index);
		mutex_unlock(&session->queues_lock);
		return;
	}
	list_add_tail(&dqbuf->list, &queue->pending_dqbufs);
	if (queue->queued_bufs == 0) {
		v4l2_warn(&vv->v4l2_dev,
			  "dqbuf event: queued_bufs already 0 (session=%u type=%u idx=%u)\n",
			  session->id, dqbuf_evt->buffer.type,
			  dqbuf_evt->buffer.index);
	} else {
		queue->queued_bufs -= 1;
	}
	mutex_unlock(&session->queues_lock);

	wake_up(&session->dqbuf_wait);
}

/**
 * virtio_media_process_events() - Process all pending events on a device.
 * @vv: device which pending events we want to process.
 *
 * Retrieves all pending events on @vv's event queue and dispatch them to their
 * corresponding session.
 *
 * Invalid events are ignored with an error log.
 */
void virtio_media_process_events(struct virtio_media *vv)
{
	static unsigned int evt_log;
	struct virtio_media_event_error *error_evt;
	struct virtio_media_event_dqbuf *dqbuf_evt;
	struct virtio_media_event_event *event_evt;
	struct virtio_media_session *session;
	struct virtio_media_event_header *evt;
	unsigned int len;

	mutex_lock(&vv->events_lock);

process_bufs:
	while ((evt = virtqueue_get_buf(vv->eventq, &len))) {
		if ((evt_log++ < 20) || (evt_log % 5000) == 0)
			v4l2_info(&vv->v4l2_dev,
				  "eventq buf len=%u event=%u session=%u\n",
				  len, evt->event, evt->session_id);
		/* Make sure we received enough data */
		if (len < sizeof(*evt)) {
			v4l2_err(
				&vv->v4l2_dev,
				"event is too short: got %u, expected at least %zu\n",
				len, sizeof(*evt));
			goto end_of_event;
		}

		session = virtio_media_find_session(vv, evt->session_id);
		if (!session) {
			v4l2_err(&vv->v4l2_dev, "cannot find session %d\n",
				 evt->session_id);
			goto end_of_event;
		}

		switch (evt->event) {
		case VIRTIO_MEDIA_EVT_ERROR:
			if (len < sizeof(*error_evt)) {
				v4l2_err(
					&vv->v4l2_dev,
					"error event is too short: got %u, expected %zu\n",
					len, sizeof(*error_evt));
				break;
			}
			error_evt = (struct virtio_media_event_error *)evt;
			v4l2_err(&vv->v4l2_dev,
				 "received error %d for session %d",
				 error_evt->errno, error_evt->hdr.session_id);
			virtio_media_session_close(vv, session);
			break;

		/*
		 * Dequeued buffer: put it into the right queue so user-space can dequeue
		 * it.
		 */
		case VIRTIO_MEDIA_EVT_DQBUF:
			if (len < sizeof(*dqbuf_evt)) {
				v4l2_err(
					&vv->v4l2_dev,
					"dqbuf event is too short: got %u, expected %zu\n",
					len, sizeof(*dqbuf_evt));
				break;
			}
			dqbuf_evt = (struct virtio_media_event_dqbuf *)evt;
			virtio_media_process_dqbuf_event(vv, session,
							 dqbuf_evt);
			break;

		case VIRTIO_MEDIA_EVT_EVENT:
			if (len < sizeof(*event_evt)) {
				v4l2_err(
					&vv->v4l2_dev,
					"session event is too short: got %u expected %zu\n",
					len, sizeof(*event_evt));
				break;
			}

			event_evt = (struct virtio_media_event_event *)evt;
			v4l2_event_queue_fh(&session->fh, &event_evt->event);
			break;

		default:
			v4l2_err(&vv->v4l2_dev, "unknown event type %d\n",
				 evt->event);
			break;
		}

end_of_event:
		virtio_media_send_event_buffer(vv, evt);
	}

	if (!virtqueue_enable_cb(vv->eventq)) {
		virtqueue_disable_cb(vv->eventq);
		goto process_bufs;
	}

	mutex_unlock(&vv->events_lock);
}

static void virtio_media_event_work(struct work_struct *work)
{
	struct virtio_media *vv =
		container_of(work, struct virtio_media, eventq_work);

	virtio_media_process_events(vv);
}

/**
 * virtio_media_device_open() - Create a new session from an opened file.
 * @file: opened file for the session.
 */
static int virtio_media_device_open(struct file *file)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtio_media *vv = to_virtio_media(video_dev);
	struct virtio_media_cmd_open *cmd_open = &vv->cmd.open;
	struct virtio_media_resp_open *resp_open = &vv->resp.open;
	struct scatterlist cmd_sg = {}, resp_sg = {};
	struct scatterlist *sgs[2] = { &cmd_sg, &resp_sg };
	struct virtio_media_session *session;
	u32 session_id;
	int ret;

	mutex_lock(&vv->vlock);

	sg_set_buf(&cmd_sg, cmd_open, sizeof(*cmd_open));
	sg_mark_end(&cmd_sg);

	sg_set_buf(&resp_sg, resp_open, sizeof(*resp_open));
	sg_mark_end(&resp_sg);

	cmd_open->hdr.cmd = VIRTIO_MEDIA_CMD_OPEN;
	ret = virtio_media_send_command(vv, sgs, 1, 1, sizeof(*resp_open),
					NULL);
	session_id = resp_open->session_id;
	mutex_unlock(&vv->vlock);
	if (ret < 0)
		return ret;

	session = virtio_media_session_alloc(vv, session_id, file);
	if (IS_ERR(session))
		return PTR_ERR(session);

	file->private_data = &session->fh;

	return 0;
}

/**
 * virtio_media_device_close() - Close a previously opened session.
 * @file: file of the session to close.
 *
 * This sends to ``VIRTIO_MEDIA_CMD_CLOSE`` command to the device, and close
 * the session on the driver side.
 */
static int virtio_media_device_close(struct file *file)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtio_media *vv = to_virtio_media(video_dev);
	struct virtio_media_session *session =
		fh_to_session(file->private_data);

	return virtio_media_session_close(vv, session);
}

/**
 * virtio_media_device_poll() - Poll logic for a virtio-media device.
 * @file: file of the session to poll.
 * @wait: poll table to wait on.
 */
static __poll_t virtio_media_device_poll(struct file *file, poll_table *wait)
{
	struct virtio_media_session *session =
		fh_to_session(file->private_data);
	enum v4l2_buf_type capture_type =
		session->uses_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
				       V4L2_BUF_TYPE_VIDEO_CAPTURE;
	enum v4l2_buf_type output_type =
		session->uses_mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
				       V4L2_BUF_TYPE_VIDEO_OUTPUT;
	struct virtio_media_queue_state *capture_queue =
		&session->queues[capture_type];
	struct virtio_media_queue_state *output_queue =
		&session->queues[output_type];
	__poll_t req_events = poll_requested_events(wait);
	__poll_t rc = 0;
	static unsigned int poll_log;

	poll_wait(file, &session->dqbuf_wait, wait);
	poll_wait(file, &session->fh.wait, wait);

	mutex_lock(&session->queues_lock);
	if (req_events & (EPOLLIN | EPOLLRDNORM)) {
		if (!capture_queue->streaming)
			rc |= EPOLLERR;
		else if (!list_empty(&capture_queue->pending_dqbufs))
			rc |= EPOLLIN | EPOLLRDNORM;
	}
	if (req_events & (EPOLLOUT | EPOLLWRNORM)) {
		if (!output_queue->streaming)
			rc |= EPOLLERR;
		else if (output_queue->queued_bufs <
			 output_queue->allocated_bufs)
			rc |= EPOLLOUT | EPOLLWRNORM;
	}
	mutex_unlock(&session->queues_lock);

	if (v4l2_event_pending(&session->fh))
		rc |= EPOLLPRI;

	if ((poll_log++ < 20) || (poll_log % 5000) == 0)
		v4l2_info(session->fh.vdev->v4l2_dev,
			  "poll: rc=0x%x cap_stream=%d cap_queued=%zu cap_pending=%d\n",
			  rc, capture_queue->streaming,
			  capture_queue->queued_bufs,
			  !list_empty(&capture_queue->pending_dqbufs));

	return rc;
}

struct virtio_media_gref_mapping {
	struct page **pages;
	grant_handle_t *handles;
	unsigned int count;
	domid_t domid;
};

struct virtio_media_vma {
	struct virtio_media *vv;
	struct virtio_media_session *session;
	struct virtio_media_gref_mapping *gref;
	u64 driver_addr;
};

static void virtio_media_vma_close_locked(struct vm_area_struct *vma)
{
	struct virtio_media_vma *vma_data = vma->vm_private_data;
	struct virtio_media *vv = vma_data->vv;
	struct virtio_media_cmd_munmap *cmd_munmap = &vv->cmd.munmap;
	struct virtio_media_resp_munmap *resp_munmap = &vv->resp.munmap;
	struct scatterlist cmd_sg = {}, resp_sg = {};
	struct scatterlist *sgs[2] = { &cmd_sg, &resp_sg };
	struct virtio_media_gref_mapping *gref = vma_data->gref;
	int ret;

	sg_set_buf(&cmd_sg, cmd_munmap, sizeof(*cmd_munmap));
	sg_mark_end(&cmd_sg);

	sg_set_buf(&resp_sg, resp_munmap, sizeof(*resp_munmap));
	sg_mark_end(&resp_sg);

	cmd_munmap->hdr.cmd = VIRTIO_MEDIA_CMD_MUNMAP;
	cmd_munmap->driver_addr = vma_data->driver_addr;
	ret = virtio_media_send_command(vv, sgs, 1, 1, sizeof(*resp_munmap),
					NULL);
	if (ret < 0) {
		v4l2_err(&vv->v4l2_dev, "host failed to unmap buffer: %d\n",
			 ret);
	}

	if (gref) {
		struct gnttab_unmap_grant_ref *unmap_ops;
		unsigned int i;

		unmap_ops = kcalloc(gref->count, sizeof(*unmap_ops),
				    GFP_KERNEL);
			if (unmap_ops) {
				for (i = 0; i < gref->count; i++) {
					if (gref->handles[i] ==
					    INVALID_GRANT_HANDLE) {
						continue;
					}
					gnttab_set_unmap_op(
						&unmap_ops[i],
						(phys_addr_t)page_address(
							gref->pages[i]),
						GNTMAP_host_map, gref->handles[i]);
			}
			gnttab_unmap_refs(unmap_ops, NULL, gref->pages,
					  gref->count);
			kfree(unmap_ops);
		} else {
			v4l2_err(&vv->v4l2_dev,
				 "gref unmap allocation failed\n");
		}

		gnttab_free_pages(gref->count, gref->pages);
		kfree(gref->handles);
		kfree(gref);
	}

	kfree(vma_data);
}

/**
 * virtio_media_vma_close() - Close a MMAP buffer mapping.
 * @vma: VMA of the mapping to close.
 *
 * Inform the host that a previously created MMAP mapping is no longer needed
 * and can be removed.
 */
static void virtio_media_vma_close(struct vm_area_struct *vma)
{
	struct virtio_media_vma *vma_data = vma->vm_private_data;
	struct virtio_media *vv = vma_data->vv;

	mutex_lock(&vv->vlock);
	virtio_media_vma_close_locked(vma);
	mutex_unlock(&vv->vlock);
}

static const struct vm_operations_struct virtio_media_vm_ops = {
	.close = virtio_media_vma_close,
};

/**
 * virtio_media_device_mmap - Perform a mmap request from userspace.
 * @file: opened file of the session to map for.
 * @vma: VM area struct describing the desired mapping.
 *
 * This requests the host to map a MMAP buffer for us, so we can then make that
 * mapping visible into user-space address space.
 */
static int virtio_media_device_mmap(struct file *file,
				    struct vm_area_struct *vma)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtio_media *vv = to_virtio_media(video_dev);
	struct virtio_media_session *session =
		fh_to_session(file->private_data);
	struct virtio_media_cmd_mmap *cmd_mmap = &session->cmd.mmap;
	struct virtio_media_resp_mmap *resp_mmap = NULL;
	struct scatterlist cmd_sg = {}, resp_sg = {};
	struct scatterlist *sgs[2] = { &cmd_sg, &resp_sg };
	struct virtio_media_vma *vma_data = NULL;
	struct virtio_media_gref_mapping *gref = NULL;
	struct gnttab_map_grant_ref *map_ops = NULL;
	int ret;
	size_t resp_len;
	unsigned int i;
	bool pages_allocated = false;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (!(vma->vm_flags & (VM_READ | VM_WRITE)))
		return -EINVAL;

	mutex_lock(&vv->vlock);

	vma_data = kzalloc(sizeof(*vma_data), GFP_KERNEL);
	if (!vma_data) {
		ret = -ENOMEM;
		goto end;
	}
	vma_data->vv = vv;
	vma_data->session = session;

	cmd_mmap->hdr.cmd = VIRTIO_MEDIA_CMD_MMAP;
	cmd_mmap->session_id = session->id;
	cmd_mmap->flags =
		(vma->vm_flags & VM_WRITE) ? VIRTIO_MEDIA_MMAP_FLAG_RW : 0;
	cmd_mmap->offset = vma->vm_pgoff << PAGE_SHIFT;

	sg_set_buf(&cmd_sg, cmd_mmap, sizeof(*cmd_mmap));
	sg_mark_end(&cmd_sg);

	/*
	 * The host performs reference counting and is smart enough to return the
	 * same guest physical address if this is called several times on the same
	 * buffer.
	 */
	if (vv->use_grefs) {
		/* Xen grant references map buffers as normal RAM (no BAR IO). */
		unsigned int max_pages;
		size_t vma_len = vma->vm_end - vma->vm_start;
		u32 gref_count;
		u32 gref_domid;
		u32 map_flags;

		max_pages = DIV_ROUND_UP(vma_len, PAGE_SIZE);
		resp_len = sizeof(*resp_mmap) + max_pages * sizeof(u32);
		resp_mmap = kzalloc(resp_len, GFP_KERNEL);
		if (!resp_mmap) {
			ret = -ENOMEM;
			goto end;
		}

		sg_set_buf(&resp_sg, resp_mmap, resp_len);
		sg_mark_end(&resp_sg);

		ret = virtio_media_send_command(vv, sgs, 1, 1, sizeof(*resp_mmap),
						&resp_len);
		if (ret < 0)
			goto end;

		if (le32_to_cpu(resp_mmap->hdr.status)) {
			ret = -EIO;
			goto end;
		}

		gref_count = le32_to_cpu(resp_mmap->gref_count);
		gref_domid = le32_to_cpu(resp_mmap->gref_domid);
		if (!gref_count || gref_count > max_pages) {
			ret = -EINVAL;
			goto end;
		}
		if (resp_len < sizeof(*resp_mmap) + gref_count * sizeof(u32)) {
			ret = -EINVAL;
			goto end;
		}
		if (le32_to_cpu(resp_mmap->gref_page_size) != PAGE_SIZE) {
			ret = -EINVAL;
			goto end;
		}

		gref = kzalloc(sizeof(*gref), GFP_KERNEL);
		if (!gref) {
			ret = -ENOMEM;
			goto end;
		}
		gref->count = gref_count;
		gref->domid = gref_domid;
		gref->pages = kcalloc(gref_count, sizeof(*gref->pages),
				      GFP_KERNEL);
		gref->handles = kcalloc(gref_count, sizeof(*gref->handles),
					GFP_KERNEL);
		map_ops = kcalloc(gref_count, sizeof(*map_ops), GFP_KERNEL);
		if (!gref->pages || !gref->handles || !map_ops) {
			ret = -ENOMEM;
			goto end;
		}

		if (gnttab_alloc_pages(gref_count, gref->pages)) {
			ret = -ENOMEM;
			goto end;
		}
		pages_allocated = true;
		for (i = 0; i < gref_count; i++)
			gref->handles[i] = INVALID_GRANT_HANDLE;

		map_flags = GNTMAP_host_map;
		if (!(vma->vm_flags & VM_WRITE))
			map_flags |= GNTMAP_readonly;

		for (i = 0; i < gref_count; i++) {
			u32 gref_id = ((u32 *)((u8 *)resp_mmap +
					       sizeof(*resp_mmap)))[i];

			gnttab_set_map_op(&map_ops[i],
					  (phys_addr_t)page_address(
						  gref->pages[i]),
					  map_flags, le32_to_cpu(gref_id),
					  gref->domid);
		}

		ret = gnttab_map_refs(map_ops, NULL, gref->pages, gref_count);
		for (i = 0; i < gref_count; i++) {
			if (map_ops[i].status == GNTST_okay) {
				gref->handles[i] = map_ops[i].handle;
				continue;
			}
			ret = -ENXIO;
			goto end;
		}

		vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_MIXEDMAP);
		for (i = 0; i < gref_count; i++) {
			ret = vm_insert_page(vma, vma->vm_start +
					     i * PAGE_SIZE, gref->pages[i]);
			if (ret)
				goto end;
		}

		vma_data->gref = gref;
		vma_data->driver_addr = le64_to_cpu(resp_mmap->driver_addr);
		vma->vm_ops = &virtio_media_vm_ops;
		vma->vm_private_data = vma_data;
	} else {
		resp_mmap = &session->resp.mmap;
		resp_len = sizeof(*resp_mmap);
		sg_set_buf(&resp_sg, resp_mmap, resp_len);
		sg_mark_end(&resp_sg);

		ret = virtio_media_send_command(vv, sgs, 1, 1, resp_len, NULL);
		if (ret < 0)
			goto end;

		vma_data->driver_addr = le64_to_cpu(resp_mmap->driver_addr);
		vma->vm_private_data = vma_data;
		/*
		 * Keep the guest address at which the buffer is mapped since we
		 * will use that to unmap.
		 */
		vma->vm_pgoff = (resp_mmap->driver_addr +
				 vv->mmap_region.addr) >> PAGE_SHIFT;

		/*
		 * We cannot let the mapping be larger than the buffer.
		 */
		if (vma->vm_end - vma->vm_start >
		    PAGE_ALIGN(le64_to_cpu(resp_mmap->len))) {
			dev_dbg(&video_dev->dev,
				"invalid MMAP, as it would overflow buffer length\n");
			virtio_media_vma_close_locked(vma);
			ret = -EINVAL;
			goto end;
		}

		ret = io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
					 vma->vm_end - vma->vm_start,
					 vma->vm_page_prot);
		if (ret)
			goto end;

		vma->vm_ops = &virtio_media_vm_ops;
	}

end:
	mutex_unlock(&vv->vlock);
	if (ret) {
		if (gref) {
			struct gnttab_unmap_grant_ref *unmap_ops;

			unmap_ops = kcalloc(gref->count, sizeof(*unmap_ops),
					    GFP_KERNEL);
			if (unmap_ops) {
				for (i = 0; i < gref->count; i++) {
					if (gref->handles[i] ==
					    INVALID_GRANT_HANDLE) {
						continue;
					}
					gnttab_set_unmap_op(
						&unmap_ops[i],
						(phys_addr_t)page_address(
							gref->pages[i]),
						GNTMAP_host_map,
						gref->handles[i]);
				}
				gnttab_unmap_refs(unmap_ops, NULL,
						  gref->pages, gref->count);
				kfree(unmap_ops);
			}
			if (pages_allocated)
				gnttab_free_pages(gref->count, gref->pages);
			kfree(gref->handles);
			kfree(gref);
		}
		kfree(vma_data);
		if (vv->use_grefs)
			kfree(resp_mmap);
	}
	if (vv->use_grefs && !ret)
		kfree(resp_mmap);
	kfree(map_ops);
	return ret;
}

static const struct v4l2_file_operations virtio_media_fops = {
	.owner = THIS_MODULE,
	.open = virtio_media_device_open,
	.release = virtio_media_device_close,
	.poll = virtio_media_device_poll,
	.unlocked_ioctl = virtio_media_device_ioctl,
	.mmap = virtio_media_device_mmap,
};

static int virtio_media_probe(struct virtio_device *virtio_dev)
{
	struct device *dev = &virtio_dev->dev;
	struct virtqueue *vqs[2];
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	static struct virtqueue_info vq_info[2] = {
		{
			.name = "command",
			.callback = commandq_callback,
		},
		{
			.name = "event",
			.callback = eventq_callback,
		},
	};
#else
	static vq_callback_t *vq_callbacks[] = {
		commandq_callback,
		eventq_callback,
	};
	static const char *const vq_names[] = { "command", "event" };
#endif
	struct virtio_media *vv;
	struct video_device *vd;
	int i;
	int ret;

	vv = devm_kzalloc(dev, sizeof(*vv), GFP_KERNEL);
	if (!vv)
		return -ENOMEM;

	vv->event_buffer = devm_kzalloc(
		dev, VIRTIO_MEDIA_EVENT_MAX_SIZE * VIRTIO_MEDIA_NUM_EVENT_BUFS,
		GFP_KERNEL);
	if (!vv->event_buffer)
		return -ENOMEM;

	INIT_LIST_HEAD(&vv->sessions);
	mutex_init(&vv->sessions_lock);
	mutex_init(&vv->events_lock);
	mutex_init(&vv->vlock);

	vv->virtio_dev = virtio_dev;
	virtio_dev->priv = vv;

	init_waitqueue_head(&vv->wq);

	ret = v4l2_device_register(dev, &vv->v4l2_dev);
	if (ret)
		return ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	ret = virtio_find_vqs(virtio_dev, 2, vqs, vq_info, NULL);
#else
	ret = virtio_find_vqs(virtio_dev, 2, vqs, vq_callbacks, vq_names, NULL);
#endif
	if (ret)
		goto err_find_vqs;

	vv->commandq = vqs[0];
	vv->eventq = vqs[1];
	INIT_WORK(&vv->eventq_work, virtio_media_event_work);

	/* Use grant references when the device advertises the feature. */
	vv->use_grefs = virtio_has_feature(virtio_dev, VIRTIO_MEDIA_F_GNTREF);
	vv->use_export_import =
		virtio_has_feature(virtio_dev, VIRTIO_MEDIA_F_EXPORT_IMPORT);
	vv->use_share_fence =
		virtio_has_feature(virtio_dev, VIRTIO_MEDIA_F_SHARE_FENCE);
	vv->use_peer_gref_import =
		virtio_has_feature(virtio_dev, VIRTIO_MEDIA_F_PEER_GREF_IMPORT);
	if (!vv->use_grefs) {
		/* Get MMAP buffer mapping SHM region */
		virtio_get_shm_region(virtio_dev, &vv->mmap_region,
				      VIRTIO_MEDIA_SHM_MMAP);
	}

	vd = &vv->video_dev;

	vd->v4l2_dev = &vv->v4l2_dev;
	vd->vfl_type = VFL_TYPE_VIDEO;
	vd->ioctl_ops = &virtio_media_ioctl_ops;
	vd->fops = &virtio_media_fops;
	vd->device_caps = virtio_cread32(virtio_dev, 0);
	if (vd->device_caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE))
		vd->vfl_dir = VFL_DIR_M2M;
	else if (vd->device_caps &
		 (V4L2_CAP_VIDEO_OUTPUT | V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE))
		vd->vfl_dir = VFL_DIR_TX;
	else
		vd->vfl_dir = VFL_DIR_RX;
	vd->release = video_device_release_empty;
	strscpy(vd->name, "virtio-media", sizeof(vd->name));

	video_set_drvdata(vd, vv);

	ret = video_register_device(vd, virtio_cread32(virtio_dev, 4), 0);
	if (ret)
		goto err_register_device;

	for (i = 0; i < VIRTIO_MEDIA_NUM_EVENT_BUFS; i++) {
		ret = virtio_media_send_event_buffer(
			vv, vv->event_buffer + VIRTIO_MEDIA_EVENT_MAX_SIZE * i);
		if (ret)
			goto err_send_event_buffer;
	}

	virtio_device_ready(virtio_dev);

	return 0;

err_send_event_buffer:
	video_unregister_device(&vv->video_dev);
err_register_device:
	virtio_dev->config->del_vqs(virtio_dev);
err_find_vqs:
	v4l2_device_unregister(&vv->v4l2_dev);

	return ret;
}

static void virtio_media_remove(struct virtio_device *virtio_dev)
{
	struct virtio_media *vv = virtio_dev->priv;
	struct list_head *p, *n;

	cancel_work_sync(&vv->eventq_work);
	virtio_reset_device(virtio_dev);

	v4l2_device_unregister(&vv->v4l2_dev);
	virtio_dev->config->del_vqs(virtio_dev);
	video_unregister_device(&vv->video_dev);

	list_for_each_safe(p, n, &vv->sessions) {
		struct virtio_media_session *s =
			list_entry(p, struct virtio_media_session, list);

		virtio_media_session_free(vv, s);
	}
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_MEDIA, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_MEDIA_F_GNTREF,
	VIRTIO_MEDIA_F_EXPORT_IMPORT,
	VIRTIO_MEDIA_F_SHARE_FENCE,
	VIRTIO_MEDIA_F_PEER_GREF_IMPORT,
};

static struct virtio_driver virtio_media_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name = VIRTIO_MEDIA_DEFAULT_DRIVER_NAME,
	.driver.owner = THIS_MODULE,
	.id_table = id_table,
	.probe = virtio_media_probe,
	.remove = virtio_media_remove,
};

module_virtio_driver(virtio_media_driver);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("virtio media driver");
MODULE_AUTHOR("Alexandre Courbot <gnurou@gmail.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_IMPORT_NS("DMA_BUF");
