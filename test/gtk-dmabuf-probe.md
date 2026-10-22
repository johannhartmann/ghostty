# NVIDIA EGL DMA-BUF presentation corruption

GTK's `GtkGLArea` can replace a valid OpenGL texture with a DMA-BUF exported
through `EGL_MESA_image_dma_buf_export`. On an NVIDIA RTX Spark N1X (GB20B,
aarch64) with driver 615.78.08 and GTK 4.24.1, importing the exported buffer
succeeds but returns scrambled pixels. Both native Wayland and XWayland/EGL
are affected. GLX, Mesa software rendering, and GTK with DMA-BUF disabled
render correctly.

The exported format observed on that machine is AB24, one plane, with
modifier `0x0300000000e08010`. A standalone EGL-only round trip reproduces
the corruption; it does not require GTK, Ghostty shaders, fonts, a shell,
or compositor presentation. This identifies an EGL driver interoperability
failure, not the precise fault in the driver's export/import implementation.

## Source-level compatibility fix

Before GTK initializes, the Linux GTK runtime checks NVIDIA EGL devices.
It uploads a small nonuniform RGBA8 texture with an odd row width, verifies
that the original texture reads back correctly, exports its DMA-BUF, imports
it using the returned format/modifier/stride/offset, and compares the result.

Only a successful round trip that changes pixels disables GTK's DMA-BUF
presentation path. Unavailable capabilities or an intact round trip preserve
the existing GTK behavior. OpenGL rendering and native Wayland stay enabled.
No GPU model/version blacklist, software renderer, or preload library is used.
The probe restores the EGL client API and releases its contexts, images,
textures, displays and file descriptors. It skips caller-owned active contexts.
EGL is loaded dynamically, so the executable does not acquire a mandatory
EGL shared-library dependency.

This guards against the driver bug; it does not repair the proprietary
NVIDIA EGL implementation. It runs once per application startup, not per frame.
In a multi-GPU setup, corruption on any enumerated NVIDIA device selects
GTK's alternative texture presentation for this application.

## Standalone reproduction

From the repository root, with EGL development headers installed:

```sh
cc -std=c11 -Wall -Wextra -Werror -Ivendor/glad/include \
  -DGHOSTTY_DMABUF_PROBE_MAIN src/apprt/gtk/dmabuf_probe.c \
  -ldl -o /tmp/ghostty-dmabuf-probe
/tmp/ghostty-dmabuf-probe
```

`corrupted` exits 1; `intact` or `unavailable` exits 0. This executable
uses EGL directly and does not initialize GTK.

## Hardware integration regression test

```sh
cc -std=c11 -Wall -Wextra -Werror -Ivendor/glad/include \
  src/apprt/gtk/dmabuf_probe.c test/gtk-dmabuf-probe.c \
  -ldl -lEGL -o /tmp/ghostty-dmabuf-regression
/tmp/ghostty-dmabuf-regression 2
```

The argument is the expected device-dependent result: 0 = unavailable,
1 = intact, 2 = corrupted. The test repeats the probe, checks that the
original EGL client API and current context are preserved, and verifies
that an existing caller-owned context is skipped without disturbing it.
On the affected N1X, result 2 passes. Restricting GLVND to Mesa on the same
machine gives result 0 and also passes:

```sh
__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
  /tmp/ghostty-dmabuf-regression 0
```

A full ReleaseFast build was also run natively on aarch64 with Zig 0.15.2.
The resulting application, using the normal configuration and no diagnostic
environment overrides, displayed readable text; Hyprland reported native
Wayland and `nvidia-smi` listed it as an NVIDIA graphics process.

References:

- https://docs.gtk.org/gtk4/running.html#other-environment-variables
- https://registry.khronos.org/EGL/extensions/MESA/EGL_MESA_image_dma_buf_export.txt
