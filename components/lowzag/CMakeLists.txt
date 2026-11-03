# lowzag 2.2.1（渐进式 JPEG 解码器）的 vendor 副本，只编译解码核心，不含示例/测试。
# Vendored copy of lowzag 2.2.1 (progressive JPEG decoder); decoder core only, no examples/tests.
#
# 来源 / Source: https://components.espressif.com/components/defensiblelogic/lowzag/2.2.1
# 版本 / Version: 2.2.1，上游仓库 commit 05fc50bb3f909d439742719b256671f9bfce15d2
# 许可证 / License: MIT，见同目录 LICENSE
# 详见 VENDOR.md（含各文件 sha256 与获取日期）。
# See VENDOR.md for per-file sha256 and fetch date.
#
# src/lowzag_mem.c 只提供 lowzag_decode_mem()/lowzag_exif_orientation()，本工程不调用；
# 静态库未引用的成员不会进镜像，所以保留上游完整性而不增加 Flash 占用。
# src/lowzag_mem.c only provides lowzag_decode_mem()/lowzag_exif_orientation(), unused here;
# an unreferenced archive member is not linked into the image, so upstream stays complete at no
# flash cost.
idf_component_register(
    SRCS "src/lowzag.c" "src/lowzag_mem.c"
    INCLUDE_DIRS "include"
)

# 上游 C99 源码保持原样：只压体积、不让它的告警升级成本工程的错误（本工程默认 -Werror）。
# Upstream C99 sources stay unmodified: shrink only, and never promote their warnings to this
# project's errors (this tree builds with -Werror by default).
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -w -Wno-error)
