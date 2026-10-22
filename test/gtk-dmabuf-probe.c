/* Hardware integration test. See gtk-dmabuf-probe.md for build/run commands. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

extern int ghostty_gtk_dmabuf_probe(void);

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s EXPECTED_RESULT (0=unavailable, 1=intact, 2=corrupted)\n", argv[0]);
        return 2;
    }
    int expected = atoi(argv[1]);
    assert(expected >= 0 && expected <= 2);

    /* A failed round trip must not leave its OpenGL API binding behind. */
    assert(eglBindAPI(EGL_OPENGL_ES_API));
    for (int i = 0; i < 3; i++) {
        int actual = ghostty_gtk_dmabuf_probe();
        assert(actual == expected);
        assert(eglQueryAPI() == EGL_OPENGL_ES_API);
        assert(eglGetCurrentContext() == EGL_NO_CONTEXT);
    }
    puts("PASS: repeatable probe result; original API and context preserved");

    /* Existing contexts belong to the caller and must not be disturbed. */
    PFNEGLQUERYDEVICESEXTPROC query_devices = (void *)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!query_devices || !get_display) return 0;
    EGLDeviceEXT devices[16];
    EGLint count;
    if (!query_devices(16, devices, &count)) return 0;
    for (EGLint i = 0; i < count && i < 16; i++) {
        EGLDisplay display = get_display(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) continue;
        assert(eglBindAPI(EGL_OPENGL_API));
        const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        EGLConfig config;
        EGLint configs;
        EGLContext context = EGL_NO_CONTEXT;
        if (eglChooseConfig(display, attrs, &config, 1, &configs) && configs)
            context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
        if (context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
            assert(ghostty_gtk_dmabuf_probe() == 0);
            assert(eglGetCurrentContext() == context);
            assert(eglGetCurrentDisplay() == display);
            assert(eglQueryAPI() == EGL_OPENGL_API);
            puts("PASS: current caller-owned context skipped and preserved");
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
            eglTerminate(display);
            return 0;
        }
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
    }
    puts("SKIP: no surfaceless context available for caller-context test");
    return 0;
}
