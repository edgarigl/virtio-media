# Guest GPU buffer adapter

`libvirtio-media-gpu` exports a virtio-media capture slot as a GPU-backed
DMA-BUF using the stock Linux virtio-gpu DRM interface. It connects the local
media export-info ioctl to QEMU's existing external-media blob importer.
Applications keep their V4L2 queues and Vulkan import/processing code.

This is a local extension, not an upstream V4L2/virtio protocol. It replaces
the need for the optional GPU kernel helper in applications using this API.
The ordinary VIDIOC_EXPBUF path remains available with that helper; an
unmodified application does not automatically select the userspace adapter.

## Build and install

Use DRM UAPI headers that include RESOURCE_CREATE_BLOB. On the qualified
Ubuntu guest these are under /usr/include/drm; the separate libdrm header
under /usr/include/libdrm is older. Override DRM_CFLAGS if needed.

```sh
make -C userspace
make -C userspace check
make -C userspace tests/export-check
make -C userspace install DESTDIR=/tmp/media-adapter-install PREFIX=/usr
```

The library has no Mesa, Vulkan, libdrm runtime, or GPU kernel helper dependency.
It uses libc and kernel ioctls. Installation provides a shared library with
SONAME libvirtio-media-gpu.so.0, a static library, headers and pkg-config metadata.
Install the staged files in the guest and refresh its loader cache as appropriate.
Compile a client with `pkg-config --cflags --libs virtio-media-gpu`.
The API version is 0.1; the SONAME major tracks incompatible library changes.

Build the matching virtio-media module from driver/ against the guest kernel.
Use its normal distribution virtio-gpu module. QEMU needs:

```text
-device virtio-gpu-gl-pci,blob=on,hostmem=256M,venus=on,x-blob-import-media=on
-device virtio-media-pci,host-device=/dev/video11,host-v4l2-memory=export-gpu,xen-grants=off
```

For common allocation UUIDs add `x-blob-common-uuid=on` and pass
VMEDIA_GPU_ASSIGN_UUID to the library. Options=0 omits UUID assignment.
Neither option changes existing QEMU or kernel module defaults. UUID assignment
support alone does not prove the host provides common identities.

## Application integration

After opening the video device and allocating its queue with VIDIOC_REQBUFS,
open the intended virtio-gpu render node and replace the export operation:

```c
#include <fcntl.h>
#include <unistd.h>
#include <virtio-media-gpu.h>

int gpu_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
struct v4l2_exportbuffer buffer = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .index = slot,
    .plane = 0,
    .flags = O_RDONLY | O_CLOEXEC,
};
if (gpu_fd < 0 ||
    vmedia_gpu_expbuf(video_fd, gpu_fd, &buffer, VMEDIA_GPU_ASSIGN_UUID) < 0) {
    /* Handle errno; buffer.fd is not changed on failure. */
}
/* On success, buffer.fd is an ordinary DMA-BUF for Vulkan import.
 * Export other slots using the same gpu_fd, then close gpu_fd.
 * Keep or transfer ownership of each DMA-BUF according to the consumer API.
 */
```

The library borrows video_fd and gpu_fd. The caller owns the returned DMA-BUF
and must eventually close or transfer it. Already exported resources survive
closing the original video and DRM fds. This does not reserve a frame: DQBUF
and QBUF still govern frame access, and GPU completion must precede QBUF.
Queue closure still follows fanoutd's quarantine and recovery protocol.

Select the render node explicitly; the adapter does not guess which GPU should
receive the buffer. Serialize exports against REQBUFS, queue teardown and
closing either input fd. Concurrent calls for different slots are stateless,
but the library cannot protect application-managed queue lifetime.

Supported fd access flags are O_RDONLY or O_RDWR, optionally O_CLOEXEC.
O_WRONLY and unknown flags/options are rejected. These flags are not host GPU
write protection. Keep the trusted/best-effort allocation boundary as designed.

## Export-info ABI and errors

include/uapi/linux/virtio_media_gpu.h defines VIDIOC_VIRTIO_MEDIA_EXPORT_GPU.
Initialize version=1, type, index, plane; zero flags/reserved fields. It returns
the QEMU-local blob token and page-aligned allocation size. It allocates no
guest GPU resource and dequeues no frame. Existing QEMU command 10 provides
the response. The token expires when the media queue is released; it is not
a UUID or a capability for another QEMU instance.

The library checks the DRM driver name and required blob, 3D and host-visible
capabilities. It creates a shareable/mappable HOST3D blob, exports a PRIME fd,
and performs a full-size read-only mmap/munmap without accessing pixels. That
mapping waits for the stock driver's host mapping result, so rejected host
imports are not silently returned as successful exports. No image conversion
or pixel copy occurs in the adapter.

Failures return -1 with errno, leaving the supplied export structure unchanged.
Temporary fds and GEM handles are released on failure. Wrong GPU drivers fail
with ENODEV; missing advertised capabilities with EOPNOTSUPP; malformed host
tokens/sizes with EPROTO; invalid arguments with EINVAL. Older media drivers
can reject the private ioctl with ENOTTY or another unsupported-ioctl error.
Backend rejection is reported through the DRM/mapping error.
The library does not fall back to copying, another GPU, or the kernel helper.

Calls can block in the existing GPU driver and have no independent timeout.
The caller must manage device-loss/liveness policy. UUID assignment itself is
asynchronous; its availability is determined by the normal kernel UUID API.
There is no new production userspace UUID-query ioctl in this library.

## Validation

`make check` injects ioctl and host-mapping failures and checks rollback,
fd flags, capability rejection, malformed tokens and EINTR handling.
`tests/export-check VIDEO RENDER_NODE [UUID_PROBE]` exercises the live media
ABI, four fd-flag combinations, aliases and lifetime after queue/device-fd close.
The optional UUID_PROBE is the existing test-only guest-kernel/tests module.

Fanoutd's optional stream-adapter-check and stream-gpu-adapter-check targets
exercise this API directly; they use no LD_PRELOAD interposition.

The initial qualified target is x86-64 Ubuntu 6.14 Linux/Venus with the existing
QEMU and Xen HMEM stack. The fixed-width ioctl layout does not by itself
qualify 32-bit compatibility, Android/gfxstream, or multiple GPU devices.

### Android EVS integration and VMA regression

`Android.bp` provides the vendor static module `libvirtio_media_gpu` for an
internal provider dependency. Place this repository in the product's source
manifest before enabling a provider target that links it. It uses the product
libdrm headers; full Soong product validation remains required.

`tests/mmap-lifetime.c` exercises a media MMAP surviving child exit, a split
mapping, and an oversized map rejection, without starting capture. Compile
with the Android NDK and run only after installing the VMA reference-counting
fix. On the old driver, child exit can free the parent's VMA private data and
panic the guest during later teardown. The 21 September guest panic happened
after a provider fd-ownership abort; the missing VMA open reference is a source
finding consistent with that trace, not a completed runtime root-cause test.
The probe has been cross-compiled but not run. The live crashed AAOS17 guest
was preserved and no module was replaced.
