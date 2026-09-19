# SPDX-License-Identifier: BSD-3-Clause
CC ?= cc
AR ?= ar
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
DRM_CFLAGS ?= -I/usr/include/drm
CPPFLAGS += -Iinclude -I../include/uapi $(DRM_CFLAGS)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -fPIC
LIBRARY = libvirtio-media-gpu.so.0
.PHONY: all clean check install
all: $(LIBRARY) libvirtio-media-gpu.so libvirtio-media-gpu.a
$(LIBRARY): virtio-media-gpu.o
	$(CC) -shared -Wl,-soname,$(LIBRARY) $(LDFLAGS) $^ -o $@
libvirtio-media-gpu.so: $(LIBRARY)
	ln -sf $(LIBRARY) $@
libvirtio-media-gpu.a: virtio-media-gpu.o
	$(AR) rcs $@ $^
virtio-media-gpu.o: virtio-media-gpu.c include/virtio-media-gpu.h ../include/uapi/linux/virtio_media_gpu.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
tests/adapter-test: tests/adapter-test.c libvirtio-media-gpu.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -Wl,--wrap=ioctl,--wrap=mmap,--wrap=munmap,--wrap=close -o $@
tests/export-check: tests/export-check.c libvirtio-media-gpu.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@
check: tests/adapter-test
	./tests/adapter-test
install: all
	install -D -m 0755 $(LIBRARY) $(DESTDIR)$(LIBDIR)/$(LIBRARY)
	ln -sf $(LIBRARY) $(DESTDIR)$(LIBDIR)/libvirtio-media-gpu.so
	install -D -m 0644 libvirtio-media-gpu.a $(DESTDIR)$(LIBDIR)/libvirtio-media-gpu.a
	install -D -m 0644 include/virtio-media-gpu.h $(DESTDIR)$(INCLUDEDIR)/virtio-media-gpu.h
	install -D -m 0644 ../include/uapi/linux/virtio_media_gpu.h $(DESTDIR)$(INCLUDEDIR)/linux/virtio_media_gpu.h
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@LIBDIR@|$(LIBDIR)|g' -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' virtio-media-gpu.pc.in > $(DESTDIR)$(LIBDIR)/pkgconfig/virtio-media-gpu.pc
clean:
	rm -f *.o *.a *.so *.so.* tests/adapter-test tests/export-check
