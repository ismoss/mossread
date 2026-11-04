/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：荧光笔式划线记录。每本书一个隐藏 sidecar 文件，按「章 + 章内字节偏移」
 *       保存，与进度/书签使用同一锚点；字号、行距、横竖排、繁简切换都不影响位置。
 *       记录里带一小段原文摘录，用于工具栏「划线」列表离线显示。
 *       文件：/sdcard/.readpico/highlight/<pathhash>-hl.bin（内置书在 /flash 下同名目录）。
 *
 * English: Highlighter-style ranges. Each book owns one hidden sidecar keyed by
 *          chapter + chapter byte offset, the same anchors as progress and bookmarks, so
 *          font size, spacing, orientation and script switching never move a range.
 *          Each record keeps a short source excerpt for the toolbar highlight list.
 *          Path: /sdcard/.readpico/highlight/<pathhash>-hl.bin (internal books under /flash).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// 每本书最多保存的划线段数；约 108 字节/段（12 字节定位 + 96 字节摘录）。
/// Maximum ranges per book; about 108 bytes each (12 bytes of anchors plus a 96-byte excerpt).
#define BOOK_HIGHLIGHT_MAX 512
#define BOOK_HIGHLIGHT_EXCERPT 96
/// 每条划线可附一段短笔记；自由文本由阅读页键盘编辑。
/// Short free-text note attached to a range; edited with the reader keyboard.
#define BOOK_HIGHLIGHT_NOTE 80

/// 载入一本书的划线；没有文件时按空集合载入（之后仍可正常新增并保存）。
/// Load one book's ranges; a missing file loads as an empty set that can still grow and save.
bool book_highlight_load(const char *book_path);
/// 释放在阅状态；换书或离开阅读页时调用。/ Drop the active book state; call when switching books or leaving the reader.
void book_highlight_unload(void);
/// 当前书的记录条数。/ Number of ranges in the active book.
int book_highlight_count(void);

/// 某一行/一段文字范围是否与已保存的划线相交；chapter 与布局使用同一章节号。
/// Whether a line/range overlaps a saved highlight; chapter uses the same index as the layout.
bool book_highlight_overlaps(uint16_t chapter, uint32_t start, uint32_t end);
/// 取与给定范围相交的所有划线在 [start,end) 内的并集边界；没有则返回 false。
/// Union of saved ranges intersecting [start,end); false when nothing intersects.
bool book_highlight_range_at(uint16_t chapter, uint32_t start, uint32_t end,
                             uint32_t *out_start, uint32_t *out_end);

/// 追加一段并合并所有相交或相邻记录（长按拖动用，不删除已有划线）。
/// Append a range and merge every intersecting/adjacent record (long-press drag keeps existing marks).
bool book_highlight_add(uint16_t chapter, uint32_t start, uint32_t end, const char *excerpt);

/// 按序号读取记录（0 为最早）；摘录指针只在下次载入/修改前有效。
/// Read a record by index (0 is oldest); the excerpt pointer is valid until the next load/edit.
int book_highlight_at(int index, uint16_t *chapter, uint32_t *start, uint32_t *end,
                      const char **excerpt);
/// 删除一条记录并立即写回；返回是否成功。/ Remove one record and flush immediately.
bool book_highlight_remove(int index);

/// 预设标签（0 无、1 重要、2 待读、3 疑问）的读取与设置；标签随书 sidecar 保存。
/// Preset tag (0 none, 1 important, 2 to-read, 3 question) read/set; saved with the book sidecar.
uint8_t book_highlight_tag(int index);
bool book_highlight_set_tag(int index, uint8_t tag);

/// 划线笔记：读取返回静态字符串（下次修改前有效），设置空串即清除；随书 sidecar 保存。
/// Highlight note: read returns a static string valid until the next edit; an empty string clears
/// it. Saved with the book sidecar.
const char *book_highlight_note(int index);
bool book_highlight_set_note(int index, const char *note);

/// 立即把脏记录写回 sidecar（临时文件 + rename）；失败保留脏标记供重试。
/// Flush dirty ranges to the sidecar (temp file + rename); a failure keeps the dirty flag for retry.
bool book_highlight_save(void);
/// 定期重试失败的保存。/ Retry a failed save periodically.
void book_highlight_tick(int64_t now_ms);
