// SPDX-License-Identifier: GPL-3.0-only
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int main(void) {
    int fd = drmOpen("drm-nvdc", NULL);
    if (fd < 0) {
        perror("drmOpen(drm-nvdc)");
        return EXIT_FAILURE;
    }
    int result = EXIT_FAILURE;
    drmModeRes *resources = drmModeGetResources(fd);
    if (!resources) {
        perror("drmModeGetResources");
        goto cleanup;
    }
    printf("NVDC: %d CRTCs, %d connectors, %d encoders\n",
           resources->count_crtcs, resources->count_connectors, resources->count_encoders);
    for (int i = 0; i < resources->count_crtcs; ++i) {
        drmModeCrtc *crtc = drmModeGetCrtc(fd, resources->crtcs[i]);
        if (!crtc) { perror("drmModeGetCrtc"); goto cleanup; }
        printf("CRTC %u: framebuffer=%u, mode_valid=%d, size=%ux%u, mode=%s\n",
               crtc->crtc_id, crtc->buffer_id, crtc->mode_valid,
               crtc->width, crtc->height, crtc->mode.name);
        drmModeObjectProperties *properties =
            drmModeObjectGetProperties(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC);
        if (!properties) {
            perror("drmModeObjectGetProperties");
            drmModeFreeCrtc(crtc);
            goto cleanup;
        }
        for (uint32_t j = 0; j < properties->count_props; ++j) {
            drmModePropertyRes *property = drmModeGetProperty(fd, properties->props[j]);
            if (!property) {
                perror("drmModeGetProperty");
                drmModeFreeObjectProperties(properties);
                drmModeFreeCrtc(crtc);
                goto cleanup;
            }
            printf("  %s=%" PRIu64 " (property %u)\n",
                   property->name, properties->prop_values[j], property->prop_id);
            drmModeFreeProperty(property);
        }
        drmModeFreeObjectProperties(properties);
        drmModeFreeCrtc(crtc);
    }
    for (int i = 0; i < resources->count_connectors; ++i) {
        drmModeConnector *connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (!connector) { perror("drmModeGetConnector"); goto cleanup; }
        printf("Connector %u: connection=%d, encoder=%u, modes=%d\n",
               connector->connector_id, connector->connection,
               connector->encoder_id, connector->count_modes);
        for (int j = 0; j < connector->count_modes; ++j) {
            drmModeModeInfo *mode = &connector->modes[j];
            printf("  mode %s: %ux%u @ %u Hz, clock=%u, type=0x%x\n",
                   mode->name, mode->hdisplay, mode->vdisplay, mode->vrefresh,
                   mode->clock, mode->type);
        }
        for (int j = 0; j < connector->count_props; ++j) {
            drmModePropertyRes *property = drmModeGetProperty(fd, connector->props[j]);
            if (!property) {
                perror("drmModeGetProperty");
                drmModeFreeConnector(connector);
                goto cleanup;
            }
            printf("  %s=%" PRIu64 "\n", property->name, connector->prop_values[j]);
            drmModeFreeProperty(property);
        }
        drmModeFreeConnector(connector);
    }
    for (int i = 0; i < resources->count_encoders; ++i) {
        drmModeEncoder *encoder = drmModeGetEncoder(fd, resources->encoders[i]);
        if (!encoder) { perror("drmModeGetEncoder"); goto cleanup; }
        printf("Encoder %u: CRTC=%u, possible_crtcs=0x%x\n",
               encoder->encoder_id, encoder->crtc_id, encoder->possible_crtcs);
        drmModeFreeEncoder(encoder);
    }
    result = EXIT_SUCCESS;
cleanup:
    if (resources) drmModeFreeResources(resources);
    if (drmClose(fd) != 0) {
        perror("drmClose");
        result = EXIT_FAILURE;
    }
    return result;
}
