/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <libavcodec/codec.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_oh.h>
#include <native_window/external_window.h>

#include "config.h"

#include "video/out/gpu/hwdec.h"
#include "video/out/ohos_common.h"
#include "common/common.h"
#include "filters/f_decoder_wrapper.h"

#if HAVE_GL
#include "video/out/opengl/ra_gl.h"
#endif

// Reference implementation: hwdec_aimagereader.c for Android MediaCodec
// OHCodec works similarly - it can output to:
// 1. Surface mode: Direct rendering to native_window (vo=ohcodec)
// 2. Buffer mode: Output to system memory buffers (vo=gpu/gpu-next)
//    BUT: Even buffer mode requires a native_window for FFmpeg's ohcodec

struct priv_owner {
    struct mp_hwdec_ctx hwctx;
    OHNativeWindow *window;  // Required even for buffer mode
};

struct priv {
    struct ra_imgfmt_desc desc;
    int num_planes;
    struct ra_tex *tex[4];

    // Cache previous frame dimensions to avoid texture recreation
    int cached_w, cached_h;
};

static AVBufferRef *create_ohcodec_device_ref(OHNativeWindow *window)
{
    // Create device with native_window
    // FFmpeg's ohcodec requires window even for buffer mode output
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_OHCODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVOHCodecDeviceContext *hwctx = ctx->hwctx;
    hwctx->native_window = window;  // Must provide window

    if (av_hwdevice_ctx_init(device_ref) < 0) {
        av_buffer_unref(&device_ref);
        return NULL;
    }

    return device_ref;
}

static void uninit(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    hwdec_devices_remove(hw->devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);

    // Destroy native_window if we created it
    if (p->window) {
        OH_NativeWindow_DestroyNativeWindow(p->window);
        p->window = NULL;
    }
}

