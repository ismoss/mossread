/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：荧光笔式划线记录的存储层。
 *       每本书一个原子写入的隐藏 sidecar：32 字节头 + 每段 108 字节
 *       （章、标志、起止字节、96 字节原文摘录）。没有文件时按空集合载入，
 *       第一次新增会自动建文件并落盘。
 *
 * English: Storage for highlighter-style ranges.
 *          One atomically written hidden sidecar per book: a 32-byte header plus 108-byte
 *          records (chapter, flags, start/end byte, 96-byte excerpt). A missing file loads as
 *          an empty set; the first new range creates the sidecar.
 */
#include "book_highlight.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

typedef struct {
    char magic[4];        ///< "RPHL"
    uint32_t version;     ///< 2
    uint32_t count;
    uint32_t file_size;
    uint64_t path_hash;
    int64_t modified;
} highlight_header_t;

#define HIGHLIGHT_VERSION 4u
// v1 只有 12 字节定位字段，读取旧文件时补空摘录。/ V1 had 12-byte anchors only; old files load with empty excerpts.
typedef struct {
    uint16_t chapter, flags;
    uint32_t start, end;
} highlight_record_v1_t;
// v2 有定位 + 摘录，没有标签；读入时标签补 0。/ V2 had anchors + excerpt but no tag; tags default to 0.
typedef struct {
    uint16_t chapter, flags;
    uint32_t start, end;
    char excerpt[BOOK_HIGHLIGHT_EXCERPT];
} highlight_record_v2_t;
// v3 增加预设标签；笔记补空串。/ V3 added the preset tag; notes default to empty.
typedef struct {
    uint16_t chapter, flags;
    uint32_t start, end;
    char excerpt[BOOK_HIGHLIGHT_EXCERPT];
    uint8_t tag;
    uint8_t reserved[3];
} highlight_record_v3_t;

typedef struct {
    uint16_t chapter, flags;
    uint32_t start, end;
    char excerpt[BOOK_HIGHLIGHT_EXCERPT];
    uint8_t tag;          ///< 0 无标签，1..3 预设标签
    uint8_t reserved[3];  ///< 对齐/将来扩展
    char note[BOOK_HIGHLIGHT_NOTE];
} highlight_range_t;

#define HIGHLIGHT_PATH_MAX 288
#define HIGHLIGHT_RETRY_MS 2000

static highlight_range_t *s_ranges;
static int s_count;
static bool s_loaded;
static char s_book_path[HIGHLIGHT_PATH_MAX];
static uint64_t s_path_hash;
static uint32_t s_file_size;
static int64_t s_modified;
static bool s_storage_ok;
static bool s_dirty;
static int64_t s_dirty_since;

static uint64_t path_hash(const char *path) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)path; p && *p; ++p)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    return hash;
}

// sidecar 跟书走：SD 书放卡上的隐藏目录，内置书放内部分区；其它路径只在本次会话内保留。
// The sidecar follows the book: SD books use the card's hidden directory, internal books use the
// flash data partition; other paths keep ranges for the session only.
static bool sidecar_path(const char *book_path, char *out, size_t cap) {
    const char *dir, *root;
    if (!book_path) return false;
    if (!strncmp(book_path, "/sdcard/", 8)) {
        dir = "/sdcard/.readpico/highlight";
        root = "/sdcard/.readpico";
    } else if (!strncmp(book_path, "/flash/", 7)) {
        dir = "/flash/.readpico/highlight";
        root = "/flash/.readpico";
    } else return false;
    // 两级目录都要建；已存在时 mkdir 返回 EEXIST，继续即可。
    // Create both levels; an existing directory returns EEXIST and is fine.
    if (mkdir(root, 0777) && errno != EEXIST) return false;
    if (mkdir(dir, 0777) && errno != EEXIST) return false;
    return snprintf(out, cap, "%s/%016llx-hl.bin", dir,
                    (unsigned long long)path_hash(book_path)) < (int)cap;
}

