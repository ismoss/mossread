# read_pico_webp

中文：WebP 解码组件。固件只在这里放 libwebp 的**解码器子集**，供 EPUB 插图/封面和 TF 图片路径使用；编码器、动画与增量接口都没有编进来。

English: WebP decoding component. Only libwebp's **decoder subset** lives here for EPUB illustrations/covers and TF image paths; the encoder, animation and incremental APIs are not built.

## 来源 / Provenance

- 上游 / Upstream: [libwebp 1.5.0](https://chromium.googlesource.com/webm/libwebp)，BSD-3-Clause，版权归 Google Inc.。
- 未改源码 / Unmodified sources: `libwebp/src/dec/`、`libwebp/src/dsp/`（仅通用 C 实现用到的文件）、`libwebp/src/utils/`、`libwebp/src/enc/*.h`（`dsp/lossless.h` 需要其中的结构定义）与 `libwebp/src/webp/` 头文件。
- 许可 / License: [LICENSE](LICENSE)（上游 `COPYING`）。

## 只编了什么 / What is compiled

- `src/dec/*.c`：VP8、VP8L、alpha 与容器解码。
- `src/dsp/`：`alpha_processing`、`cpu`、`dec`、`dec_clip_tables`、`filters`、`lossless`、`rescaler`、`upsampling`、`yuv`。
- `src/utils/`：`bit_reader_utils`、`color_cache_utils`、`filters_utils`、`huffman_utils`、`palette`、`quant_levels_dec_utils`、`rescaler_utils`、`random_utils`、`thread_utils`、`utils`。
- 没有编 SIMD、demux/ANIM、编码器；X86 主机测试用 `-U__SSE2__` 等把 SIMD 检测关掉，因为仓库只带通用 C 实现。

Only the generic C sources above are built. SIMD, demux/ANIM and the encoder are excluded; the x86 host test disables SIMD detection because only the generic C paths are vendored.

## 对外接口 / Public surface

见 [include/read_pico_webp.h](include/read_pico_webp.h)：签名判断、只读头部尺寸、按像素上限解码成 RGB565 平面。裁剪、缩放与灰度换算仍由 `main/book/book_cover.c` 统一负责，这一层不做第二套。

See [include/read_pico_webp.h](include/read_pico_webp.h): signature check, header-only dimensions, and a pixel-capped RGB565 decode. Cropping, scaling and grayscale stay in `main/book/book_cover.c`; this layer owns no second implementation.

## 体积 / Footprint

ESP32-S3、`-Os`：约 46 KB `.text` + 9.5 KB `.rodata`，约 0.7 KB DIRAM（`idf.py size-components` 实测）。整包因此增加约 64 KB。

ESP32-S3 at `-Os`: about 46 KB `.text`, 9.5 KB `.rodata` and 0.7 KB DIRAM (measured with `idf.py size-components`); the whole image grows by about 64 KB.

## 升级上游 / Updating upstream

1. 取新版本 tarball，按上面的文件清单替换 `libwebp/` 与 `LICENSE`；
2. 同步更新根目录 `THIRD_PARTY_NOTICES.md` 的版本号；
3. 跑 `python tools/webp_host_test.py`（头部尺寸、RGB565 解码、动画拒绝、限幅缩放、EPUB 取图）；
4. 重新 `idf.py build`，确认 `Read_Pico.bin` 仍小于 0x400000 字节的应用分区。

Re-vendor the listed files from the new tarball, bump the version in `THIRD_PARTY_NOTICES.md`, run the host test, and rebuild while checking the app partition bound.
