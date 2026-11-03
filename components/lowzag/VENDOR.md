# lowzag（渐进式 JPEG 解码器）vendor 记录 / Vendoring record

- 组件名 / Component: `defensiblelogic/lowzag`
- 版本 / Version: **2.2.1**
- 上游仓库 / Upstream repo: <https://github.com/lowzag/lowzag.git>
- 项目主页 / Homepage: <https://gitlab.defensiblelogic.com/pub/lowzag>
- 上游 commit / Upstream commit: `05fc50bb3f909d439742719b256671f9bfce15d2`
- ESP 组件仓库下载地址 / Registry download:
  <https://components-file.espressif.com/components/defensiblelogic/lowzag/2.2.1/defensiblelogic__lowzag-v2.2.1.zip>
- 组件仓库记录 hash / Registry component hash:
  `6c103bf81dfb1c5e49c25713d084c1695b8944ff61cb5b81f8634069dd1d9a60`
  （与上游 `dependencies.lock` 中 rc103 使用的同版本一致 / same version rc103 locks）
- 许可证 / License: MIT（见 `LICENSE`，Copyright (c) 2026 Tom, defensiblelogic.com）
- 获取日期 / Fetched: 2026-11-01（经本地代理 `http://127.0.0.1:7897`）
- 下载 zip 的 sha256 / zip sha256:
  `011daee436650bef03fd4a21552be43ac157fbed71793731d0b34d1b296893eb`

## 收录文件 / Files vendored

只收录解码核心与许可证；未收录 `examples/`、`docs/`、`CMakeLists.txt`（上游独立库构建脚本由
本目录最小 CMakeLists 取代）、`idf_component.yml` 等非运行文件。
Only the decoder core and licence are vendored; `examples/`, `docs/`, upstream standalone
`CMakeLists.txt` and `idf_component.yml` are omitted.

| 文件 / File | 大小 / Size | sha256（与官方 CHECKSUMS.json 一致 / matches official CHECKSUMS.json） |
| --- | --- | --- |
| `include/lowzag.h` | 52,039 | `717c7939eb27bef5b7cbf24ef42cc01eedad934c4771debe86e7b84fb9322d2c` |
| `src/lowzag.c` | 108,176 | `a5cde172134a3be0b24e433c9500891936cf028096fa6e69f8fa785c5142926a` |
| `src/lowzag_mem.c` | 12,414 | `b7e8467c840192f3aa04930629c0664694580bcb2d04481e0b789e512c0c763a` |
| `LICENSE` | 1,082 | `31d6f2be03751dce1258d616f99d808349c1cc13444eb12c33d662956c82a112` |

官方校验源 / Official checksum source:
<https://components-file.espressif.com/components/defensiblelogic/lowzag/2.2.1/CHECKSUMS.json>

## 本地改动 / Local changes

无：上述源码与官方发布逐字节一致；仅新增本 CMakeLists 与 VENDOR.md。
None: the sources are byte-identical to the official release; only this CMakeLists and VENDOR.md
are added here.
