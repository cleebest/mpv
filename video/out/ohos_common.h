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

#pragma once

#include <libavutil/buffer.h>
#include <native_window/external_window.h>

#include "common/common.h"

struct vo;
struct mp_image;

bool vo_ohos_init(struct vo *vo);
void vo_ohos_uninit(struct vo *vo);
OHNativeWindow *vo_ohos_native_window(struct vo *vo);
bool vo_ohos_surface_size(struct vo *vo, int *w, int *h);

// Create an OHCodec hardware device reference for the given VO.
// Calls vo_ohos_init() internally; on failure the caller must NOT call
// vo_ohos_uninit() (it is handled here).  Returns NULL on error.
AVBufferRef *vo_ohos_create_ohcodec_device_ref(struct vo *vo);

// Surface 模式帧丢弃：在 mp_image_unrefp() 之前调用，使 oh_buffer_release()
// 调用 FreeOutputBuffer 而非 RenderOutputBuffer，立即归还解码器输出槽。
// 用于高倍速播放时防止 NativeWindow 队列回压阻塞解码器。
// 传入非 OHCODEC 帧或 NULL 是安全的 no-op。
void vo_ohos_ohcodec_discard(struct mp_image *mpi);
