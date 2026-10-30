#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the driver's queue transitions with injected command/event ordering.

This compiles the actual queue structures and ioctl functions. Transport,
locking and import callbacks are stubs; it is not a kernel concurrency test.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'driver/virtio_media_ioctls.c').read_text()
header = (root / 'driver/session.h').read_text()


def extract(text, start):
    begin = text.index(start)
    end = text.index('\n}', begin) + 2
    if text[end:end + 1] == ';':
        end += 1
    return text[begin:end] + '\n'


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <linux/videodev2.h>
typedef uint32_t u32;
typedef uint8_t u8;
#define VIRTIO_MEDIA_LAST_QUEUE V4L2_BUF_TYPE_META_OUTPUT
struct list_head { struct list_head *next, *prev; };
#define list_for_each_safe(p, n, h) \
    for (p = (h)->next, n = p->next; p != (h); p = n, n = p->next)
#define list_entry(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
static void list_del(struct list_head *p)
{
    p->next->prev = p->prev;
    p->prev->next = p->next;
}
static void mutex_lock(int *lock) { assert(!(*lock)++); }
static void mutex_unlock(int *lock) { assert((*lock)-- == 1); }
'''
structs = (extract(header, 'struct virtio_media_buffer {') +
           extract(header, 'struct virtio_media_queue_state {'))
stubs = r'''
struct virtio_media { int events_lock; };
struct v4l2_fh { struct virtio_media *vdev; };
struct file { struct v4l2_fh *fh; };
struct virtio_media_session {
    struct v4l2_fh fh;
    struct virtio_media_queue_state queues[VIRTIO_MEDIA_LAST_QUEUE + 1];
    int queues_lock;
};
#define file_to_v4l2_fh(f) ((f)->fh)
#define fh_to_session(f) ((struct virtio_media_session *)(f))
#define to_virtio_media(d) (d)
static int prepare_ret, command_ret, stop_ret;
static unsigned submissions, releases, completions, drains;
static bool early_done, corrupt_reply;
static int virtio_media_prepare_imports(struct virtio_media *v,
        struct virtio_media_buffer *b, struct v4l2_buffer *input)
{
    return prepare_ret;
}
static void virtio_media_release_imports(struct virtio_media_buffer *b)
{
    releases++;
}
static void virtio_media_complete_imports(struct virtio_media_buffer *b, int err)
{
    assert(err == -ECANCELED);
    completions++;
}
static void virtio_media_process_events(struct virtio_media *v) { drains++; }
static int virtio_media_send_buffer_ioctl(struct v4l2_fh *fh, u32 cmd,
                                         struct v4l2_buffer *b)
{
    struct virtio_media_queue_state *q = &fh_to_session(fh)->queues[b->type];
    assert(cmd == VIDIOC_QBUF && q->queued_bufs == 1);
    submissions++;
    if (early_done) {
        q->buffers[b->index].buffer.flags |= V4L2_BUF_FLAG_DONE;
        q->queued_bufs--;
    }
    if (corrupt_reply)
        b->memory = V4L2_MEMORY_MMAP;
    return command_ret;
}
static int virtio_media_send_w_ioctl(struct v4l2_fh *fh, u32 cmd,
                                     void *input, size_t size)
{
    assert(cmd == VIDIOC_STREAMON || cmd == VIDIOC_STREAMOFF);
    return cmd == VIDIOC_STREAMOFF ? stop_ret : 0;
}
'''
functions = ''.join(extract(source, 'static void ' + name + '(')
                    for name in ['virtio_media_clear_queue'])
functions += ''.join(extract(source, 'static int ' + name + '(')
                     for name in ['virtio_media_streamon',
                                  'virtio_media_streamoff', 'virtio_media_qbuf'])
tests = r'''
int main(void)
{
    for (int trial = 0; trial < 5; trial++) {
        struct virtio_media dev = {0};
        struct virtio_media_session s = {.fh = {.vdev = &dev}};
        struct file file = {.fh = &s.fh};
        struct virtio_media_buffer storage = {0};
        struct virtio_media_queue_state *q = &s.queues[1];
        struct v4l2_buffer b = {.type = 1, .memory = V4L2_MEMORY_DMABUF,
                               .length = 4096, .m.fd = 99};
        q->buffers = &storage;
        q->allocated_bufs = 1;
        q->memory = b.memory;
        q->pending_dqbufs.next = q->pending_dqbufs.prev = &q->pending_dqbufs;
        prepare_ret = trial == 4 ? -ENOMEM : 0;
        command_ret = trial == 0 ? 0 : -EINVAL;
        early_done = trial == 2;
        corrupt_reply = trial == 3;
        stop_ret = 0;
        submissions = releases = completions = drains = 0;
        int ret = virtio_media_qbuf(&file, NULL, &b);
        if (trial == 0) {
            assert(!ret && q->queued_bufs == 1 && !q->import_needs_cancel);
            assert(virtio_media_qbuf(&file, NULL, &b) == -EINVAL);
        } else if (trial == 4) {
            assert(ret == -ENOMEM && !submissions && !q->import_needs_cancel);
            continue;
        } else {
            assert(ret == -EINVAL && q->queued_bufs == 0);
            assert(releases == 1 && q->import_needs_cancel);
            b.memory = V4L2_MEMORY_DMABUF;
            assert(virtio_media_qbuf(&file, NULL, &b) == -EIO);
            assert(submissions == 1);
            assert(virtio_media_streamon(&file, NULL, 1) == -EIO);
            stop_ret = -EIO;
            assert(virtio_media_streamoff(&file, NULL, 1) == -EIO);
            assert(q->import_needs_cancel && !drains);
            stop_ret = 0;
        }
        assert(!virtio_media_streamoff(&file, NULL, 1));
        assert(!q->import_needs_cancel && !q->queued_bufs);
        assert(!storage.buffer.flags && completions == 1 && drains == 1);
        command_ret = 0;
        early_done = corrupt_reply = false;
        assert(!virtio_media_qbuf(&file, NULL, &b));
        assert(q->queued_bufs == 1 && storage.buffer.m.fd == 99);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(prefix + structs + stubs + functions + tests)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-sign-compare',
                    '-fsanitize=address,undefined', '-o', str(exe), str(c)],
                   check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: 5 frontend queue/cancel/error-ordering cases; ASan/UBSan')
