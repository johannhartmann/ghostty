/*
 * Validate NVIDIA's EGL DMA-BUF export/import before GTK initializes.
 *
 * Some drivers advertise a successful round trip but import corrupted pixels.
 * GtkGLArea then replaces its valid GL texture with that corrupted DMA-BUF.
 * Test the data, rather than maintaining a driver or GPU version blacklist.
 * No EGL/GL library becomes a mandatory dependency: load EGL at runtime and
 * leave GTK's normal behavior alone when the probe cannot be performed.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/gl.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef void (*ImageTargetTexture2D)(GLenum, void *);

/* 0: unavailable, 1: intact, 2: corrupted. */
static int probe_display(EGLDisplay display, PFNEGLGETPROCADDRESSPROC get_proc) {
#define LOAD(type, name) type name = (type)get_proc(#name); if (!name) return 0
    LOAD(PFNEGLBINDAPIPROC, eglBindAPI);
    LOAD(PFNEGLCHOOSECONFIGPROC, eglChooseConfig);
    LOAD(PFNEGLCREATECONTEXTPROC, eglCreateContext);
    LOAD(PFNEGLDESTROYCONTEXTPROC, eglDestroyContext);
    LOAD(PFNEGLMAKECURRENTPROC, eglMakeCurrent);
    LOAD(PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR);
    LOAD(PFNEGLDESTROYIMAGEKHRPROC, eglDestroyImageKHR);
    LOAD(PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC, eglExportDMABUFImageQueryMESA);
    LOAD(PFNEGLEXPORTDMABUFIMAGEMESAPROC, eglExportDMABUFImageMESA);
    LOAD(PFNGLGETSTRINGPROC, glGetString);
    LOAD(PFNGLGETERRORPROC, glGetError);
    LOAD(PFNGLGENTEXTURESPROC, glGenTextures);
    LOAD(PFNGLDELETETEXTURESPROC, glDeleteTextures);
    LOAD(PFNGLBINDTEXTUREPROC, glBindTexture);
    LOAD(PFNGLTEXIMAGE2DPROC, glTexImage2D);
    LOAD(PFNGLTEXPARAMETERIPROC, glTexParameteri);
    LOAD(PFNGLGETTEXIMAGEPROC, glGetTexImage);
    LOAD(ImageTargetTexture2D, glEGLImageTargetTexture2DOES);
    LOAD(PFNGLFINISHPROC, glFinish);
#undef LOAD

    EGLConfig config;
    EGLint count;
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE,
    };
    const EGLint context_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    if (!eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(display, config_attrs, &config, 1, &count) || !count)
        return 0;

    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (context == EGL_NO_CONTEXT) return 0;
    int result = 0;
    GLuint source = 0, imported_texture = 0;
    EGLImage source_image = EGL_NO_IMAGE, imported_image = EGL_NO_IMAGE;
    int fds[4] = {-1, -1, -1, -1};
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) goto cleanup;

    const char *vendor = (const char *)glGetString(GL_VENDOR);
    if (!vendor || !strstr(vendor, "NVIDIA")) goto cleanup;

    /* An odd size catches row-stride and tiled-layout errors. */
    enum { WIDTH = 127, HEIGHT = 65, SIZE = WIDTH * HEIGHT * 4 };
    uint8_t expected[SIZE], actual[SIZE];
    for (size_t i = 0; i < SIZE; i++) expected[i] = (uint8_t)((i * 73) ^ (i >> 8));

    glGenTextures(1, &source);
    glBindTexture(GL_TEXTURE_2D, source);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, WIDTH, HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, expected);
    glFinish();
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    /* A bad upload/context is inconclusive, not a broken DMA-BUF round trip. */
    if (glGetError() != GL_NO_ERROR || memcmp(expected, actual, SIZE)) goto cleanup;

    const EGLint image_attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    source_image = eglCreateImageKHR(display, context, EGL_GL_TEXTURE_2D_KHR,
                                    (EGLClientBuffer)(uintptr_t)source, image_attrs);
    if (source_image == EGL_NO_IMAGE) goto cleanup;
    EGLint fourcc, planes, strides[4], offsets[4];
    EGLuint64KHR modifiers[4];
    if (!eglExportDMABUFImageQueryMESA(display, source_image, &fourcc, &planes, NULL) ||
        planes != 1 ||
        !eglExportDMABUFImageQueryMESA(display, source_image, &fourcc, &planes, modifiers) ||
        !eglExportDMABUFImageMESA(display, source_image, fds, strides, offsets)) goto cleanup;

    const EGLint import_attrs[] = {
        EGL_WIDTH, WIDTH, EGL_HEIGHT, HEIGHT, EGL_LINUX_DRM_FOURCC_EXT, fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, fds[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, strides[0],
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, offsets[0],
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)modifiers[0],
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(modifiers[0] >> 32),
        EGL_NONE,
    };
    imported_image = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                      NULL, import_attrs);
    if (imported_image == EGL_NO_IMAGE) goto cleanup;
    glGenTextures(1, &imported_texture);
    glBindTexture(GL_TEXTURE_2D, imported_texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, imported_image);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    if (glGetError() != GL_NO_ERROR) goto cleanup;
    result = memcmp(expected, actual, SIZE) ? 2 : 1;

