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

#include "ohos_common.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "vo.h"
#include "video/mp_image.h"

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_oh.h>

// Forward declaration of FFmpeg internal function (defined in libavcodec/ohdec.c).
// ohcodec.h is an internal FFmpeg header not installed to the public include path,
// so we declare the symbol directly to avoid the missing-header error.
// The function is guaranteed to be present when OHCodec video decode is enabled.
void ff_ohcodec_discard_buffer(void *ohcodec_buf_ptr);

struct vo_ohos_state {
    struct mp_log *log;
    OHNativeWindow *native_window;
};

bool vo_ohos_init(struct vo *vo)
{
    vo->ohos = talloc_zero(vo, struct vo_ohos_state);
    struct vo_ohos_state *ctx = vo->ohos;

    *ctx = (struct vo_ohos_state){
        .log = mp_log_new(ctx, vo->log, "ohos"),
    };

    if (vo->opts->WinID == 0 || vo->opts->WinID == -1) {
        MP_FATAL(ctx, "Missing surface pointer\n");
        goto fail;
    }

    // WinID is int64_t from mpv options, surface ID is uint64_t.
    // On OHOS, surface IDs are always non-negative, so direct cast is safe.
    uint64_t surface = (uint64_t)vo->opts->WinID;
    OH_NativeWindow_CreateNativeWindowFromSurfaceId(surface, &ctx->native_window);
    if (!ctx->native_window) {
        MP_FATAL(ctx, "Failed to create OHNativeWindow\n");
        goto fail;
    }

    return true;
fail:
    talloc_free(ctx);
    vo->ohos = NULL;
    return false;
}

void vo_ohos_uninit(struct vo *vo)
{
    struct vo_ohos_state *ctx = vo->ohos;
    if (!ctx)
        return;

    if (ctx->native_window)
        OH_NativeWindow_DestroyNativeWindow(ctx->native_window);

    talloc_free(ctx);
    vo->ohos = NULL;
}

OHNativeWindow *vo_ohos_native_window(struct vo *vo)
{
    struct vo_ohos_state *ctx = vo->ohos;
    return ctx->native_window;
}

bool vo_ohos_surface_size(struct vo *vo, int *out_w, int *out_h)
{
    struct vo_ohos_state *ctx = vo->ohos;

    int w = vo->opts->ohos_surface_size.w,
        h = vo->opts->ohos_surface_size.h;
    if (!w || !h)
        OH_NativeWindow_NativeWindowHandleOpt(ctx->native_window, GET_BUFFER_GEOMETRY, &h, &w);

    if (w <= 0 || h <= 0) {
        MP_ERR(ctx, "Failed to get height and width.\n");
        return false;
    }
    *out_w = w;
    *out_h = h;
    return true;
}

AVBufferRef *vo_ohos_create_ohcodec_device_ref(struct vo *vo)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_OHCODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVOHCodecDeviceContext *hwctx = ctx->hwctx;

    if (!vo_ohos_init(vo)) {
        av_buffer_unref(&device_ref);
        return NULL;
    }

    hwctx->native_window = vo_ohos_native_window(vo);

    if (av_hwdevice_ctx_init(device_ref) < 0) {
        av_buffer_unref(&device_ref);
        vo_ohos_uninit(vo);
        return NULL;
    }

    return device_ref;
}

void vo_ohos_ohcodec_discard(struct mp_image *mpi)
{
    if (!mpi || mpi->imgfmt != IMGFMT_OHCODEC)
        return;
    // For AV_PIX_FMT_OHCODEC frames, AVFrame->data[3] (= mp_image->planes[3])
    // points to the OHCodecBuffer struct inside FFmpeg's ohdec.c.
    // ff_ohcodec_discard_buffer() calls FreeOutputBuffer directly and marks the
    // buffer as rendered so oh_buffer_release() won't touch it again.
    ff_ohcodec_discard_buffer(mpi->planes[3]);
}
