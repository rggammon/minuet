// SPDX-License-Identifier: GPL-3.0-only
#define _POSIX_C_SOURCE 200809L
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

enum { WIDTH = 1024, HEIGHT = 600 };
static volatile sig_atomic_t stopped;

static void stop(int signal_number) {
    (void)signal_number;
    stopped = 1;
}

static double seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void rectangle(int x, int y, int width, int height,
                      float red, float green, float blue) {
    glScissor(x, HEIGHT - y - height, width, height);
    glClearColor(red, green, blue, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

int main(int argc, char **argv) {
    bool configure_output = argc == 3 && strcmp(argv[2], "--configure-output") == 0;
    if ((argc != 2 && !configure_output) || strcmp(argv[1], "--display") != 0) {
        fprintf(stderr, "Usage: %s --display [--configure-output]\n"
                        "Shows a 90-second HDMI test.\n"
                        "Optionally configures the connected output through NVIDIA NVDC.\n"
                        "Reads only the BIQU BTT-HDMI7 touchscreen; no input grab.\n", argv[0]);
        return EXIT_FAILURE;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    int result = EXIT_FAILURE;
    int input = -1;
    int drm_fd = -1;
    drmModeRes *resources = NULL;
    drmModeConnector *connector = NULL;
    drmModeEncoder *encoder = NULL;
    drmModeCrtc *crtc = NULL;
    uint32_t mode_blob = 0;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLStreamKHR stream = EGL_NO_STREAM_KHR;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;

#define LOAD(type, name) \
    type name = (type)eglGetProcAddress(#name); \
    if (!name) { fprintf(stderr, "Missing EGL entry point: %s\n", #name); return EXIT_FAILURE; }
    LOAD(PFNEGLQUERYDEVICESEXTPROC, eglQueryDevicesEXT);
    LOAD(PFNEGLGETPLATFORMDISPLAYEXTPROC, eglGetPlatformDisplayEXT);
    LOAD(PFNEGLGETOUTPUTLAYERSEXTPROC, eglGetOutputLayersEXT);
    LOAD(PFNEGLQUERYOUTPUTLAYERATTRIBEXTPROC, eglQueryOutputLayerAttribEXT);
    LOAD(PFNEGLCREATESTREAMKHRPROC, eglCreateStreamKHR);
    LOAD(PFNEGLDESTROYSTREAMKHRPROC, eglDestroyStreamKHR);
    LOAD(PFNEGLSTREAMCONSUMEROUTPUTEXTPROC, eglStreamConsumerOutputEXT);
    LOAD(PFNEGLCREATESTREAMPRODUCERSURFACEKHRPROC, eglCreateStreamProducerSurfaceKHR);
    LOAD(PFNEGLQUERYSTREAMKHRPROC, eglQueryStreamKHR);
#undef LOAD

#define CHECK(condition, message) \
    do { if (!(condition)) { \
        fprintf(stderr, "%s (EGL error 0x%x)\n", message, eglGetError()); \
        goto cleanup; \
    } } while (0)

    EGLDeviceEXT devices[8];
    EGLint count = 0;
    CHECK(eglQueryDevicesEXT(8, devices, &count) && count > 0, "No EGL device");
    if (configure_output) {
        drm_fd = drmOpen("drm-nvdc", NULL);
        if (drm_fd < 0) { perror("drmOpen(drm-nvdc)"); goto cleanup; }
        resources = drmModeGetResources(drm_fd);
        if (!resources) { perror("drmModeGetResources"); goto cleanup; }
        for (int i = 0; i < resources->count_connectors; ++i) {
            connector = drmModeGetConnector(drm_fd, resources->connectors[i]);
            if (!connector) { perror("drmModeGetConnector"); goto cleanup; }
            if (connector->connection == DRM_MODE_CONNECTED && connector->encoder_id != 0)
                break;
            drmModeFreeConnector(connector);
            connector = NULL;
        }
        if (!connector) { fprintf(stderr, "No connected NVDC output with an encoder\n"); goto cleanup; }
        encoder = drmModeGetEncoder(drm_fd, connector->encoder_id);
        if (!encoder) { perror("drmModeGetEncoder"); goto cleanup; }
        crtc = drmModeGetCrtc(drm_fd, encoder->crtc_id);
        if (!crtc) { perror("drmModeGetCrtc"); goto cleanup; }
        if (!crtc->mode_valid || crtc->mode.hdisplay != WIDTH || crtc->mode.vdisplay != HEIGHT) {
            fprintf(stderr, "Connected output is not already configured for %dx%d\n", WIDTH, HEIGHT);
            goto cleanup;
        }
        if (drmSetMaster(drm_fd) != 0) { perror("drmSetMaster"); goto cleanup; }
        if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
            perror("Enabling NVDC atomic modesetting");
            goto cleanup;
        }
        /* NVDC's -1 framebuffer value sets the mode without binding a framebuffer. */
        if (drmModeSetCrtc(drm_fd, crtc->crtc_id, (uint32_t)-1, 0, 0,
                           &connector->connector_id, 1, &crtc->mode) != 0) {
            perror("drmModeSetCrtc");
            goto cleanup;
        }
        printf("Configured connector %u / CRTC %u, preserving mode %s\n",
               connector->connector_id, crtc->crtc_id, crtc->mode.name);
        drmModeObjectProperties *properties =
            drmModeObjectGetProperties(drm_fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC);
        if (!properties) { perror("drmModeObjectGetProperties"); goto cleanup; }
        uint32_t active_property = 0;
        uint32_t mode_property = 0;
        for (uint32_t i = 0; i < properties->count_props; ++i) {
            drmModePropertyRes *property = drmModeGetProperty(drm_fd, properties->props[i]);
            if (!property) {
                perror("drmModeGetProperty");
                drmModeFreeObjectProperties(properties);
                goto cleanup;
            }
            if (strcmp(property->name, "ACTIVE") == 0) {
                active_property = property->prop_id;
                printf("NVDC CRTC ACTIVE before stream: %llu\n",
                       (unsigned long long)properties->prop_values[i]);
            }
            if (strcmp(property->name, "MODE_ID") == 0) mode_property = property->prop_id;
            drmModeFreeProperty(property);
        }
        drmModeFreeObjectProperties(properties);
        if (!active_property || !mode_property) {
            fprintf(stderr, "Missing CRTC ACTIVE or MODE_ID property\n");
            goto cleanup;
        }
        /* Legacy modesetting does not initialize NVDC's atomic ACTIVE state. */
        if (drmModeCreatePropertyBlob(drm_fd, &crtc->mode, sizeof(crtc->mode), &mode_blob) != 0) {
            perror("Creating mode property blob");
            goto cleanup;
        }
        drmModeAtomicReq *request = drmModeAtomicAlloc();
        if (!request) { perror("drmModeAtomicAlloc"); goto cleanup; }
        if (drmModeAtomicAddProperty(request, crtc->crtc_id, active_property, 1) < 0 ||
            drmModeAtomicAddProperty(request, crtc->crtc_id, mode_property, mode_blob) < 0) {
            fprintf(stderr, "Adding CRTC atomic properties failed\n");
            drmModeAtomicFree(request);
            goto cleanup;
        }
        int commit = drmModeAtomicCommit(drm_fd, request, 0, NULL);
        drmModeAtomicFree(request);
        if (commit != 0) { perror("Committing CRTC ACTIVE and MODE_ID"); goto cleanup; }
        printf("Committed CRTC ACTIVE=1 and existing mode to NVDC atomic state\n");
    }
    const EGLint display_attributes[] = { EGL_DRM_MASTER_FD_EXT, drm_fd, EGL_NONE };
    display = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devices[0],
                                       configure_output ? display_attributes : NULL);
    CHECK(display != EGL_NO_DISPLAY, "No EGLDevice display");
    CHECK(eglInitialize(display, NULL, NULL), "EGL initialization failed");
    printf("EGL vendor: %s\n", eglQueryString(display, EGL_VENDOR));
    EGLOutputLayerEXT layers[16];
    CHECK(eglGetOutputLayersEXT(display, NULL, layers, 16, &count) && count > 0,
          "No EGL output layers");
    if (count > 16) { fprintf(stderr, "Too many output layers\n"); goto cleanup; }
    int layer_index = configure_output ? -1 : 0;
    for (int i = 0; i < count; ++i) {
        EGLAttrib crtc_id = 0;
        if (eglQueryOutputLayerAttribEXT(display, layers[i], EGL_DRM_CRTC_EXT, &crtc_id)) {
            printf("Layer %d: CRTC %ld\n", i, (long)crtc_id);
            if (configure_output && crtc_id == crtc->crtc_id) layer_index = i;
        } else {
            EGLint error = eglGetError();
            if (error != EGL_BAD_MATCH) {
                fprintf(stderr, "Querying output layer %d failed (EGL error 0x%x)\n", i, error);
                goto cleanup;
            }
            printf("Layer %d: non-CRTC layer\n", i);
        }
    }
    if (layer_index < 0) { fprintf(stderr, "No EGL layer matches connected CRTC\n"); goto cleanup; }
    printf("Output layers: %d; testing layer %d at %dx%d\n", count, layer_index, WIDTH, HEIGHT);
    const EGLint stream_attributes[] = { EGL_STREAM_FIFO_LENGTH_KHR, 0, EGL_NONE };
    stream = eglCreateStreamKHR(display, stream_attributes);
    CHECK(stream != EGL_NO_STREAM_KHR, "Creating EGLStream failed");
    CHECK(eglStreamConsumerOutputEXT(display, stream, layers[layer_index]), "Connecting output layer failed");
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_STREAM_BIT_KHR,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE
    };
    EGLConfig config;
    CHECK(eglChooseConfig(display, config_attributes, &config, 1, &count) && count > 0,
          "No GLES3 stream configuration");
    CHECK(eglBindAPI(EGL_OPENGL_ES_API), "Binding GLES failed");
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    CHECK(context != EGL_NO_CONTEXT, "Creating GLES context failed");
    const EGLint surface_attributes[] = { EGL_WIDTH, WIDTH, EGL_HEIGHT, HEIGHT, EGL_NONE };
    surface = eglCreateStreamProducerSurfaceKHR(display, config, stream, surface_attributes);
    CHECK(surface != EGL_NO_SURFACE, "Creating stream surface failed");
    CHECK(eglMakeCurrent(display, surface, surface, context), "Making context current failed");
    printf("Renderer: %s\nGLES: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    input = open("/dev/input/by-id/usb-BIQU_BTT-HDMI7_SN0000000003-event-if00",
                 O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (input < 0) {
        perror("Opening BTT touchscreen");
        goto cleanup;
    }
    struct input_absinfo x_range, y_range;
    if (ioctl(input, EVIOCGABS(ABS_MT_POSITION_X), &x_range) < 0 ||
        ioctl(input, EVIOCGABS(ABS_MT_POSITION_Y), &y_range) < 0) {
        perror("Reading multitouch coordinate ranges");
        goto cleanup;
    }
    if (x_range.maximum <= x_range.minimum || y_range.maximum <= y_range.minimum) {
        fprintf(stderr, "Invalid touch coordinate ranges\n");
        goto cleanup;
    }
    printf("Touch X: %d..%d; Y: %d..%d\n", x_range.minimum, x_range.maximum,
           y_range.minimum, y_range.maximum);
    printf("Look for red/green/blue bars, white corner squares and a moving bottom marker.\n"
           "Tap the corners and center. A yellow square should follow your finger.\n");
    glEnable(GL_SCISSOR_TEST);
    int x = 0, y = 0, slot = 0;
    bool touching = false, changed = false;
    unsigned frames = 0, touches = 0;
    const double start = seconds();
    while (!stopped && seconds() - start < 90) {
        struct input_event event;
        ssize_t bytes;
        while ((bytes = read(input, &event, sizeof(event))) == sizeof(event)) {
            if (event.type == EV_SYN && event.code == SYN_DROPPED) {
                fprintf(stderr, "Touch event overflow; rerun the diagnostic\n");
                goto cleanup;
            }
            if (event.type == EV_ABS) {
                if (event.code == ABS_MT_SLOT) slot = event.value;
                if (slot == 0) {
                    if (event.code == ABS_MT_POSITION_X) { x = event.value; changed = true; }
                    if (event.code == ABS_MT_POSITION_Y) { y = event.value; changed = true; }
                    if (event.code == ABS_MT_TRACKING_ID) {
                        touching = event.value >= 0;
                        if (touching) ++touches;
                        changed = true;
                    }
                }
            }
            if (event.type == EV_SYN && event.code == SYN_REPORT && changed) {
                printf("Touch %s: raw=(%d,%d), screen=(%d,%d)\n",
                       touching ? "down/move" : "up", x, y,
                       (x - x_range.minimum) * (WIDTH - 1) / (x_range.maximum - x_range.minimum),
                       (y - y_range.minimum) * (HEIGHT - 1) / (y_range.maximum - y_range.minimum));
                changed = false;
            }
        }
        if (bytes >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            if (bytes < 0) perror("Reading touchscreen");
            else fprintf(stderr, "Short touchscreen read: %zd bytes\n", bytes);
            goto cleanup;
        }
        rectangle(0, 0, WIDTH, HEIGHT, 0.04f, 0.06f, 0.09f);
        rectangle(32, 80, 320, 400, 0.8f, 0.05f, 0.05f);
        rectangle(352, 80, 320, 400, 0.05f, 0.8f, 0.05f);
        rectangle(672, 80, 320, 400, 0.05f, 0.05f, 0.8f);
        rectangle(0, 0, 24, 24, 1, 1, 1);
        rectangle(WIDTH - 24, 0, 24, 24, 1, 1, 1);
        rectangle(0, HEIGHT - 24, 24, 24, 1, 1, 1);
        rectangle(WIDTH - 24, HEIGHT - 24, 24, 24, 1, 1, 1);
        rectangle(WIDTH / 2 - 8, HEIGHT / 2 - 8, 16, 16, 1, 1, 1);
        rectangle((frames * 5) % (WIDTH - 40), HEIGHT - 60, 40, 20, 0, 1, 1);
        if (touching) {
            int px = (x - x_range.minimum) * (WIDTH - 1) / (x_range.maximum - x_range.minimum);
            int py = (y - y_range.minimum) * (HEIGHT - 1) / (y_range.maximum - y_range.minimum);
            rectangle(px - 15, py - 15, 30, 30, 1, 1, 0);
        }
        if (glGetError() != GL_NO_ERROR) {
            fprintf(stderr, "GLES drawing failed\n");
            goto cleanup;
        }
        CHECK(eglSwapBuffers(display, surface), "Swapping stream buffers failed");
        if (++frames == 1) {
            EGLint state;
            CHECK(eglQueryStreamKHR(display, stream, EGL_STREAM_STATE_KHR, &state),
                  "Querying stream state failed");
            printf("First frame submitted; stream state: 0x%x\n", state);
        }
        struct timespec delay = { .tv_nsec = 16000000 };
        nanosleep(&delay, NULL);
    }
    printf("Finished: %u frames, %u primary-finger contacts. Visual confirmation required.\n",
           frames, touches);
    result = EXIT_SUCCESS;

cleanup:
    if (input >= 0) close(input);
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (stream != EGL_NO_STREAM_KHR) eglDestroyStreamKHR(display, stream);
        eglTerminate(display);
    }
    if (mode_blob && drmModeDestroyPropertyBlob(drm_fd, mode_blob) != 0) {
        perror("Destroying mode property blob");
        result = EXIT_FAILURE;
    }
    if (crtc) drmModeFreeCrtc(crtc);
    if (encoder) drmModeFreeEncoder(encoder);
    if (connector) drmModeFreeConnector(connector);
    if (resources) drmModeFreeResources(resources);
    if (drm_fd >= 0 && drmClose(drm_fd) != 0) {
        perror("drmClose");
        result = EXIT_FAILURE;
    }
    return result;
}