cleanup:
    if (imported_texture) glDeleteTextures(1, &imported_texture);
    if (imported_image != EGL_NO_IMAGE) eglDestroyImageKHR(display, imported_image);
    if (source_image != EGL_NO_IMAGE) eglDestroyImageKHR(display, source_image);
    for (size_t i = 0; i < 4; i++) if (fds[i] >= 0) close(fds[i]);
    if (source) glDeleteTextures(1, &source);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    return result;
}

int ghostty_gtk_dmabuf_probe(void) {
    void *library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) return 0;
    int result = 0;
    PFNEGLGETPROCADDRESSPROC get_proc = (PFNEGLGETPROCADDRESSPROC)dlsym(library, "eglGetProcAddress");
    if (!get_proc) goto cleanup;
#define LOAD(type, name) type name = (type)get_proc(#name); if (!name) goto cleanup
    LOAD(PFNEGLQUERYDEVICESEXTPROC, eglQueryDevicesEXT);
    LOAD(PFNEGLGETPLATFORMDISPLAYEXTPROC, eglGetPlatformDisplayEXT);
    LOAD(PFNEGLINITIALIZEPROC, eglInitialize);
    LOAD(PFNEGLTERMINATEPROC, eglTerminate);
    LOAD(PFNEGLQUERYSTRINGPROC, eglQueryString);
    LOAD(PFNEGLGETCURRENTCONTEXTPROC, eglGetCurrentContext);
    LOAD(PFNEGLQUERYAPIPROC, eglQueryAPI);
    LOAD(PFNEGLBINDAPIPROC, eglBindAPI);
#undef LOAD
    /* Never disturb an embedding application's current context. */
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) goto cleanup;
    EGLenum previous_api = eglQueryAPI();
    EGLDeviceEXT devices[16];
    EGLint count;
    if (!eglQueryDevicesEXT(16, devices, &count)) goto cleanup;
    for (EGLint i = 0; i < count && i < 16; i++) {
        EGLDisplay display = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) continue;
        const char *vendor = eglQueryString(display, EGL_VENDOR);
        if (vendor && strstr(vendor, "NVIDIA")) {
            int checked = probe_display(display, get_proc);
            if (checked > result) result = checked;
        }
        eglTerminate(display);
        if (result == 2) break;
    }
    eglBindAPI(previous_api);
cleanup:
    dlclose(library);
    return result;
}

#ifdef GHOSTTY_DMABUF_PROBE_MAIN
#include <stdio.h>
int main(void) {
    int result = ghostty_gtk_dmabuf_probe();
    printf("DMA-BUF probe: %s\n", result == 2 ? "corrupted" : result == 1 ? "intact" : "unavailable");
    return result == 2 ? 1 : 0;
}
#endif