static bool ranges_alloc(void) {
    if (!s_ranges)
        s_ranges = heap_caps_malloc(sizeof(highlight_range_t) * BOOK_HIGHLIGHT_MAX,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_ranges != NULL;
}

static void copy_excerpt(char *out, const char *excerpt) {
    if (!excerpt) excerpt = "";
    size_t i = 0;
    while (i + 1 < BOOK_HIGHLIGHT_EXCERPT && excerpt[i]) {
        out[i] = excerpt[i];
        ++i;
    }
    out[i] = 0;
}

bool book_highlight_load(const char *book_path) {
    book_highlight_unload();
    if (!book_path || !book_path[0]) return false;
    struct stat st;
    if (stat(book_path, &st) || st.st_size <= 0) return false;
    // 先记住书身份并标记已载入：没有 sidecar 是“空集合”，不是加载失败。
    // Remember the book identity and mark it loaded first: a missing sidecar is an empty set,
    // not a load failure (the old behaviour made the first highlight impossible to save).
    memcpy(s_book_path, book_path, sizeof(s_book_path));
    s_book_path[sizeof(s_book_path) - 1] = 0;
    s_path_hash = path_hash(book_path);
    s_file_size = (uint32_t)st.st_size;
    s_modified = (int64_t)st.st_mtime;
    s_loaded = true;
    s_storage_ok = false;
    s_dirty = false;
    s_dirty_since = 0;
    char sidecar[HIGHLIGHT_PATH_MAX];
    if (!sidecar_path(book_path, sidecar, sizeof(sidecar))) return true;
    s_storage_ok = true;
    FILE *file = fopen(sidecar, "rb");
    if (!file) return true;
    highlight_header_t header;
    bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
              !memcmp(header.magic, "RPHL", 4) &&
              (header.version >= 1 && header.version <= HIGHLIGHT_VERSION) &&
              header.count <= BOOK_HIGHLIGHT_MAX && header.file_size == (uint32_t)st.st_size &&
              header.path_hash == s_path_hash;
    if (ok && header.count && ranges_alloc()) {
        int kept = 0;
        if (header.version == 1) {
            highlight_record_v1_t old;
            for (uint32_t i = 0; i < header.count; ++i) {
                if (fread(&old, sizeof(old), 1, file) != 1) break;
                if (old.start >= old.end) continue;
                memset(&s_ranges[kept], 0, sizeof(s_ranges[kept]));
                s_ranges[kept].chapter = old.chapter;
                s_ranges[kept].flags = old.flags;
                s_ranges[kept].start = old.start;
                s_ranges[kept].end = old.end;
                ++kept;
            }
        } else if (header.version == 2) {
            highlight_record_v2_t old;
            for (uint32_t i = 0; i < header.count; ++i) {
                if (fread(&old, sizeof(old), 1, file) != 1) break;
                if (old.start >= old.end) continue;
                memset(&s_ranges[kept], 0, sizeof(s_ranges[kept]));
                s_ranges[kept].chapter = old.chapter;
                s_ranges[kept].flags = old.flags;
                s_ranges[kept].start = old.start;
                s_ranges[kept].end = old.end;
                memcpy(s_ranges[kept].excerpt, old.excerpt, BOOK_HIGHLIGHT_EXCERPT);
                ++kept;
            }
        } else if (header.version == 3) {
            highlight_record_v3_t old;
            for (uint32_t i = 0; i < header.count; ++i) {
                if (fread(&old, sizeof(old), 1, file) != 1) break;
                if (old.start >= old.end) continue;
                memset(&s_ranges[kept], 0, sizeof(s_ranges[kept]));
                s_ranges[kept].chapter = old.chapter;
                s_ranges[kept].flags = old.flags;
                s_ranges[kept].start = old.start;
                s_ranges[kept].end = old.end;
                memcpy(s_ranges[kept].excerpt, old.excerpt, BOOK_HIGHLIGHT_EXCERPT);
                s_ranges[kept].tag = old.tag <= 3 ? old.tag : 0;
                ++kept;
            }
        } else {
            kept = (int)fread(s_ranges, sizeof(highlight_range_t), header.count, file);
            if (kept < 0) kept = 0;
            int write = 0;
            for (int i = 0; i < kept; ++i) {
                if (s_ranges[i].start >= s_ranges[i].end) continue;
                s_ranges[i].excerpt[BOOK_HIGHLIGHT_EXCERPT - 1] = 0;
                if (s_ranges[i].tag > 3) s_ranges[i].tag = 0;
                s_ranges[i].reserved[0] = s_ranges[i].reserved[1] = s_ranges[i].reserved[2] = 0;
                s_ranges[i].note[BOOK_HIGHLIGHT_NOTE - 1] = 0;
                s_ranges[write++] = s_ranges[i];
            }
            kept = write;
        }
        s_count = kept;
    }
    fclose(file);
    return true;
}

void book_highlight_unload(void) {
    s_count = 0;
    s_loaded = false;
    s_book_path[0] = 0;
    s_path_hash = 0;
    s_file_size = 0;
    s_modified = 0;
    s_storage_ok = false;
    s_dirty = false;
    s_dirty_since = 0;
}

int book_highlight_count(void) { return s_loaded ? s_count : 0; }

static bool range_overlaps(const highlight_range_t *range, uint16_t chapter,
                           uint32_t start, uint32_t end) {
    return range->chapter == chapter && range->start < end && start < range->end;
}

bool book_highlight_overlaps(uint16_t chapter, uint32_t start, uint32_t end) {
    if (!s_loaded || !s_count || start >= end) return false;
    for (int i = 0; i < s_count; ++i)
        if (range_overlaps(&s_ranges[i], chapter, start, end)) return true;
    return false;
}

bool book_highlight_range_at(uint16_t chapter, uint32_t start, uint32_t end,
                             uint32_t *out_start, uint32_t *out_end) {
    if (!s_loaded || !s_count || start >= end) return false;
    bool found = false;
    uint32_t low = 0, high = 0;
    for (int i = 0; i < s_count; ++i) {
        const highlight_range_t *range = &s_ranges[i];
        if (!range_overlaps(range, chapter, start, end)) continue;
        const uint32_t a = range->start > start ? range->start : start;
        const uint32_t b = range->end < end ? range->end : end;
        if (!found || a < low) low = a;
        if (!found || b > high) high = b;
        found = true;
    }
    if (found) {
        if (out_start) *out_start = low;
        if (out_end) *out_end = high;
    }
    return found;
}

bool book_highlight_add(uint16_t chapter, uint32_t start, uint32_t end, const char *excerpt) {
    if (!s_loaded || start >= end || !ranges_alloc()) return false;
    // 拖动会扫过已有记录：先取出与本次相交/相邻的所有段并合并，避免留下碎片。
    // A drag sweeps across existing marks: merge every intersecting/adjacent range and keep no fragments.
    uint32_t merged_start = start, merged_end = end;
    uint8_t merged_tag = 0;
    char merged_note[BOOK_HIGHLIGHT_NOTE] = {0};
    int kept = 0;
    for (int i = 0; i < s_count; ++i) {
        const highlight_range_t *range = &s_ranges[i];
        if (range->chapter == chapter && range->start <= merged_end && merged_start <= range->end) {
            if (range->start < merged_start) merged_start = range->start;
            if (range->end > merged_end) merged_end = range->end;
            if (!merged_tag) merged_tag = range->tag;
            if (!merged_note[0]) memcpy(merged_note, range->note, sizeof(merged_note));
            continue;
        }
        s_ranges[kept++] = *range;
    }
    s_count = kept;
    bool added = false;
    if (s_count < BOOK_HIGHLIGHT_MAX) {
        highlight_range_t *record = &s_ranges[s_count++];
        memset(record, 0, sizeof(*record));
        record->chapter = chapter;
        record->start = merged_start;
        record->end = merged_end;
        record->tag = merged_tag;
        memcpy(record->note, merged_note, sizeof(record->note));
        copy_excerpt(record->excerpt, excerpt);
        added = true;
    }
    s_dirty = true;
    s_dirty_since = esp_timer_get_time() / 1000;
    (void)book_highlight_save();
    return added;
}

int book_highlight_at(int index, uint16_t *chapter, uint32_t *start, uint32_t *end,
                      const char **excerpt) {
    if (!s_loaded || index < 0 || index >= s_count) return -1;
    const highlight_range_t *record = &s_ranges[index];
    if (chapter) *chapter = record->chapter;
    if (start) *start = record->start;
    if (end) *end = record->end;
    if (excerpt) *excerpt = record->excerpt;
    return index;
}

// 预设标签：0 无、1 重要、2 待读、3 疑问；列表页点标签循环切换并立即落盘。
// Preset tags: 0 none, 1 important, 2 to-read, 3 question; the list cycles them and saves at once.
uint8_t book_highlight_tag(int index) {
    if (!s_loaded || index < 0 || index >= s_count) return 0xFF;
    return s_ranges[index].tag;
}

bool book_highlight_set_tag(int index, uint8_t tag) {
    if (!s_loaded || index < 0 || index >= s_count || tag > 3) return false;
    if (s_ranges[index].tag == tag) return true;
    s_ranges[index].tag = tag;
    s_dirty = true;
    s_dirty_since = esp_timer_get_time() / 1000;
    return book_highlight_save();
}

const char *book_highlight_note(int index) {
    if (!s_loaded || index < 0 || index >= s_count) return "";
    return s_ranges[index].note;
}

bool book_highlight_set_note(int index, const char *note) {
    if (!s_loaded || index < 0 || index >= s_count) return false;
    if (!note) note = "";
    size_t i = 0;
    while (i + 1 < BOOK_HIGHLIGHT_NOTE && note[i]) ++i;
    highlight_range_t *record = &s_ranges[index];
    char stored[BOOK_HIGHLIGHT_NOTE];
    memcpy(stored, note, i);
    stored[i] = 0;
    if (!strcmp(stored, record->note)) return true;
    memset(record->note, 0, sizeof(record->note));
    memcpy(record->note, stored, i + 1);
    s_dirty = true;
    s_dirty_since = esp_timer_get_time() / 1000;
    return book_highlight_save();
}

bool book_highlight_remove(int index) {
    if (!s_loaded || index < 0 || index >= s_count) return false;
    for (int i = index + 1; i < s_count; ++i) s_ranges[i - 1] = s_ranges[i];
    --s_count;
    s_dirty = true;
    s_dirty_since = esp_timer_get_time() / 1000;
    return book_highlight_save();
}

bool book_highlight_save(void) {
    if (!s_loaded || !s_dirty || !s_storage_ok || !s_book_path[0]) return !s_dirty;
    char sidecar[HIGHLIGHT_PATH_MAX];
    if (!sidecar_path(s_book_path, sidecar, sizeof(sidecar))) return false;
    char temp[HIGHLIGHT_PATH_MAX + 4];
    if (snprintf(temp, sizeof(temp), "%s.tmp", sidecar) >= (int)sizeof(temp)) return false;
    FILE *file = fopen(temp, "wb");
    if (!file) return false;
    highlight_header_t header = {.version = HIGHLIGHT_VERSION, .count = (uint32_t)s_count,
                                 .file_size = s_file_size, .path_hash = s_path_hash,
                                 .modified = s_modified};
    memcpy(header.magic, "RPHL", 4);
    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
              (!s_count || fwrite(s_ranges, sizeof(highlight_range_t), s_count, file) ==
                               (size_t)s_count);
    if (fclose(file)) ok = false;
    if (ok) {
        (void)remove(sidecar);
        ok = rename(temp, sidecar) == 0;
    }
    if (!ok) {
        (void)remove(temp);
        s_dirty_since = esp_timer_get_time() / 1000;
        return false;
    }
    s_dirty = false;
    return true;
}

void book_highlight_tick(int64_t now_ms) {
    if (!s_dirty || !s_loaded) return;
    if (now_ms - s_dirty_since < HIGHLIGHT_RETRY_MS) return;
    (void)book_highlight_save();
}
