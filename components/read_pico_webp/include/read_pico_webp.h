/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：WebP 解码能力的最小外壳。只提供 EPUB/TF 图片路径需要的两件事：读头部尺寸、
 * 解码成 RGB565 平面；不暴露 libwebp 的编码器、动画与增量接口。
 *
 * English: Minimal WebP decoding surface. It offers only what the EPUB/TF image paths need:
 * header dimensions and an RGB565 plane; libwebp's encoder, animation and incremental APIs stay
 * out of this interface.
 *
 * 冻结：解码结果只写 PSRAM；不引入第二套缩放或裁剪逻辑（由 book_cover.c 统一做）。
 * Frozen: decoded planes live in PSRAM only; no second scaling/cropping implementation (book_cover.c
 * owns that).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// 头部信息；读尺寸不解码正文。/ Bitstream information; reading sizes never decodes pixels.
typedef struct {
    unsigned width, height;
    bool has_alpha;
    bool has_animation;
} read_pico_webp_info_t;

/// 默认源平面像素上限（RGB565 约 3 MiB）：更宽的图按比例先缩到上限再交给调用方。
/// Default source-plane pixel cap (about 3 MiB as RGB565); wider images are pre-scaled to it.
#define READ_PICO_WEBP_DEFAULT_MAX_PIXELS (1536u * 1024u)

/// 是否是 WebP（RIFF....WEBP）。/ Whether the bytes are WebP (RIFF....WEBP).
bool read_pico_webp_is(const uint8_t *data, size_t size);

/// 只读头部：宽高、alpha、是否动画；失败时不保证 out 被写。/ Header only: size, alpha and animation.
bool read_pico_webp_info(const uint8_t *data, size_t size, read_pico_webp_info_t *out);

/// 解成 RGB565 平面（PSRAM）。动画与超出 max_pixels 的图会被拒绝或先等比缩小；
/// 调用方 free(*out)。max_pixels 为 0 时用 READ_PICO_WEBP_DEFAULT_MAX_PIXELS。
/// Decode to an RGB565 plane in PSRAM. Animated images are rejected and oversized images are
/// scaled down to max_pixels first; the caller frees *out.
bool read_pico_webp_rgb565(const uint8_t *data, size_t size, size_t max_pixels,
                           uint8_t **out, unsigned *width, unsigned *height);
