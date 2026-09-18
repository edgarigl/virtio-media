// SPDX-License-Identifier: GPL-2.0-only
/* Test-only adapter to the in-kernel virtio dma-buf UUID query API. */
#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/virtio_dma_buf.h>

struct uuid_query { __s32 fd; __u8 uuid[16]; };
#define UUID_QUERY _IOWR('U', 0, struct uuid_query)
static long query_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct uuid_query q;
    struct dma_buf *buf;
    uuid_t uuid;
    int ret;

    if (cmd != UUID_QUERY)
        return -ENOTTY;
    if (copy_from_user(&q, (void __user *)arg, sizeof(q)))
        return -EFAULT;
    buf = dma_buf_get(q.fd);
    if (IS_ERR(buf))
        return PTR_ERR(buf);
    ret = virtio_dma_buf_get_uuid(buf, &uuid);
    dma_buf_put(buf);
    if (ret)
        return ret;
    memcpy(q.uuid, uuid.b, sizeof(q.uuid));
    return copy_to_user((void __user *)arg, &q, sizeof(q)) ? -EFAULT : 0;
}
static const struct file_operations ops = {
    .owner = THIS_MODULE, .unlocked_ioctl = query_ioctl,
    .compat_ioctl = query_ioctl,
};
static struct miscdevice probe = {
    .minor = MISC_DYNAMIC_MINOR, .name = "fanout-uuid-probe",
    .fops = &ops, .mode = 0600,
};
static int __init start(void) { return misc_register(&probe); }
static void __exit stop(void) { misc_deregister(&probe); }
module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test-only virtio dma-buf UUID query");
MODULE_IMPORT_NS("DMA_BUF");
