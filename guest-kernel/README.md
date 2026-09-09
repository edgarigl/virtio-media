# Guest GPU export experiment

The optional helper in `virtgpu_media.c` lets virtio-media `VIDIOC_EXPBUF`
return a real virtio-gpu dma-buf backed by an external host allocation. The
application uses ordinary V4L2 EXPBUF and Vulkan dma-buf import; it does not
create a blob by hand or exchange QMP fds.

This is an experimental kernel integration, not an upstream virtio interface.
It was built against the Linux v6.14 virtio-gpu sources and Ubuntu
6.14.0-37-generic guest headers. Build the helper into the guest's virtio-gpu
module; it is not a separate module. Use driver sources matching your kernel.

```sh
sh guest-kernel/build-virtgpu.sh \
    /path/to/linux/drivers/gpu/drm/virtio \
    /lib/modules/$(uname -r)/build /tmp/virtgpu-fanout
make -C driver KDIR=/lib/modules/$(uname -r)/build
```

On a test guest, stop applications holding DRM/video devices before replacing
the modules. Retain their dependencies with rmmod rather than removing the
dependency tree:

```sh
sudo modprobe virtio_gpu
sudo rmmod virtio_gpu
sudo insmod /tmp/virtgpu-fanout/virtio-gpu.ko
sudo modprobe videodev
sudo modprobe videobuf2_memops
sudo insmod driver/virtio-media.ko
```

If virtio-media is already loaded, unload its old module before the last step.
These commands do not replace the installed modules; a reboot restores those.

The QEMU companion is on
[virtio-media-handover](https://github.com/edgarigl/qemu/tree/virtio-media-handover).
Configure both devices:

```text
-device virtio-gpu-gl-pci,blob=on,hostmem=256M,venus=on,x-blob-import-media=on
-device virtio-media-pci,host-device=/dev/video11,max-buffers=9,host-v4l2-memory=export-gpu,xen-grants=off
```

QEMU obtains the loopback CAPTURE dma-bufs at REQBUFS and retains a private
blob token per plane. Feature bit 58 negotiates command 10 (EXPORT_GPU), using
the existing export request layout and a response containing the standard
header, a 64-bit blob token and a 64-bit allocation length. The guest helper
creates a HOST3D resource backed by this token and exports its GEM object.
Tokens are QEMU-local, never reused within a process, and removed when the
media queue is released. Existing exported GPU resources retain their own
storage references after the video node closes.

The helper requires exactly one bound virtio-gpu device in the guest. Its
symbol is optional: an unmodified virtio-gpu module makes EXPBUF return
EOPNOTSUPP. The old virtio-media export/import and import-uuid paths retain
their existing behavior when the new feature is absent.

The host pool must be bound to stable loopback slots before guest REQBUFS.
This experiment uses a finite, frozen pool: the current loopback producer
completion is not a consumer-release barrier. Continuous reuse, Android
gfxstream import, explicit pairing of multiple GPUs, and CPU mmap of the
video node in export-gpu mode remain outside this experiment. An EXPBUF fd
uses the GPU's mapping implementation. GPU/display completion must precede
requeue when continuous capture is added.
