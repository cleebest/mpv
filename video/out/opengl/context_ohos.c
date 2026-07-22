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

#include <stdbool.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <native_window/external_window.h>
#include <native_vsync/native_vsync.h>

#include "video/out/ohos_common.h"
#include "egl_helpers.h"
#include "common/common.h"
#include "context.h"

struct priv {
    struct GL gl;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    // NativeVSync: 仅用于读取刷新率周期，不使用回调机制
    OH_NativeVSync *native_vsync;
    int64_t         vsync_period;  // vsync 周期 (ns)
};

static void ohos_swap_buffers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    eglSwapBuffers(p->egl_display, p->egl_surface);
    // 不使用 OH_NativeVSync_RequestFrame：该 API 是单次回调模式，
    // 快速 seek 期间高频调用会导致内部死锁。
    // vsync 时间戳由 ohos_get_vsync 中直接读取 wall clock 提供，
    // mpv 内部会做样本平均，精度足够。
}

static void ohos_get_vsync(struct ra_ctx *ctx, struct vo_vsync_info *info)
{
    struct priv *p = ctx->priv;

    // 刷新率查询：仅在 vsync_period 未知时查询一次
    if (p->native_vsync && p->vsync_period <= 0) {
        long long period = 0;
        if (OH_NativeVSync_GetPeriod(p->native_vsync, &period) == 0 && period > 0)
            p->vsync_period = (int64_t)period;
    }

    // 使用 wall clock 作为 last_queue_display_time；
    // vo.c 中若该值 <= 0 会自动补 mp_time_ns()，行为一致。
    // 这与旧版（native_vsync 未链接、p->native_vsync == NULL）完全相同。
}

static void ohos_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    ra_gl_ctx_uninit(ctx);

    if (p->native_vsync) {
        OH_NativeVSync_Destroy(p->native_vsync);
        p->native_vsync = NULL;
    }

    if (p->egl_surface) {
        eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroySurface(p->egl_display, p->egl_surface);
    }
    if (p->egl_context)
        eglDestroyContext(p->egl_display, p->egl_context);

    vo_ohos_uninit(ctx->vo);
}

static bool ohos_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);

    if (!vo_ohos_init(ctx->vo))
        goto fail;

    p->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(p->egl_display, NULL, NULL)) {
        MP_FATAL(ctx, "EGL failed to initialize.\n");
        goto fail;
    }

    EGLConfig config;
    if (!mpegl_create_context(ctx, p->egl_display, &p->egl_context, &config))
        goto fail;

    OHNativeWindow *native_window = vo_ohos_native_window(ctx->vo);
    p->egl_surface = eglCreateWindowSurface(p->egl_display, config,
                                    (EGLNativeWindowType)native_window, NULL);

    if (p->egl_surface == EGL_NO_SURFACE) {
        MP_FATAL(ctx, "Could not create EGL surface!\n");
        goto fail;
    }

    if (!eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface,
                        p->egl_context)) {
        MP_FATAL(ctx, "Failed to set context!\n");
        goto fail;
    }

    mpegl_load_functions(&p->gl, ctx->log);

    // 创建 NativeVSync 实例：仅用于查询刷新率周期（GetPeriod），
    // 不使用 RequestFrame 回调，避免快速 seek 时并发触发死锁
    const char *vsync_name = "mpv-ohos";
    p->native_vsync = OH_NativeVSync_Create(vsync_name, strlen(vsync_name));
    if (!p->native_vsync)
        MP_WARN(ctx, "Failed to create OH_NativeVSync, FPS detection will be estimated\n");

    struct ra_ctx_params params = {
        .swap_buffers = ohos_swap_buffers,
        .get_vsync    = ohos_get_vsync,
    };

    if (!ra_gl_ctx_init(ctx, &p->gl, params))
        goto fail;

    return true;
fail:
    ohos_uninit(ctx);
    return false;
}

static bool ohos_reconfig(struct ra_ctx *ctx)
{
    int w, h;
    if (!vo_ohos_surface_size(ctx->vo, &w, &h))
        return false;

    ctx->vo->dwidth = w;
    ctx->vo->dheight = h;
    ra_gl_ctx_resize(ctx->swapchain, w, h, 0);
    return true;
}

static int ohos_control(struct ra_ctx *ctx, int *events, int request, void *arg)
{
    struct priv *p = ctx->priv;
    switch (request) {
    case VOCTRL_GET_DISPLAY_FPS: {
        if (p->vsync_period > 0) {
            *(double *)arg = 1e9 / (double)p->vsync_period;
            return VO_TRUE;
        }
        return VO_NOTIMPL;
    }
    }
    return VO_NOTIMPL;
}

const struct ra_ctx_fns ra_ctx_ohos = {
    .type           = "opengl",
    .name           = "ohos",
    .description    = "HarmonyOS/EGL",
    .reconfig       = ohos_reconfig,
    .control        = ohos_control,
    .init           = ohos_init,
    .uninit         = ohos_uninit,
};