static int init(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;
    struct vo *vo = hw->ra_ctx->vo;

    // In surface mode (vo=ohcodec or vo=ohcodec-osd), the VO itself registers
    // the hwctx directly via hwdec_devices_add in preinit(). This RA hwdec
    // path must NOT register a second hwctx or it will conflict with the VO's
    // hwctx and confuse vd_lavc into failing hardware decode.
    // Surface mode VOs are identified by vo->driver->name being "ohcodec" or
    // "ohcodec-osd".
    if (vo && vo->driver && vo->driver->name) {
        if (strcmp(vo->driver->name, "ohcodec") == 0 ||
            strcmp(vo->driver->name, "ohcodec-osd") == 0) {
            MP_VERBOSE(hw, "Surface mode VO (%s) detected, "
                       "hwctx already registered by VO - skipping RA hwdec init\n",
                       vo->driver->name);
            return -1;
        }
    }

    // GPU output mode (vo=gpu/gpu-next): need a native_window for OHCodec.
    // Currently not supported since we have no surface ID in GPU mode.
    if (!vo || !vo->ohos) {
        MP_VERBOSE(hw, "OHCodec RA hwdec requires an OHOS VO\n");
        return -1;
    }

    p->window = vo_ohos_native_window(vo);
    if (!p->window) {
        MP_ERR(hw, "OHCodec: no native_window available from VO\n");
        return -1;
    }

    // Create ohcodec device with native_window
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = hw->driver->name,
        .av_device_ref = create_ohcodec_device_ref(p->window),
        .hw_imgfmt = IMGFMT_OHCODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(hw, "Failed to create ohcodec hwdevice_ctx\n");
        return -1;
    }

    hwdec_devices_add(hw->devs, &p->hwctx);

    MP_VERBOSE(hw, "OHCodec RA hwdec initialized\n");
    return 0;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;

    for (int n = 0; n < 4; n++)
        ra_tex_free(mapper->ra, &p->tex[n]);
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    // Textures are freed in mapper_unmap
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = mapper->src_params.hw_subfmt;
    mapper->dst_params.hw_subfmt = 0;

    if (!mapper->dst_params.imgfmt) {
        MP_ERR(mapper, "OHCodec: hw_subfmt is 0 - frame has no software format.\n");
        return -1;
    }

    // OHCodec buffer mode outputs 8-bit formats: NV12 or YUV420P
    int fmt = mapper->dst_params.imgfmt;
    if (fmt != IMGFMT_NV12 && fmt != IMGFMT_420P) {
        MP_ERR(mapper, "OHCodec: unsupported format %s (expected NV12 or YUV420P)\n",
               mp_imgfmt_to_name(fmt));
        return -1;
    }

    if (!ra_get_imgfmt_desc(mapper->ra, mapper->dst_params.imgfmt, &p->desc)) {
        MP_ERR(mapper, "OHCodec: unsupported texture format %s\n",
               mp_imgfmt_to_name(mapper->dst_params.imgfmt));
        return -1;
    }

    p->num_planes = p->desc.num_planes;
    p->cached_w = 0;
    p->cached_h = 0;

    MP_VERBOSE(mapper, "OHCodec: mapping format=%s, %d planes, size=%dx%d\n",
               mp_imgfmt_to_name(fmt), p->num_planes,
               mapper->src_params.w, mapper->src_params.h);

    return 0;
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct ra *ra = mapper->ra;

    // OHCodec buffer mode: AVFrame contains system memory buffers (NV12/YUV420P)
    // We need to upload these to GPU textures for rendering
    // Reference: hwdec_vaapi.c, hwdec_vt.c (similar sw-download path)

    // Check if frame dimensions changed - only recreate textures if needed
    bool dimensions_changed = (p->cached_w != mapper->src->w ||
                               p->cached_h != mapper->src->h);

    if (dimensions_changed) {
        MP_VERBOSE(mapper, "OHCodec: frame dimensions changed %dx%d -> %dx%d, recreating textures\n",
                   p->cached_w, p->cached_h, mapper->src->w, mapper->src->h);
        // Free old textures
        for (int n = 0; n < 4; n++)
            ra_tex_free(ra, &p->tex[n]);
        p->cached_w = mapper->src->w;
        p->cached_h = mapper->src->h;
    }

    for (int n = 0; n < p->num_planes; n++) {
        if (!p->tex[n]) {
            // Create GPU texture for this plane
            struct ra_tex_params params = {
                .dimensions = 2,
                .w = mp_image_plane_w(mapper->src, n),
                .h = mp_image_plane_h(mapper->src, n),
                .d = 1,
                .format = p->desc.planes[n],
                .render_src = true,
                .src_linear = true,
                .host_mutable = true,
            };

            p->tex[n] = ra_tex_create(ra, &params);
            if (!p->tex[n]) {
                MP_ERR(mapper, "Failed to create texture for plane %d\n", n);
                return -1;
            }
        }

        // Upload plane data from AVFrame to GPU texture
        struct ra_tex_upload_params upload_params = {
            .tex = p->tex[n],
            .src = mapper->src->planes[n],
            .stride = mapper->src->stride[n],
        };

        if (!ra->fns->tex_upload(ra, &upload_params)) {
            MP_ERR(mapper, "Failed to upload plane %d\n", n);
            return -1;
        }

        mapper->tex[n] = p->tex[n];
    }

    return 0;
}

const struct ra_hwdec_driver ra_hwdec_ohcodec = {
    .name = "ohcodec",
    .priv_size = sizeof(struct priv_owner),
    .imgfmts = {IMGFMT_OHCODEC, 0},
    .device_type = AV_HWDEVICE_TYPE_OHCODEC,
    .init = init,
    .uninit = uninit,
    .mapper = &(const struct ra_hwdec_mapper_driver){
        .priv_size = sizeof(struct priv),
        .init = mapper_init,
        .uninit = mapper_uninit,
        .map = mapper_map,
        .unmap = mapper_unmap,
    },
};
