# libwebp 1.5.0 的解码器子集：只编 VP8/VP8L/alpha 解码与它依赖的 dsp、utils。
# libwebp 1.5.0 decoder subset: VP8/VP8L/alpha decoding plus the dsp and utils it needs.
set(webp_decoder_sources
    "libwebp/src/dec/alpha_dec.c"
    "libwebp/src/dec/buffer_dec.c"
    "libwebp/src/dec/frame_dec.c"
    "libwebp/src/dec/idec_dec.c"
    "libwebp/src/dec/io_dec.c"
    "libwebp/src/dec/quant_dec.c"
    "libwebp/src/dec/tree_dec.c"
    "libwebp/src/dec/vp8_dec.c"
    "libwebp/src/dec/vp8l_dec.c"
    "libwebp/src/dec/webp_dec.c"
    "libwebp/src/dsp/alpha_processing.c"
    "libwebp/src/dsp/cpu.c"
    "libwebp/src/dsp/dec.c"
    "libwebp/src/dsp/dec_clip_tables.c"
    "libwebp/src/dsp/filters.c"
    "libwebp/src/dsp/lossless.c"
    "libwebp/src/dsp/rescaler.c"
    "libwebp/src/dsp/upsampling.c"
    "libwebp/src/dsp/yuv.c"
    "libwebp/src/utils/bit_reader_utils.c"
    "libwebp/src/utils/color_cache_utils.c"
    "libwebp/src/utils/filters_utils.c"
    "libwebp/src/utils/huffman_utils.c"
    "libwebp/src/utils/quant_levels_dec_utils.c"
    "libwebp/src/utils/palette.c"
    "libwebp/src/utils/rescaler_utils.c"
    "libwebp/src/utils/random_utils.c"
    "libwebp/src/utils/thread_utils.c"
    "libwebp/src/utils/utils.c"
)

idf_component_register(
    SRCS "src/read_pico_webp.c" ${webp_decoder_sources}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "libwebp"
    PRIV_REQUIRES heap
)

# 上游源码保留原样：只压体积、不把它的告警当成本工程的错误。
# Upstream stays unmodified: shrink only, and never promote its warnings to this project's errors.
set_source_files_properties(${webp_decoder_sources} PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
