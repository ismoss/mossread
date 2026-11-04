/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 用户2026-10-20修订：文件二级页常驻灰底栏，操作与分页恢复原高度，边缘返回沿用取消与目录回退。
 * User revision2026-10-20: file children retain gray navigation with original footer/control heights; edge back retains cancellation and directory backtracking.
 *
 * 中文：TF 卡文件入口、目录浏览及 USB/Wi-Fi 传输入口。
 * English: SD file entry, directory browser and USB/Wi-Fi transfer routes.
 * 用户修订：文件管理直接显示 WiFi、热点、USB 三个入口，不再包含在线书籍下载。
 * User revision: show direct WiFi, hotspot and USB routes, with no online book downloads.
 * 用户授权新输入法：文件改名使用共用九宫格/全键盘及常亮光标；离页释放候选。
 * Authorized keyboard revision: rename uses shared T9/QWERTY and a steady caret; exit frees candidates.
 * 用户修订：输入只刷新变化区域，布局切换局部灰阶，避免打字累计整屏清残影。
 * User revision: input refreshes changed regions; layout switches use local grayscale, without accumulating whole-page cleanup.
 */
#include "ui_avatar.h"
#include "book_cover_jobs.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "app_content_open.h"
#include "app_transfer_mode.h"
#include "book_store.h"
#include "book_progress.h"
#include "book_title.h"
#include "display.h"
#include "file_tree.h"
#include "read_pico_sd.h"
#include "read_pico_search.h"
#include "settings.h"
#include "ttf_font.h"
#include "app_font_context.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_text_input.h"
#include "ui_keyboard.h"
#include "file_search.h"
#include "e0470_epaper_waveform.h"
#include "esp_timer.h"
#include "ui_nav.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

#define FILE_MAX 96
#define FILE_ROWS 9
typedef struct { char path[288]; char name[128]; off_t size; int64_t mtime; bool is_dir; bool selected; } file_item_t;
// 文件列表放 PSRAM：内部 RAM 已被静态占用到约 90%，而 BLE 控制器只能用内部 RAM，
// 列表这类大数组没必要跟它抢。
// The file list lives in PSRAM. Internal RAM is already around 90% statically used and the BLE
// controller can only use internal RAM, so a list this big has no business holding any of it.
static file_item_t *s_files;

static bool files_alloc(void) {
    if (!s_files)
        s_files = heap_caps_calloc(FILE_MAX, sizeof(file_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_files != NULL;
}
static int s_count, s_folder, s_page;
static int s_requested_folder = -1;
static int s_counts[3];
static bool s_scan_pending;
static int s_pager_pressed=UI_PAGER_NONE;
static bool s_pager_paint_pending;
static int64_t s_space_poll_ms;
static char s_message[96];
static char s_dir[288] = "/sdcard";
static read_pico_sd_info_t s_sd;
// 卡在、挂不上、又不是超时，多半是文件系统本机不认（exFAT 之类）。这种卡仍然格得动，
// 所以卡片本身变成可点的：第一次点只是问，第二次点才真的格。
// A card is in the slot but will not mount and it is not a timeout: most likely a filesystem this
// firmware does not read, exFAT for instance. Such a card can still be formatted, so the card
// itself becomes a tap target -- the first tap asks, the second one does it.
static bool s_sd_format_confirm;
// 格式化尝试的反馈，显示在卡片的信息行上；为空时回到提示语。
// Feedback from a format attempt, shown on the card's info line; empty falls back to the hint.
static char s_sd_notice[64];
typedef enum { FILE_VIEW_LIST, FILE_VIEW_ACTIONS, FILE_VIEW_RENAME, FILE_VIEW_MOVE, FILE_VIEW_SEARCH } file_view_t;
static file_view_t s_view;
// 文件列表排序：文件夹恒在前，组内按所选键升降序（rc103 文件管理套件·排序）。点表头摘要循环切换。
// File list sort: folders always first; within a group sort by the chosen key asc/desc. Tap the header
// summary to cycle. Folder-first is preserved, so directory browsing stays predictable.
typedef enum { SORT_NAME_ASC, SORT_NAME_DESC, SORT_TIME_DESC, SORT_TIME_ASC, SORT_SIZE_DESC, SORT_SIZE_ASC, SORT_COUNT } file_sort_t;
static file_sort_t s_sort = SORT_NAME_ASC;
static bool s_file_grid;   // 列表/网格视图切换（rc103 套件）。/ List vs grid view.
static bool s_select_mode; // 跨页多选模式（rc103 套件）。/ Cross-page multi-select mode.
static bool s_batch_confirm;
static int selected_count(void);
static void clear_selection(void);
static void batch_delete_selected(void);
static char (*s_batch_src)[288];
static int s_batch_src_count;
static void batch_src_clear(void);
static void batch_move_begin(void);
static app_redraw_t batch_move_to_dir(void);
static file_item_t s_selected;
static bool s_delete_confirm;
static int s_move_origin_folder, s_move_origin_page;
static char s_move_origin_dir[288];
static char s_editor[121], s_editor_ext[16], s_editor_notice[96];
static ui_text_edit_t s_editor_input;
// 递归搜索状态：查询串、键盘输入、增量遍历句柄与节流。/ Recursive search: query, keyboard input, incremental walker.
static char s_query[65];
static ui_text_edit_t s_query_input;
static file_search_t *s_fs;
static unsigned s_fs_matches;
static int64_t s_search_paint_ms;
static void search_close(void);
static void search_clear(void);
static void search_begin(void);
static void render_search(uint8_t *fb);
static app_redraw_t search_paint(app_ctx_t *ctx, bool field);
static app_redraw_t search_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev);
// 书籍统计（根页展示书籍数量与总大小）。/ Book stats (count and total size on the root page).
static file_search_t *s_book_stat_fs;
static unsigned s_book_stat_count;
static uint64_t s_book_stat_bytes;
static bool s_book_stat_done;
static int64_t s_book_stat_next_ms;
static void book_stat_start(void);
static void scan_folder(int folder);
static bool main_page_visible(app_ctx_t *ctx);
static bool s_render_secondary;
static void files_navigation(uint8_t *fb) {
    ui_nav_draw(fb,2);
}
static int files_nav_hit(uint16_t x,uint16_t y) {
    return ui_nav_hit(x,y);
}

static const char *const names[] = {"书籍", "图片", "字体", "TF 卡目录"};
static const char *folder_root(int folder) {
    if (folder == 0) return app_settings_books_dir();
    if (folder == 1) return "/sdcard/pictures";
    if (folder == 2) return app_settings_fonts_dir();
    return s_dir;
}
static int file_pager_top(void) {
    return 1006;
}
static EpdRect file_action_rect(int index) {
    int top=875;
    return (EpdRect){index?355:36,top,293,76};
}
static int file_message_top(void) {
    return 978;
}
static int file_sheet_offset(void) {
    return 0;
}
// 网格视图仅在普通文件夹列表生效（移动目标/搜索结果为列表）。/ Grid only in the folder list (not move-target or search results).
static bool files_grid_active(void) { return s_file_grid && s_view != FILE_VIEW_MOVE && !s_query[0]; }
static EpdRect files_entry_rect(int row) {
    if (files_grid_active()) { int col = row % 3, r = row / 3; return (EpdRect){36 + col * 204, 214 + r * 196, 196, 188}; }
    return (EpdRect){36, 214 + row * 88, 612, 82};
}
static int visible_rows(void) {
    int end=s_folder==3||s_view==FILE_VIEW_MOVE?file_action_rect(0).y-16:file_pager_top()-16;
    if (files_grid_active()) { int rows=(end-214)/196; return rows>0?rows*3:3; }
    return (end-214)/88;
}
static int files_entry_hit(int x, int y) { for (int row = 0; row < visible_rows(); ++row) if (ui_rect_hit(files_entry_rect(row), x, y)) return row; return -1; }
static int root_rows(void) { return 5; }
static int file_pager_pages(void) {
    int rows=s_folder<0?root_rows():visible_rows();
    return s_count?1+(s_count-1)/rows:1;
}
static void draw_file_pager(uint8_t *fb) {
    char label[48];snprintf(label,sizeof(label),"第 %d 页 · 共 %d 页",s_page+1,file_pager_pages());
    ui_pager_draw_at(fb,label,s_page,file_pager_pages(),false,s_pager_pressed,file_pager_top());
}
static app_redraw_t file_pager_paint(app_ctx_t *ctx) {
    ui_fill_rect_fast(ui_pager_area_at(file_pager_top()),UI_GRAY_WHITE,ctx->fb);
    draw_file_pager(ctx->fb);s_pager_paint_pending=true;
    return APP_REDRAW_AREA;
}
// 四键共用命中和目标计算；只在抬手提交，同一按钮外移/长按/取消只恢复箭头。
// Share hits/targets across four controls; commit on release, while leaving/holding/cancelling only restores arrows.
static bool file_pager_event(app_ctx_t *ctx,const ui_gesture_event_t *ev,app_redraw_t *redraw) {
    if(s_view!=FILE_VIEW_LIST&&s_view!=FILE_VIEW_MOVE)return false;
    int start=ui_pager_hit_at(ev->x0,ev->y0,file_pager_top()),end=ui_pager_hit_at(ev->x,ev->y,file_pager_top());
    if(start<0&&s_pager_pressed<0)return false;
    *redraw=APP_REDRAW_NONE;
    if(s_scan_pending)return true;
    if(ev->type==UI_GESTURE_PRESS){s_pager_pressed=start;*redraw=file_pager_paint(ctx);return true;}
    if(ev->type==UI_GESTURE_MOVE&&start==end)return true;
    bool decorated=s_pager_pressed>=0;s_pager_pressed=UI_PAGER_NONE;
    if(ev->type==UI_GESTURE_TAP&&start>=0&&start==end){
        int target=ui_pager_target(start,s_page,file_pager_pages());
        if(target!=s_page){s_page=target;*redraw=APP_REDRAW_PAGE;return true;}
    }
    if(decorated)*redraw=file_pager_paint(ctx);
    return true;
}
static EpdRect home_transfer_rect(int index) {
    return (EpdRect){36 + index * 208, 350, 196, 92};
}

// 存储卡卡片的位置，绘制与命中测试共用一份，免得两处各写一个坐标。
// The storage card's rect, shared by drawing and hit testing so the two cannot drift apart.
static EpdRect storage_card_rect(void) { return (EpdRect){36, 171, 612, 132}; }

// 需要格式化：卡在、没挂上、而且不是超时。超时是接触或供电问题，格式化解决不了，那种情况下
// 说"格式化可能可以解决"会把用户带偏。
// Needs formatting: the card is present, will not mount, and the failure is not a timeout. A
// timeout is a contact or power problem that formatting cannot fix, so offering it there would
// point the user at the wrong thing.
static bool sd_needs_format(void) { return s_sd.present && !s_sd.mounted && s_sd.needs_format; }

static void copy_utf8(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xc0) == 0x80) --n;
    }
    memcpy(dst, src, n); dst[n] = 0;
}

// 划线导出产物（旧版写进 books 的 highlights.md/txt，以及新目录 mossread_notes）不算书籍。
// Highlight export artifacts (legacy highlights.md/txt in books, and the new mossread_notes dir) are not books.
static bool ignored_book_artifact(const char *path) {
    if (!path) return false;
    if (strstr(path, "/mossread_notes/")) return true;
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    return !strcasecmp(b, "highlights.md") || !strcasecmp(b, "highlights.txt");
}
static bool is_book_path(const char *path) {
    if (ignored_book_artifact(path)) return false;
    const char *ext = strrchr(path, '.');
    return ext && (!strcasecmp(ext, ".epub") || !strcasecmp(ext, ".txt") ||
                   !strcasecmp(ext, ".md") || !strcasecmp(ext, ".markdown"));
}

static bool sibling_path(const char *name, char out[288]) {
    const char *slash = strrchr(s_selected.path, '/');
    if (!slash || strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) return false;
    size_t parent = (size_t)(slash - s_selected.path);
    if (!name[0] || name[0] == '.' || parent + 1 + strlen(name) >= 288) return false;
    memcpy(out, s_selected.path, parent);
    out[parent] = '/';
    strcpy(out + parent + 1, name);
    return true;
}

static void migrate_book_state(const char *old_path, const char *new_path, uint32_t size) {
    if (!is_book_path(old_path)) return;
    book_progress_t progress = {0};
    bool had_progress = book_progress_load(old_path, size, &progress);
    char last[BOOK_STORE_PATH_MAX] = {0};
    bool was_last = book_progress_last_path(last, sizeof(last)) && !strcmp(last, old_path);
    if (had_progress) (void)book_progress_save(new_path, &progress);
    (void)book_progress_forget(old_path);
    if (was_last) (void)book_progress_set_last_path(new_path);
    (void)book_title_clear(old_path);
}

static esp_err_t copy_selected_item(char out_name[128]) {
    const char *dot = s_selected.is_dir ? NULL : strrchr(s_selected.name, '.');
    size_t stem_len = dot && dot != s_selected.name ? (size_t)(dot - s_selected.name) : strlen(s_selected.name);
    char stem[100], candidate[128], destination[288];
    if (stem_len >= sizeof(stem)) stem_len = sizeof(stem) - 1;
    memcpy(stem, s_selected.name, stem_len); stem[stem_len] = 0;
    const char *ext = dot && dot != s_selected.name ? dot : "";
    for (int number = 1; number < 100; ++number) {
        int used = number == 1 ? snprintf(candidate, sizeof(candidate), "%s 副本%s", stem, ext) :
            snprintf(candidate, sizeof(candidate), "%s 副本 %d%s", stem, number, ext);
        if (used < 0 || used >= (int)sizeof(candidate)) return ESP_ERR_INVALID_SIZE;
        if (!sibling_path(candidate, destination)) return ESP_ERR_INVALID_ARG;
        struct stat exists;
        if (stat(destination, &exists) != 0) {
            if (errno == ENOENT) break;
            return ESP_FAIL;
        }
        if (number == 99) return ESP_ERR_INVALID_SIZE;
    }
    read_pico_sd_get_info(&s_sd);
    if (!s_sd.mounted) return ESP_ERR_INVALID_STATE;
    esp_err_t err = file_tree_copy(s_selected.path, destination, s_sd.free_bytes);
    if (err != ESP_OK) return err;
    copy_utf8(out_name, 128, candidate);
    book_store_notify_changed();
    return ESP_OK;
}

static bool rebased_path(const char *path, const char *old_path, const char *new_path,
                         char *out, size_t cap) {
    if (!file_tree_same_or_below(path, old_path)) return false;
    int n = snprintf(out, cap, "%s%s", new_path, path + strlen(old_path));
    return n >= 0 && (size_t)n < cap;
}

static bool moved_settings_fit(const char *old_path, const char *new_path) {
    char updated[288];
    const char *paths[] = {app_settings_books_dir(),app_settings_fonts_dir(),app_settings_system_font_path(),
        app_settings_font_path(), app_settings_wallpaper_path()};
    const size_t limits[] = {96,96,160,160,288};
    for (int i = 0; i < 5; ++i) {
        if (!file_tree_same_or_below(paths[i], old_path)) continue;
        if (!rebased_path(paths[i], old_path, new_path, updated, sizeof(updated)) ||
            strlen(updated) >= limits[i]) return false;
    }
    return true;
}

static void sync_moved_settings(const char *old_path, const char *new_path) {
    char updated[288];
    if(rebased_path(app_settings_books_dir(),old_path,new_path,updated,sizeof(updated)))
        (void)app_settings_set_books_dir(updated);
    if(rebased_path(app_settings_fonts_dir(),old_path,new_path,updated,sizeof(updated))){
        (void)app_settings_set_fonts_dir(updated);ttf_font_scan();
    }
    if (rebased_path(app_settings_system_font_path(), old_path, new_path, updated, sizeof(updated))) {
        app_settings_set_system_font_path(updated);
        app_font_activate_system();
    }
    if (rebased_path(app_settings_font_path(), old_path, new_path, updated, sizeof(updated)))
        app_settings_set_font_path(updated);
    if (rebased_path(app_settings_wallpaper_path(), old_path, new_path, updated, sizeof(updated)))
        app_settings_set_wallpaper_path(updated);
    if (rebased_path(app_settings_wallpaper_landscape_path(), old_path, new_path, updated, sizeof(updated)))
        app_settings_set_wallpaper_landscape_path(updated);
}

static void notify_moved_tree(const char *old_path, const char *new_path, unsigned depth) {
    if (depth > 8) return;
    struct stat st;
    if (stat(new_path, &st)) return;
    if (S_ISREG(st.st_mode)) {
        migrate_book_state(old_path, new_path,
            st.st_size >= 0 && (uint64_t)st.st_size <= UINT32_MAX ? (uint32_t)st.st_size : 0);
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;
    DIR *dir = opendir(new_path);
    if (!dir) return;
    char *paths = heap_caps_malloc(576, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!paths) { closedir(dir); return; }
    char *old_child = paths, *new_child = paths + 288;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (snprintf(old_child, 288, "%s/%s", old_path, entry->d_name) >= 288 ||
            snprintf(new_child, 288, "%s/%s", new_path, entry->d_name) >= 288) continue;
        notify_moved_tree(old_child, new_child, depth + 1);
    }
    free(paths);
    closedir(dir);
}

static void file_deleted(const char *path, bool directory, void *ctx) {
    (void)ctx;
    if (directory) return;
    if (is_book_path(path)) (void)book_progress_forget(path);
    (void)book_title_clear(path);
    if (!strcmp(app_settings_system_font_path(), path)) {
        app_settings_set_system_font_path("");
        app_font_activate_system();
    }
    if (!strcmp(app_settings_font_path(), path)) app_settings_set_font_path("");
    if (!strcmp(app_settings_book_background_path(), path)) (void)app_settings_set_book_background_path("");
    if (!strcmp(app_settings_wallpaper_landscape_path(), path)) app_settings_set_wallpaper_landscape_path("");
    if (!strcmp(app_settings_wallpaper_path(), path)) {
        app_settings_set_wallpaper_path("");
        app_settings_set_lock_style(0);
    }
}

static void repair_deleted_directories(void) {
    struct stat st;
    if (stat(app_settings_books_dir(), &st) || !S_ISDIR(st.st_mode)) {
        (void)mkdir("/sdcard/books", 0775);
        (void)app_settings_set_books_dir("/sdcard/books");
    }
    if (stat(app_settings_fonts_dir(), &st) || !S_ISDIR(st.st_mode)) {
        (void)mkdir("/sdcard/fonts", 0775);
        (void)app_settings_set_fonts_dir("/sdcard/fonts");
        ttf_font_scan();
    }
}

static void start_move(void) {
    s_move_origin_folder = s_folder;
    s_move_origin_page = s_page;
    copy_utf8(s_move_origin_dir, sizeof(s_move_origin_dir), s_dir);
    copy_utf8(s_dir, sizeof(s_dir), "/sdcard");
    s_folder = 3;
    s_view = FILE_VIEW_MOVE;
    scan_folder(3);
}

static void leave_move(bool changed) {
    s_folder = s_move_origin_folder;
    copy_utf8(s_dir, sizeof(s_dir), s_move_origin_dir);
    s_view = FILE_VIEW_LIST;
    scan_folder(s_folder >= 0 ? s_folder : 3);
    if (!changed) s_page = s_move_origin_page;
    batch_src_clear();
}

static app_redraw_t move_selected(void) {
    read_pico_sd_get_info(&s_sd);
    if (!s_sd.mounted) {
        snprintf(s_message, sizeof(s_message), "TF 卡不可用"); return APP_REDRAW_PAGE;
    }
    char destination[288];
    if (snprintf(destination, sizeof(destination), "%s/%s", s_dir, s_selected.name) >= sizeof(destination)) {
        snprintf(s_message, sizeof(s_message), "目标路径过长"); return APP_REDRAW_PAGE;
    }
    if (!strcmp(destination, s_selected.path)) {
        snprintf(s_message, sizeof(s_message), "已在此文件夹"); return APP_REDRAW_PAGE;
    }
    if (!moved_settings_fit(s_selected.path, destination)) {
        snprintf(s_message, sizeof(s_message), "设置路径过长，无法移动"); return APP_REDRAW_PAGE;
    }
    esp_err_t err = file_tree_move(s_selected.path, destination);
    if (err != ESP_OK) {
        snprintf(s_message, sizeof(s_message), err == ESP_ERR_INVALID_STATE ? "目标已有同名项目" :
                 err == ESP_ERR_INVALID_ARG ? "不能移入自身文件夹" : "移动失败，请检查 TF 卡");
        return APP_REDRAW_PAGE;
    }
    sync_moved_settings(s_selected.path, destination);
    notify_moved_tree(s_selected.path, destination, 0);
    book_store_notify_changed();
    leave_move(true);
    snprintf(s_message, sizeof(s_message), "移动完成");
    return APP_REDRAW_PAGE;
}
// 批量移动：先把所选源路径快照进 PSRAM（避免进入目标浏览时 s_files 被替换），再复用 MOVE 视图选目标。
// Batch move: snapshot the selected source paths to PSRAM (the target browser replaces s_files), then reuse the MOVE view.
static void batch_src_clear(void) { if (s_batch_src) { free(s_batch_src); s_batch_src = NULL; } s_batch_src_count = 0; }
static void batch_move_begin(void) {
    int n = selected_count();
    if (n <= 0) { snprintf(s_message, sizeof(s_message), "未选择文件"); return; }
    batch_src_clear();
    s_batch_src = heap_caps_malloc((size_t)n * 288, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_batch_src) { snprintf(s_message, sizeof(s_message), "内存不足"); return; }
    s_batch_src_count = 0;
    for (int i = 0; i < s_count; ++i) {
        if (!s_files[i].selected) continue;
        copy_utf8(s_batch_src[s_batch_src_count], 288, s_files[i].path);
        ++s_batch_src_count;
    }
    s_select_mode = false; s_batch_confirm = false;
    s_move_origin_folder = s_folder; s_move_origin_page = s_page;
    copy_utf8(s_move_origin_dir, sizeof(s_move_origin_dir), s_dir);
    copy_utf8(s_dir, sizeof(s_dir), "/sdcard");
    s_folder = 3; s_view = FILE_VIEW_MOVE; scan_folder(3);
}
static app_redraw_t batch_move_to_dir(void) {
    read_pico_sd_get_info(&s_sd);
    if (!s_sd.mounted) { snprintf(s_message, sizeof(s_message), "TF 卡不可用"); return APP_REDRAW_PAGE; }
    unsigned done = 0; bool failed = false;
    for (int i = 0; i < s_batch_src_count; ++i) {
        const char *src = s_batch_src[i];
        const char *base = strrchr(src, '/'); base = base ? base + 1 : src;
        char dest[288];
        if (snprintf(dest, sizeof(dest), "%s/%s", s_dir, base) >= (int)sizeof(dest)) { failed = true; continue; }
        if (!strcmp(dest, src)) continue;
        if (!moved_settings_fit(src, dest)) { failed = true; continue; }
        if (file_tree_move(src, dest) == ESP_OK) { sync_moved_settings(src, dest); notify_moved_tree(src, dest, 0); ++done; }
        else failed = true;
    }
    book_store_notify_changed();
    batch_src_clear();
    s_view = FILE_VIEW_LIST; s_folder = s_move_origin_folder;
    copy_utf8(s_dir, sizeof(s_dir), s_move_origin_dir);
    scan_folder(s_folder >= 0 ? s_folder : 3);
    snprintf(s_message, sizeof(s_message), failed ? "部分移动失败，已移动 %u 项" : "已移动 %u 项", done);
    return APP_REDRAW_PAGE;
}

void app_files_request_folder(int folder) {
    if (folder >= 0 && folder < 3) s_requested_folder = folder;
}

static bool supported(int folder, const char *name) {
    if (!name[0] || name[0] == '.' || !strncmp(name, "._", 2)) return false;
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (folder == 0) {
        if (!strcasecmp(name, "highlights.md") || !strcasecmp(name, "highlights.txt")) return false; // 旧版导出产物
        return !strcasecmp(ext, ".epub") || !strcasecmp(ext, ".txt") ||
               !strcasecmp(ext, ".md") || !strcasecmp(ext, ".markdown");
    }
    if (folder == 1) return !strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png") || !strcasecmp(ext, ".webp");
    return !strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf");
}

static const char *files_sort_label(void) {
    switch (s_sort) {
        case SORT_NAME_DESC: return "名称↓";
        case SORT_TIME_DESC: return "时间↓";
        case SORT_TIME_ASC:  return "时间↑";
        case SORT_SIZE_DESC: return "大小↓";
        case SORT_SIZE_ASC:  return "大小↑";
        default:             return "名称↑";
    }
}
static int compare_files(const void *left, const void *right) {
    const file_item_t *a = left, *b = right;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;   // 文件夹恒在前 / folders first
    int c = 0;
    switch (s_sort) {
        case SORT_NAME_DESC: c = -strcasecmp(a->name, b->name); break;
        case SORT_TIME_DESC: c = (a->mtime < b->mtime) - (a->mtime > b->mtime); break;
        case SORT_TIME_ASC:  c = (a->mtime > b->mtime) - (a->mtime < b->mtime); break;
        case SORT_SIZE_DESC: c = (a->size < b->size) - (a->size > b->size); break;
        case SORT_SIZE_ASC:  c = (a->size > b->size) - (a->size < b->size); break;
        default:             c = strcasecmp(a->name, b->name); break;   // SORT_NAME_ASC
    }
    // 同一键值（如相同时间/大小）再按名称稳定排。/ Tie-break by name for a stable order.
    return c ? c : strcasecmp(a->name, b->name);
}
// 排序切换按钮：文件页表头右侧摘要行（y≈172，行区从 214 起，不冲突）。
// Sort toggle: the header summary line (y≈172); list rows start at 214, so no overlap.
static EpdRect files_sort_rect(void) { return (EpdRect){360, 156, 288, 36}; }
static EpdRect files_grid_rect(void) { return (EpdRect){508, 86, 74, 44}; }
static EpdRect files_multi_rect(void) { return (EpdRect){430, 86, 74, 44}; }
static EpdRect files_selectall_rect(void) { return (EpdRect){508, 86, 74, 44}; }
static EpdRect files_batchdel_rect(void) { return (EpdRect){586, 86, 74, 44}; }
static EpdRect files_search_entry_rect(void); // 定义在递归搜索区，渲染在更早处需前置声明。/ Defined later; forward-declared for early render use.

static void scan_folder(int folder) {
    if (!files_alloc()) return;
    s_count = s_page = 0;
    s_message[0] = 0;
    read_pico_sd_get_info(&s_sd);
    if (!s_sd.mounted) { snprintf(s_message, sizeof(s_message), "TF 卡尚未挂载"); return; }
    const char *root = folder_root(folder);
    DIR *dir = opendir(root);
    if (!dir) { snprintf(s_message, sizeof(s_message), "目录不存在：%.60s", root); return; }
    struct dirent *entry;
    while (s_count < FILE_MAX && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.' || !strncmp(entry->d_name, "._", 2)) continue;
        file_item_t *item = &s_files[s_count];
        if (snprintf(item->path, sizeof(item->path), "%s/%s", root, entry->d_name) >= sizeof(item->path)) continue;
        struct stat st;
        bool is_dir = false, is_file = false;
        if (!stat(item->path, &st)) {
            is_dir = S_ISDIR(st.st_mode);
            is_file = S_ISREG(st.st_mode);
            item->size = st.st_size;
            item->mtime = (int64_t)st.st_mtime;
        } else {
            DIR *probe = opendir(item->path);
            if (probe) { is_dir = true; item->size = 0; item->mtime = 0; closedir(probe); }
        }
        if (!is_dir && !is_file) continue;
        if (s_view == FILE_VIEW_MOVE && !is_dir) continue;
        if (folder != 3 && (!is_file || !supported(folder, entry->d_name))) continue;
        if (strnlen(entry->d_name, sizeof(item->name)) >= sizeof(item->name)) continue;
        strcpy(item->name, entry->d_name);
        item->is_dir = is_dir;
        item->selected = false; // 每次扫描清掉多选残留，避免“已选 N”错位 / clear stale selection on each scan
        ++s_count;
    }
    closedir(dir);
    qsort(s_files, s_count, sizeof(s_files[0]), compare_files);
    if (folder >= 0 && folder < 3) s_counts[folder] = s_count;
}

static void rescan_current(void) {
    scan_folder(s_folder >= 0 ? s_folder : 3);
}

static int count_root(int folder) {
    if (!s_sd.mounted) return 0;
    DIR *dir = opendir(folder_root(folder));
    if (!dir) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) if (supported(folder, entry->d_name)) ++count;
    closedir(dir);
    return count;
}

// 列表行的文件夹 / 文档图标，来自 Lucide；盒子 46 像素，中心对到原来的图形中心。
// Lucide folder / file marks for list rows; a 46 px box centred on the old artwork.
#define FILE_ICON_PX 46

static void file_icon(uint8_t *fb, int x, int y, bool folder) {
    ui_draw_icon(
        fb, x + 21, y + 21, FILE_ICON_PX,
        folder ? UI_ICON_FOLDER : UI_ICON_FILE, UI_GRAY_BLACK
    );
}

static void file_detail(char *dst, size_t cap, const file_item_t *item) {
    if (item->is_dir) snprintf(dst, cap, "文件夹");
    else if (item->size >= 1024 * 1024) snprintf(dst, cap, "%.1f MB", item->size / 1048576.0);
    else if (item->size >= 1024) snprintf(dst, cap, "%.1f KB", item->size / 1024.0);
    else snprintf(dst, cap, "%lld B", (long long)item->size);
}

// 文件列表的分隔线保持两像素，墨水屏上比浅色发丝线更容易辨认。
// Two-pixel file dividers stay legible on the panel without changing the layout.
static void file_rule(uint8_t *fb, int x, int y, int width) {
    ui_draw_separator(fb, y, x, width, 0x60);
}

// 大卡片用双层描边，避免单像素浅灰线在低对比度屏上消失。
// Double-stroke large cards so their outlines survive low-contrast E-ink rendering.
static void file_card_border(uint8_t *fb, EpdRect rect, int radius) {
    ui_draw_control_frame(fb, rect, radius, 0x50);
}

static void fit_name(char *name, int px, int width) {
    while (*name && ttf_text_width_px(ui_text_effective_px(px), name) > width) {
        size_t n = strlen(name) - 1;
        while (n && ((unsigned char)name[n] & 0xc0) == 0x80) --n;
        name[n] = 0;
    }
}



static void editor_start(void) {
    if (strlen(s_selected.name) >= sizeof(s_editor)) {
        snprintf(s_message, sizeof(s_message), "名称过长，无法在设备上编辑");
        return;
    }
    copy_utf8(s_editor, sizeof(s_editor), s_selected.name);
    s_editor_ext[0] = 0;
    char *dot = strrchr(s_editor, '.');
    if (!s_selected.is_dir && dot && dot != s_editor) {
        copy_utf8(s_editor_ext, sizeof(s_editor_ext), dot);
        *dot = 0;
    }
    ui_text_edit_init(&s_editor_input, s_editor, sizeof(s_editor));
    s_editor_notice[0] = 0;
    ui_keyboard_begin(&s_editor_input, false);
    s_view = FILE_VIEW_RENAME;
}







static void render_actions(uint8_t *fb) {
    const int offset=file_sheet_offset();
    // S03：保留当前目录作背景，只叠加文件操作底部面板。/ S03: keep the directory visible and overlay a file-action sheet.
    EpdRect sheet = {20, 430+offset, 644, 626};
    ui_fill_round_rect(fb, sheet, 28, UI_GRAY_WHITE);
    file_card_border(fb, sheet, 28);
    ui_fill_round_rect(fb, (EpdRect){604, 454+offset, 38, 38}, 19, 0xe0);
    ui_text_vc(fb, 623, 473+offset, 24, "×", EPD_DRAW_ALIGN_CENTER, false);
    char name[128]; copy_utf8(name, sizeof(name), s_selected.name); fit_name(name, 28, 540);
    file_icon(fb, 48, 467+offset, s_selected.is_dir);
    ui_text(fb, 102, 462+offset, 28, name, EPD_DRAW_ALIGN_LEFT, false);
    char detail[48]; file_detail(detail, sizeof(detail), &s_selected);
    ui_text(fb, 102, 507+offset, 20, detail, EPD_DRAW_ALIGN_LEFT, false);
    char path[128]; copy_utf8(path, sizeof(path), s_selected.path); fit_name(path, 19, 585);
    ui_text(fb, 50, 552+offset, 19, path, EPD_DRAW_ALIGN_LEFT, false);
    file_rule(fb, 42, 590+offset, 600);
    if (s_message[0]) ui_text(fb, 50, 608+offset, 20, s_message, EPD_DRAW_ALIGN_LEFT, false);
    if (s_delete_confirm) {
        ui_text_vc(fb, 342, 712+offset, s_selected.is_dir ? 23 : 28,
                   s_selected.is_dir ? "将删除文件夹内所有内容，无法恢复" : "删除后无法恢复，是否继续？",
                   EPD_DRAW_ALIGN_CENTER, false);
        ui_draw_button(fb, (EpdRect){50, 788+offset, 278, 76}, "取消", false);
        ui_draw_button(fb, (EpdRect){356, 788+offset, 278, 76}, "确认删除", true);
    } else {
        static const char *labels[] = {"重命名", "复制", "移动", "删除"};
        for (int i = 0; i < 4; ++i) {
            EpdRect button = {40 + (i % 2) * 312, 648 + (i / 2) * 122, 292, 100};
            ui_fill_round_rect(fb, button, 20, 0xe8);
            ui_draw_control_frame(fb, button, 20, 0x60);
            ui_text_vc(fb, button.x + button.width / 2, button.y + 50, 25,
                       labels[i], EPD_DRAW_ALIGN_CENTER, false);
        }
        ui_text_vc(fb, 342, 900+offset, 20, "长按文件或文件夹打开操作", EPD_DRAW_ALIGN_CENTER, false);
        ui_fill_round_rect(fb, (EpdRect){40, 930+offset, 604, 76}, 26, 0xe0);
        ui_text_vc(fb, 342, 968+offset, 23, "取消", EPD_DRAW_ALIGN_CENTER, false);
    }
}

static void render_rename(uint8_t *fb) {
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, "重命名", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 646, 107, 25, "完成", EPD_DRAW_ALIGN_RIGHT, false);
    file_rule(fb, 36, 157, 612);
    ui_text(fb, 36, 202, 22, "文件名", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect field = {36, 242, 612, 82};
    ui_text_input_draw(fb, &s_editor_input, field, 29, false, s_editor_ext);
    if (s_editor_notice[0]) ui_text_fixed_vc(fb, 36, 510, 22, s_editor_notice, EPD_DRAW_ALIGN_LEFT, false);
    ui_keyboard_draw(fb, UI_SYSTEM_KEYBOARD_TOP);
}

static bool storage_space_valid(const read_pico_sd_info_t *info) {
    return info->mounted && info->space_valid && info->filesystem_bytes &&
           info->free_bytes<=info->filesystem_bytes && info->filesystem_bytes<=info->capacity_bytes;
}
static void storage_amount(char *text,size_t size,uint64_t bytes) {
    if (bytes>=1073741824ULL) snprintf(text,size,"%.2f GB",bytes/1073741824.0);
    else if (bytes>=1048576ULL) snprintf(text,size,"%.1f MB",bytes/1048576.0);
    else if (bytes) snprintf(text,size,"%.1f KB",bytes/1024.0);
    else snprintf(text,size,"0 MB");
}
static void storage_usage(const read_pico_sd_info_t *info,char *total,size_t total_size,char *usage,size_t usage_size) {
    storage_amount(total,total_size,info->space_valid?info->filesystem_bytes:info->capacity_bytes);
    if (!storage_space_valid(info)) { snprintf(usage,usage_size,info->space_error==ESP_ERR_NOT_FINISHED?
        "正在统计空间，文件可正常使用":"空间统计暂不可用，请重新进入此页"); return; }
    char used[24],available[24];
    storage_amount(used,sizeof(used),info->filesystem_bytes-info->free_bytes);
    storage_amount(available,sizeof(available),info->free_bytes);
    snprintf(usage,usage_size,"已用 %s · 可用 %s",used,available);
}
static void render(app_ctx_t *ctx,uint8_t *fb);
static void render_content(app_ctx_t *ctx, uint8_t *fb) {
    if (s_view == FILE_VIEW_ACTIONS) {
        file_view_t restore = s_view;
        s_view = FILE_VIEW_LIST;
        render(ctx, fb);
        s_view = restore;
        render_actions(fb);
        return;
    }
    (void)ctx;
    ui_clear_page(fb);
    ui_nav_status(fb);
    if (s_view == FILE_VIEW_RENAME) {
        render_rename(fb);
        files_navigation(fb);
        return;
    }
    if (s_view == FILE_VIEW_SEARCH) {
        render_search(fb);
        files_navigation(fb);
        return;
    }
    if (s_folder >= 0) {
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, s_view == FILE_VIEW_MOVE ? "选择目标文件夹" : names[s_folder], EPD_DRAW_ALIGN_CENTER, false);
        // 搜索入口（仅普通文件夹列表，非移动目标选择）。/ Search entry (folder list only, not move-target).
        if (s_view == FILE_VIEW_LIST && !s_query[0]) {
            EpdRect r_multi = files_multi_rect(), r_mid = files_grid_rect(), r_right = files_search_entry_rect();
            if (s_select_mode) {
                ui_text_vc(fb, r_multi.x + r_multi.width / 2, 107, 22, "取消", EPD_DRAW_ALIGN_CENTER, false);
                ui_text_vc(fb, r_mid.x + r_mid.width / 2, 107, 22, "全选", EPD_DRAW_ALIGN_CENTER, false);
                char del[20]; snprintf(del, sizeof(del), "删除%u", (unsigned)selected_count());
                ui_text_vc(fb, r_right.x + r_right.width / 2, 107, 22, del, EPD_DRAW_ALIGN_CENTER, false);
            } else {
                ui_text_vc(fb, r_multi.x + r_multi.width / 2, 107, 22, "多选", EPD_DRAW_ALIGN_CENTER, false);
                ui_text_vc(fb, r_mid.x + r_mid.width / 2, 107, 22, s_file_grid ? "列表" : "网格", EPD_DRAW_ALIGN_CENTER, false);
                ui_text_vc(fb, r_right.x + r_right.width / 2, 107, 22, "搜索", EPD_DRAW_ALIGN_CENTER, false);
            }
        }
        if (s_select_mode) {
            // 多选时该行改为批量移动入口；右侧显示已选数量。/ In select mode this line is the batch-move entry; right shows the count.
            ui_text(fb, 36, 172, 20, "→ 移动所选", EPD_DRAW_ALIGN_LEFT, false);
            char sel[24]; snprintf(sel, sizeof(sel), "已选 %d", selected_count());
            ui_text(fb, 648, 172, 20, sel, EPD_DRAW_ALIGN_RIGHT, false);
        } else {
            char path[144];
            if (s_query[0]) snprintf(path, sizeof(path), "搜索：%s（点按清除）", s_query);
            else snprintf(path, sizeof(path), "TF卡%s", folder_root(s_folder) + 7);
            ui_text(fb, 36, 172, 20, path, EPD_DRAW_ALIGN_LEFT, false);
            int folders = 0, files = 0;
            for (int i = 0; i < s_count; ++i) s_files[i].is_dir ? ++folders : ++files;
            char summary[64];
            if (s_query[0]) snprintf(summary, sizeof(summary), "%d 项", s_count);
            else snprintf(summary, sizeof(summary), "%d夹·%d文件·%s", folders, files, files_sort_label());
            ui_text(fb, 648, 172, 20, summary, EPD_DRAW_ALIGN_RIGHT, false);
        }
        file_rule(fb, 36, 202, 612);
        if (s_message[0] && s_folder != 3) ui_text(fb, 36, 238, 27, s_message, EPD_DRAW_ALIGN_LEFT, false);
        else if (!s_count) ui_text(fb, 36, 238, 27, "此目录还没有文件", EPD_DRAW_ALIGN_LEFT, false);
        for (int row = 0; row < visible_rows(); ++row) {
            int index = s_page * visible_rows() + row;
            if (index >= s_count) break;
            char name[128]; snprintf(name, sizeof(name), "%s", s_files[index].name);
            char detail[32]; file_detail(detail, sizeof(detail), &s_files[index]);
            if (files_grid_active()) {
                EpdRect r = files_entry_rect(row);
                fit_name(name, 20, r.width - 20);
                file_icon(fb, r.x + r.width / 2 - 23, r.y + 24, s_files[index].is_dir);
                ui_text_vc(fb, r.x + r.width / 2, r.y + 112, 21, name, EPD_DRAW_ALIGN_CENTER, false);
                ui_text_vc(fb, r.x + r.width / 2, r.y + 152, 17, detail, EPD_DRAW_ALIGN_CENTER, false);
                if (s_select_mode) { EpdRect cb = {r.x + r.width - 30, r.y + 8, 24, 24}; if (s_files[index].selected) ui_fill_round_rect(fb, cb, 6, UI_GRAY_BLACK); else ui_draw_control_frame(fb, cb, 6, UI_GRAY_BLACK); }
            } else {
                int y = 214 + row * 88;
                while (*name && ttf_text_width_px(ui_text_effective_px(23), name) > 450) {
                    size_t n = strlen(name) - 1;
                    while (n && ((unsigned char)name[n] & 0xc0) == 0x80) --n;
                    name[n] = 0;
                }
                file_icon(fb, 48, y + 20, s_files[index].is_dir);
                ui_text(fb, 101, y + 11, 23, name, EPD_DRAW_ALIGN_LEFT, false);
                ui_text(fb, 101, y + 49, 19, detail, EPD_DRAW_ALIGN_LEFT, false);
                const char *tag = !strcmp(s_files[index].path, app_settings_books_dir()) ? "书籍目录" :
                                  !strcmp(s_files[index].path, app_settings_fonts_dir()) ? "字体目录" : NULL;
                if (tag) ui_text(fb, 588, y + 35, 18, tag, EPD_DRAW_ALIGN_RIGHT, false);
                if (s_select_mode) { EpdRect cb = {610, y + 28, 26, 26}; if (s_files[index].selected) ui_fill_round_rect(fb, cb, 6, UI_GRAY_BLACK); else ui_draw_control_frame(fb, cb, 6, UI_GRAY_BLACK); }
                else ui_text(fb, 640, y + 30, 22, "›", EPD_DRAW_ALIGN_RIGHT, false);
                file_rule(fb, 101, y + 82, 547);
            }
        }
        if (s_folder == 3 || s_view == FILE_VIEW_MOVE) {
            ui_draw_control_frame(fb, file_action_rect(0), 8, UI_GRAY_BLACK);
            ui_draw_control_frame(fb, file_action_rect(1), 8, UI_GRAY_BLACK);
            ui_text(fb, 182, file_action_rect(0).y+22, 24, s_view == FILE_VIEW_MOVE ? "取消移动" : "设为书籍目录", EPD_DRAW_ALIGN_CENTER, false);
            ui_text(fb, 501, file_action_rect(1).y+22, 24, s_view == FILE_VIEW_MOVE ? "移动到此处" : "设为字体目录", EPD_DRAW_ALIGN_CENTER, false);
            if (s_message[0]) ui_text(fb, 36, file_message_top(), 22, s_message, EPD_DRAW_ALIGN_LEFT, false);
        }
    } else {
        ui_text(fb, 36, 91, 52, "文件", EPD_DRAW_ALIGN_LEFT, false);
        // 根页也放 多选/网格/搜索：点它们进入 TF 卡目录并套用对应模式。/ Root-page shortcuts into the TF-card list.
        {
            EpdRect r_m = files_multi_rect(), r_g = files_grid_rect(), r_s = files_search_entry_rect();
            ui_text_vc(fb, r_m.x + r_m.width / 2, 107, 22, "多选", EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, r_g.x + r_g.width / 2, 107, 22, s_file_grid ? "列表" : "网格", EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, r_s.x + r_s.width / 2, 107, 22, "搜索", EPD_DRAW_ALIGN_CENTER, false);
        }
        EpdRect storage = storage_card_rect();
        ui_fill_round_rect(fb, storage, 24, UI_GRAY_WHITE);
        file_card_border(fb, storage, 24);
        ui_text(fb, 58, 186, 27, "存储卡", EPD_DRAW_ALIGN_LEFT, false);
        char total[32], usage[80];
        if (s_sd.mounted) {
            storage_usage(&s_sd,total,sizeof(total),usage,sizeof(usage));
        } else if (sd_needs_format()) {
            // 卡在但读不了：说清是文件系统的问题，并给出可点的出路。
            // Present but unreadable: say the filesystem is the problem and offer the way out.
            snprintf(total, sizeof(total), "需要格式化");
            snprintf(usage, sizeof(usage), "%s", s_sd_notice[0] ? s_sd_notice
                     : (s_sd_format_confirm ? "将清空卡内全部数据，再点确认" : "格式化可能可以解决此问题"));
        } else {
            snprintf(total, sizeof(total), "未挂载");
            snprintf(usage, sizeof(usage), "请检查 TF 卡");
        }
        ui_text(fb, 624, 189, 21, total, EPD_DRAW_ALIGN_RIGHT, false);
        ui_text(fb, 58, 224, 19, usage, EPD_DRAW_ALIGN_LEFT, false);
        {
            char bstat[64];
            if (!s_book_stat_done) snprintf(bstat, sizeof(bstat), "书籍统计…");
            else if (s_book_stat_count) { char amt[16]; storage_amount(amt, sizeof(amt), s_book_stat_bytes); snprintf(bstat, sizeof(bstat), "书籍 %u 本 · %s", s_book_stat_count, amt); }
            else snprintf(bstat, sizeof(bstat), "书籍 0 本");
            ui_text(fb, 58, 268, 20, bstat, EPD_DRAW_ALIGN_LEFT, false);
        }
        EpdRect capacity = {58, 250, 566, 6};
        ui_fill_round_rect(fb, capacity, 3, 0xa0);
        if (storage_space_valid(&s_sd)) {
            capacity.width = (int)((s_sd.filesystem_bytes - s_sd.free_bytes) * 566 / s_sd.filesystem_bytes);
            if (capacity.width > 0) ui_fill_round_rect(fb, capacity, 3, 0x68);
        }
        ui_text(fb, 42, 310, 21, "传输文件", EPD_DRAW_ALIGN_LEFT, false);
        static const char *const methods[] = {"WiFi传输", "热点传输", "USB传输"};
        static const char *const details[] = {"同一网络", "连接设备热点", "连接电脑"};
        for (int i = 0; i < 3; ++i) {
            EpdRect card = home_transfer_rect(i);
            ui_fill_round_rect(fb, card, 20, UI_GRAY_WHITE);
            ui_draw_control_frame(fb, card, 20, UI_GRAY_BLACK);
            ui_text_vc(fb, card.x + card.width / 2, card.y + 35, 21, methods[i],
                       EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, card.x + card.width / 2, card.y + 75, 19, details[i],
                       EPD_DRAW_ALIGN_CENTER, false);
        }
        char summary[48]; snprintf(summary, sizeof(summary), "根目录 · %d 项", s_count);
        ui_text(fb, 36, 456, 21, summary, EPD_DRAW_ALIGN_LEFT, false);
        ui_fill_round_rect(fb, (EpdRect){537, 458, 111, 50}, 23, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, (EpdRect){537, 458, 111, 50}, 23, 0x70);
        ui_text_vc(fb, 592, 483, 19, "名称 ↑", EPD_DRAW_ALIGN_CENTER, false);
        int first = s_page * root_rows();
        int rows = s_count - first;
        if (rows > root_rows()) rows = root_rows();
        if (rows < 0) rows = 0;
        EpdRect list = {36, 520, 612, root_rows() * 91};
        ui_fill_round_rect(fb, list, 24, UI_GRAY_WHITE);
        file_card_border(fb, list, 24);
        if (!s_count) ui_text_vc(fb, 342, 565, 22, s_message[0] ? s_message : "根目录为空", EPD_DRAW_ALIGN_CENTER, false);
        for (int row = 0; row < root_rows(); ++row) {
            int index = first + row;
            if (index >= s_count) break;
            int y = 520 + row * 91;
            if (row) file_rule(fb, 96, y, 532);
            char name[128]; copy_utf8(name, sizeof(name), s_files[index].name); fit_name(name, 23, 430);
            file_icon(fb, 52, y + 24, s_files[index].is_dir);
            if (s_files[index].is_dir)
                ui_text_vc(fb, 101, y + 46, 23, name, EPD_DRAW_ALIGN_LEFT, false);
            else
                ui_text(fb, 101, y + 20, 23, name, EPD_DRAW_ALIGN_LEFT, false);
            char detail[32]; file_detail(detail, sizeof(detail), &s_files[index]);
            if (s_files[index].is_dir) ui_text_vc(fb, 628, y + 46, 24, "›", EPD_DRAW_ALIGN_RIGHT, false);
            else {
                ui_text(fb, 101, y + 55, 19, detail, EPD_DRAW_ALIGN_LEFT, false);
                ui_text_vc(fb, 624, y + 44, 22, "›", EPD_DRAW_ALIGN_RIGHT, false);
            }
        }
    }
    draw_file_pager(fb);
    files_navigation(fb);
}

static void render(app_ctx_t *ctx,uint8_t *fb) {
    bool previous=s_render_secondary;
    s_render_secondary=previous || !main_page_visible(ctx);
    render_content(ctx,fb);
    s_render_secondary=previous;
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_pager_pressed=UI_PAGER_NONE;s_pager_paint_pending=false;
    s_folder = s_requested_folder; s_requested_folder = -1;
    s_view = FILE_VIEW_LIST;
    s_delete_confirm = false;
    search_close(); s_query[0] = 0; s_select_mode = false; s_batch_confirm = false; batch_src_clear();
    snprintf(s_dir, sizeof(s_dir), "/sdcard");
    s_page = 0; s_scan_pending = true;
    read_pico_sd_start_probe();
    const esp_err_t info_status=read_pico_sd_get_info(&s_sd);
    s_space_poll_ms=ctx->now_ms;
    s_sd_format_confirm = false;
    s_sd_notice[0] = 0;
    for (int i = 0; i < 3; ++i) s_counts[i] = count_root(i);
    if (s_folder >= 0) scan_folder(s_folder);
    else { scan_folder(3); book_stat_start(); }
    // 已完成首帧目录扫描时不在下一拍重复统计并推整页；挂载/分配未完成仍沿原重试路径。
    // Do not repeat a completed first-frame scan and whole-page present on the next tick; retain mount/allocation retries.
    if(info_status!=ESP_ERR_NOT_FINISHED&&s_files)s_scan_pending=false;
}
static void on_media_lost(app_ctx_t *ctx) {
    s_pager_pressed=UI_PAGER_NONE;s_pager_paint_pending=false;
    (void)ctx; ui_keyboard_end(); search_close(); s_query[0] = 0; s_select_mode = false; s_batch_confirm = false; s_folder = -1; s_count = 0; s_scan_pending = false;
    s_view = FILE_VIEW_LIST; s_delete_confirm = false;
    memset(s_counts, 0, sizeof(s_counts)); read_pico_sd_get_info(&s_sd);
}
static void on_media_ready(app_ctx_t *ctx) {
    (void)ctx;
    s_scan_pending = true;
    if (s_folder < 0) book_stat_start();
}
static app_redraw_t rename_paint(app_ctx_t *ctx, bool field);
static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (s_view == FILE_VIEW_RENAME) {
        if (ctx->consumed) return APP_REDRAW_NONE;
        bool held = !ctx->released && ctx->touch && ctx->touch->touched && ctx->touch->count == 1;
        if (ui_keyboard_hold_tick(held, ctx->touch ? ctx->touch->x : 0,
            ctx->touch ? ctx->touch->y : 0, ctx->now_ms) == UI_KEYBOARD_CHANGED) return rename_paint(ctx, false);
        return ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms) == UI_KEYBOARD_CHANGED
            ? rename_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (s_view == FILE_VIEW_SEARCH) {
        if (ctx->consumed) return APP_REDRAW_NONE;
        bool held = !ctx->released && ctx->touch && ctx->touch->touched && ctx->touch->count == 1;
        if (ui_keyboard_hold_tick(held, ctx->touch ? ctx->touch->x : 0,
            ctx->touch ? ctx->touch->y : 0, ctx->now_ms) == UI_KEYBOARD_CHANGED) return search_paint(ctx, false);
        return ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms) == UI_KEYBOARD_CHANGED
            ? search_paint(ctx, false) : APP_REDRAW_NONE;
    }
    // 增量递归搜索：每次有界步进，最多每 500ms 重绘一次。/ Incremental recursive search: bounded steps, repaint at most every 500ms.
    if (s_fs && s_view == FILE_VIEW_LIST && !(ctx->touch && ctx->touch->touched)) {
        file_search_step(s_fs, 24);
        file_search_stats_t st = file_search_stats(s_fs);
        s_fs_matches = st.matched;
        if (st.done) { file_search_close(s_fs); s_fs = NULL; qsort(s_files, s_count, sizeof(s_files[0]), compare_files); }
        if (st.done || ctx->now_ms - s_search_paint_ms >= 500) { s_search_paint_ms = ctx->now_ms; return APP_REDRAW_PAGE; }
    }
    // 根页书籍统计增量步进。/ Incremental book-stat walk on the root page.
    if (s_folder < 0 && s_book_stat_fs && !s_book_stat_done && ctx->now_ms >= s_book_stat_next_ms) {
        s_book_stat_next_ms = ctx->now_ms + 500;
        file_search_step(s_book_stat_fs, 24);
        if (file_search_stats(s_book_stat_fs).done) { file_search_close(s_book_stat_fs); s_book_stat_fs = NULL; s_book_stat_done = true; }
        return APP_REDRAW_PAGE;
    }
    if (!s_scan_pending) {
        if (s_folder<0 && s_sd.mounted && !(ctx->touch && ctx->touch->touched) && ctx->now_ms-s_space_poll_ms>=
            (s_sd.space_valid?15000:500)
            ) {
            s_space_poll_ms=ctx->now_ms;
            read_pico_sd_info_t previous=s_sd;
            (void)read_pico_sd_start_space_refresh();
            read_pico_sd_get_info(&s_sd);
            if (previous.space_valid!=s_sd.space_valid || previous.filesystem_bytes!=s_sd.filesystem_bytes || previous.free_bytes!=s_sd.free_bytes)
                return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (read_pico_sd_get_info(&s_sd) == ESP_ERR_NOT_FINISHED) return APP_REDRAW_NONE;
    s_scan_pending = false;
    s_space_poll_ms=ctx->now_ms;
    for (int i = 0; i < 3; ++i) s_counts[i] = count_root(i);
    if (s_folder >= 0) scan_folder(s_folder);
    else scan_folder(3);
    return APP_REDRAW_PAGE;
}

static void open_selected(app_ctx_t *ctx) {
    const char *ext = strrchr(s_selected.name, '.');
    if (!ext) { snprintf(s_message, sizeof(s_message), "没有可用的打开方式"); return; }
    if (!strcasecmp(ext, ".epub") || !strcasecmp(ext, ".txt") ||
        !strcasecmp(ext, ".md") || !strcasecmp(ext, ".markdown")) {
        extern const app_desc_t app_book;
        if (app_book_request_open(s_selected.path)) ctx->request_app = &app_book;
    } else if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png") || !strcasecmp(ext, ".webp")) {
        extern const app_desc_t app_image;
        if (app_image_request_open(s_selected.path)) ctx->request_app = &app_image;
    } else if (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf")) {
        if (ttf_font_open(s_selected.path) == ESP_OK) {
            app_settings_set_system_font_path(s_selected.path);
            app_font_activate_system();
            snprintf(s_message, sizeof(s_message), "已设为系统字体");
        } else snprintf(s_message, sizeof(s_message), "字体无法加载");
    } else snprintf(s_message, sizeof(s_message), "没有可用的打开方式");
}

static void delete_selected(void) {
    char old_path[288]; copy_utf8(old_path, sizeof(old_path), s_selected.path);
    esp_err_t err = file_tree_delete(old_path, file_deleted, NULL);
    if (err != ESP_OK) {
        s_delete_confirm = false;
        repair_deleted_directories();
        book_store_notify_changed();
        rescan_current();
        snprintf(s_message, sizeof(s_message), "删除未完成，请检查 TF 卡");
        return;
    }
    repair_deleted_directories();
    book_store_notify_changed();
    s_view = FILE_VIEW_LIST; s_delete_confirm = false;
    rescan_current();
    snprintf(s_message, sizeof(s_message), s_selected.is_dir ? "文件夹已删除" : "文件已删除");
}
static int selected_count(void) { int n = 0; for (int i = 0; i < s_count; ++i) if (s_files[i].selected) ++n; return n; }
static void clear_selection(void) { for (int i = 0; i < s_count; ++i) s_files[i].selected = false; }
static void batch_delete_selected(void) {
    unsigned done = 0; bool failed = false;
    for (int i = 0; i < s_count; ++i) {
        if (!s_files[i].selected) continue;
        char p[288]; copy_utf8(p, sizeof(p), s_files[i].path);
        if (file_tree_delete(p, file_deleted, NULL) == ESP_OK) ++done; else failed = true;
    }
    repair_deleted_directories(); book_store_notify_changed();
    s_select_mode = false; s_batch_confirm = false; s_view = FILE_VIEW_LIST; rescan_current();
    snprintf(s_message, sizeof(s_message), failed ? "部分删除失败，已删 %u 项" : "已删除 %u 项", done);
}

static app_redraw_t save_rename(void) {
    if (ui_keyboard_pending()) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "请先选择候选字");
        return APP_REDRAW_PAGE;
    }
    char *start = s_editor;
    while (*start == ' ') ++start;
    if (start != s_editor) memmove(s_editor, start, strlen(start) + 1);
    size_t len = strlen(s_editor);
    while (len && (s_editor[len - 1] == ' ' || s_editor[len - 1] == '.')) s_editor[--len] = 0;
    if (!len || strchr(s_editor, '/') || strchr(s_editor, '\\') || strchr(s_editor, ':')) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "请输入有效文件名");
        return APP_REDRAW_PAGE;
    }
    char name[128], destination[288];
    if (snprintf(name, sizeof(name), "%s%s", s_editor, s_editor_ext) >= sizeof(name) ||
        !sibling_path(name, destination)) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "文件名过长");
        return APP_REDRAW_PAGE;
    }
    if (!strcmp(destination, s_selected.path)) { s_view = FILE_VIEW_ACTIONS; return APP_REDRAW_PAGE; }
    struct stat existing;
    if (stat(destination, &existing) == 0) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "同名文件已经存在");
        return APP_REDRAW_PAGE;
    }
    char old_path[288]; copy_utf8(old_path, sizeof(old_path), s_selected.path);
    if (!moved_settings_fit(old_path, destination)) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "设置路径过长");
        return APP_REDRAW_PAGE;
    }
    if (file_tree_move(old_path, destination) != ESP_OK) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "重命名失败，请检查 TF 卡");
        return APP_REDRAW_PAGE;
    }
    sync_moved_settings(old_path, destination);
    notify_moved_tree(old_path, destination, 0);
    book_store_notify_changed();
    copy_utf8(s_selected.path, sizeof(s_selected.path), destination);
    copy_utf8(s_selected.name, sizeof(s_selected.name), name);
    s_view = FILE_VIEW_ACTIONS;
    ui_keyboard_end();
    snprintf(s_message, sizeof(s_message), "重命名完成");
    return APP_REDRAW_PAGE;
}

static app_redraw_t action_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    const int y=(int)ev->y0-file_sheet_offset();
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (y < 430 || (ev->x0 >= 596 && y < 510) || y >= 918) {
        s_view = FILE_VIEW_LIST; s_delete_confirm = false; rescan_current(); return APP_REDRAW_PAGE;
    }
    if (s_delete_confirm) {
        if (y >= 788 && y < 864) {
            if (ev->x0 < 342) s_delete_confirm = false;
            else delete_selected();
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (y >= 648 && y < 870) {
        int action = (y >= 770 ? 2 : 0) + (ev->x0 >= 352 ? 1 : 0);
        if (action == 0) editor_start();
        else if (action == 1) {
            char copied[128] = {0};
            esp_err_t err = copy_selected_item(copied);
            if (err == ESP_OK) snprintf(s_message, sizeof(s_message), "已复制：%.74s", copied);
            else snprintf(s_message, sizeof(s_message), err == ESP_ERR_NO_MEM ? "内存不足，复制失败" : "复制失败，请检查空间");
        } else if (action == 2) start_move();
        else { s_delete_confirm = true; s_message[0] = 0; }
        return ctx->request_app ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static EpdRect s_input_area;
static bool s_input_layout, s_input_settle;
static app_redraw_t rename_paint(app_ctx_t *ctx, bool field) {
    ui_keyboard_update_t update = ui_keyboard_update(ctx->fb, UI_SYSTEM_KEYBOARD_TOP, (EpdRect){36, 242, 612, 82}, 29, false, s_editor_ext, field);
    s_input_area = update.area; s_input_layout = update.layout; s_input_settle = update.settle;
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}
static bool files_present(app_ctx_t *ctx, app_redraw_t redraw) {
    bool pager=s_pager_paint_pending;s_pager_paint_pending=false;
    if(pager&&redraw==APP_REDRAW_AREA&&!ctx->request_app&&
        (s_view==FILE_VIEW_LIST||s_view==FILE_VIEW_MOVE)){
        guard_draw_result(ctx->hl,update_display_area_diff_with(ctx->hl,&E0470_FOLLOW_WAVEFORM,MODE_DU,ui_pager_area_at(file_pager_top())));
        return true;
    }
    if (redraw != APP_REDRAW_AREA || (s_view != FILE_VIEW_RENAME && s_view != FILE_VIEW_SEARCH)) return false;
    if (s_input_settle) {
        guard_draw_result(ctx->hl, update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, s_input_area));
        s_input_settle = false;
        return true;
    }
    guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl,
        s_input_layout ? &E0470_WAVEFORM : &E0470_FOLLOW_WAVEFORM,
        s_input_layout ? MODE_GL16 : MODE_DU, s_input_area));
    return true;
}
static app_redraw_t rename_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_PRESS)
        return ui_keyboard_press(ev->x0, ev->y0, UI_SYSTEM_KEYBOARD_TOP, ctx->now_ms) ? rename_paint(ctx, false) : APP_REDRAW_NONE;
    bool feedback = ev->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
        bool page = ui_keyboard_page(ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        return (page || feedback) ? rename_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return feedback ? rename_paint(ctx, false) : APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    if (y < 160 && x < 160) { ui_keyboard_end(); s_view = FILE_VIEW_ACTIONS; return APP_REDRAW_PAGE; }
    if (y < 160 && x > 510) return save_rename();
    if (ui_text_input_tap(&s_editor_input, (EpdRect){36, 242, 612, 82}, 29,
                          false, s_editor_ext, x, y)) return rename_paint(ctx, true);
    ui_keyboard_result_t result = ui_keyboard_tap(x, y, UI_SYSTEM_KEYBOARD_TOP, esp_timer_get_time() / 1000);
    if (result == UI_KEYBOARD_DONE) return save_rename();
    return result == UI_KEYBOARD_CHANGED ? rename_paint(ctx, false) : APP_REDRAW_NONE;
}

/* ---- 递归搜索 / Recursive search ---- */
// 搜索入口按钮（文件页标题行右侧）。/ Search entry button (folder title row, right side).
static EpdRect files_search_entry_rect(void) { return (EpdRect){586, 86, 74, 44}; }
static void search_close(void) { if (s_fs) { file_search_close(s_fs); s_fs = NULL; } }
static void search_found_cb(const char *path, const struct stat *info, void *ctx) {
    (void)ctx;
    if (!info || !S_ISREG(info->st_mode) || s_count >= FILE_MAX) return;
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    if (s_folder >= 0 && s_folder != 3 && !supported(s_folder, name)) return;
    file_item_t *item = &s_files[s_count];
    if (strlen(path) >= sizeof(item->path)) return;
    copy_utf8(item->path, sizeof(item->path), path);
    copy_utf8(item->name, sizeof(item->name), name);
    item->size = info->st_size; item->mtime = (int64_t)info->st_mtime; item->is_dir = false;
    ++s_count;
}
static void search_clear(void) { search_close(); s_query[0] = 0; scan_folder(s_folder >= 0 ? s_folder : 3); }
static void search_begin(void) {
    s_query[0] = 0; ui_text_edit_init(&s_query_input, s_query, sizeof(s_query));
    ui_keyboard_begin(&s_query_input, false); s_view = FILE_VIEW_SEARCH;
}
static void render_search(uint8_t *fb) {
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, "搜索", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 646, 107, 25, "搜索", EPD_DRAW_ALIGN_RIGHT, false);
    file_rule(fb, 36, 157, 612);
    ui_text(fb, 36, 202, 22, "文件名 / 拼音 / 首字母", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect field = {36, 242, 612, 82};
    ui_text_input_draw(fb, &s_query_input, field, 29, false, "");
    ui_keyboard_draw(fb, UI_SYSTEM_KEYBOARD_TOP);
}
static app_redraw_t search_paint(app_ctx_t *ctx, bool field) {
    ui_keyboard_update_t u = ui_keyboard_update(ctx->fb, UI_SYSTEM_KEYBOARD_TOP, (EpdRect){36, 242, 612, 82}, 29, false, "", field);
    s_input_area = u.area; s_input_layout = u.layout; s_input_settle = u.settle;
    return u.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}
static app_redraw_t search_apply(void) {
    ui_keyboard_end();
    s_view = FILE_VIEW_LIST;
    search_close();
    if (!s_query[0]) { scan_folder(s_folder >= 0 ? s_folder : 3); return APP_REDRAW_PAGE; }
    if (!files_alloc()) return APP_REDRAW_NONE;
    s_count = s_page = 0; s_message[0] = 0; s_fs_matches = 0;
    int folder = s_folder >= 0 ? s_folder : 3;
    s_fs = file_search_begin(folder_root(folder), s_query, search_found_cb, NULL);
    if (!s_fs) { snprintf(s_message, sizeof(s_message), "无法搜索当前目录"); scan_folder(folder); s_query[0] = 0; }
    return APP_REDRAW_PAGE;
}
static app_redraw_t search_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_PRESS)
        return ui_keyboard_press(ev->x0, ev->y0, UI_SYSTEM_KEYBOARD_TOP, ctx->now_ms) ? search_paint(ctx, false) : APP_REDRAW_NONE;
    bool feedback = ev->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
        bool page = ui_keyboard_page(ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        return (page || feedback) ? search_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return feedback ? search_paint(ctx, false) : APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    if (y < 160 && x < 160) { ui_keyboard_end(); s_view = FILE_VIEW_LIST; return APP_REDRAW_PAGE; }
    if (y < 160 && x > 510) return search_apply();
    if (ui_text_input_tap(&s_query_input, (EpdRect){36, 242, 612, 82}, 29, false, "", x, y)) return search_paint(ctx, true);
    ui_keyboard_result_t result = ui_keyboard_tap(x, y, UI_SYSTEM_KEYBOARD_TOP, esp_timer_get_time() / 1000);
    if (result == UI_KEYBOARD_DONE) return search_apply();
    return result == UI_KEYBOARD_CHANGED ? search_paint(ctx, false) : APP_REDRAW_NONE;
}

/* ---- 书籍统计 / Book stats ---- */
static void book_stat_cb(const char *path, const struct stat *info, void *ctx) {
    (void)ctx;
    if (!info || !S_ISREG(info->st_mode)) return;
    if (ignored_book_artifact(path)) return;
    const char *ext = strrchr(path, '.'); if (!ext) return;
    if (strcasecmp(ext, ".epub") && strcasecmp(ext, ".txt") &&
        strcasecmp(ext, ".md") && strcasecmp(ext, ".markdown")) return;
    ++s_book_stat_count; s_book_stat_bytes += (uint64_t)info->st_size;
}
static void book_stat_start(void) {
    if (s_book_stat_fs) { file_search_close(s_book_stat_fs); s_book_stat_fs = NULL; }
    s_book_stat_count = 0; s_book_stat_bytes = 0; s_book_stat_done = false; s_book_stat_next_ms = 0;
    s_book_stat_fs = file_search_begin_all(app_settings_books_dir(), book_stat_cb, NULL);
    if (!s_book_stat_fs) s_book_stat_done = true;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    app_redraw_t pager;
    if(file_pager_event(ctx,ev,&pager))return pager;
    if (s_view == FILE_VIEW_ACTIONS) return action_gesture(ctx, ev);
    if (s_view == FILE_VIEW_RENAME) return rename_gesture(ctx, ev);
    if (s_view == FILE_VIEW_SEARCH) return search_gesture(ctx, ev);
    if (s_view == FILE_VIEW_MOVE) {
        if (ev->type == UI_GESTURE_SWIPE_L && (s_page + 1) * visible_rows() < s_count) {
            ++s_page; return APP_REDRAW_PAGE;
        }
        if (ev->type == UI_GESTURE_SWIPE_R && s_page > 0) { --s_page; return APP_REDRAW_PAGE; }
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (ev->y0 < 185) {
            if (strcmp(s_dir, "/sdcard")) {
                char *slash = strrchr(s_dir, '/'); if (slash) *slash = 0;
                scan_folder(3);
            } else leave_move(false);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(file_action_rect(0),ev->x0,ev->y0) || ui_rect_hit(file_action_rect(1),ev->x0,ev->y0)) {
            if (ev->x0 < 342) leave_move(false);
            else return s_batch_src_count > 0 ? batch_move_to_dir() : move_selected();
            return APP_REDRAW_PAGE;
        }
        int row = (ev->y0 - 214) / 88;
        int index = s_page * visible_rows() + row;
        if (ev->y0 >= 214 && row >= 0 && row < visible_rows() && index < s_count) {
            copy_utf8(s_dir, sizeof(s_dir), s_files[index].path);
            scan_folder(3);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_SWIPE_L && s_folder >= 0 && (s_page + 1) * visible_rows() < s_count) { ++s_page; return APP_REDRAW_PAGE; }
    if (ev->type == UI_GESTURE_SWIPE_R && s_folder >= 0 && s_page > 0) { --s_page; return APP_REDRAW_PAGE; }
    if (ev->type == UI_GESTURE_SWIPE_L && s_folder < 0 && (s_page + 1) * root_rows() < s_count) { ++s_page; return APP_REDRAW_PAGE; }
    if (ev->type == UI_GESTURE_SWIPE_R && s_folder < 0 && s_page > 0) { --s_page; return APP_REDRAW_PAGE; }
    if (ev->type != UI_GESTURE_TAP && ev->type != UI_GESTURE_LONG_PRESS) return APP_REDRAW_NONE;
    bool long_press = ev->type == UI_GESTURE_LONG_PRESS;
    if (long_press && ev->y0 < (s_folder >= 0 ? 214 : 520)) return APP_REDRAW_NONE;
    int tab = files_nav_hit(ev->x0, ev->y0);
    if (!long_press && tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    if (s_folder >= 0) {
        // 表头控件（多选/网格/搜索/排序）在 y<185 区，必须先于“返回上一级”判定，否则会被当成返回。
        // Header controls sit in y<185; they must be tested before the "go up" branch or they'd act as Back.
        bool hdr_hit = ui_rect_hit(files_multi_rect(), ev->x0, ev->y0) ||
                       ui_rect_hit(files_grid_rect(), ev->x0, ev->y0) ||
                       ui_rect_hit(files_search_entry_rect(), ev->x0, ev->y0) ||
                       ui_rect_hit(files_sort_rect(), ev->x0, ev->y0) ||
                       (s_query[0] && ev->y0 >= 154 && ev->y0 < 196);
        if (!long_press && !hdr_hit && ev->y0 < 185) {
            if (s_folder == 3 && strcmp(s_dir, "/sdcard")) {
                char *slash = strrchr(s_dir, '/'); if (slash) *slash = 0;
                scan_folder(3);
            } else s_folder = -1;
            return APP_REDRAW_PAGE;
        }
        if (!long_press && s_folder == 3 && (ui_rect_hit(file_action_rect(0),ev->x0,ev->y0) || ui_rect_hit(file_action_rect(1),ev->x0,ev->y0))) {
            struct stat selected;
            if (!strcmp(s_dir,"/sdcard") || stat(s_dir,&selected) || !S_ISDIR(selected.st_mode))
                snprintf(s_message,sizeof(s_message),"请先进入 TF 卡内的文件夹");
            else if (ev->x0 < 342) {
                if(app_settings_set_books_dir(s_dir)){
                    book_store_notify_changed();snprintf(s_message,sizeof(s_message),"已设为书籍目录");
                }else snprintf(s_message,sizeof(s_message),"保存失败或路径过长，请重试");
            }else{
                if(app_settings_set_fonts_dir(s_dir)){
                    ttf_font_scan();snprintf(s_message,sizeof(s_message),"已设为字体目录");
                }else snprintf(s_message,sizeof(s_message),"保存失败或路径过长，请重试");
            }
            s_counts[0]=count_root(0);s_counts[2]=count_root(2);
            return APP_REDRAW_PAGE;
        }
        // 搜索中：点表头整行清除搜索。/ While searching: tap the header line to clear.
        if (!long_press && s_query[0] && ev->y0 >= 154 && ev->y0 < 196) { search_clear(); return APP_REDRAW_PAGE; }
        // 搜索入口（仅普通文件夹列表）。/ Search entry (folder list only).
        if (!long_press && !s_query[0] && s_view == FILE_VIEW_LIST) {
            if (ui_rect_hit(files_multi_rect(), ev->x0, ev->y0)) { s_select_mode = !s_select_mode; clear_selection(); s_batch_confirm = false; return APP_REDRAW_PAGE; }
            if (s_select_mode) {
                if (ev->y0 >= 154 && ev->y0 < 196 && ev->x0 < 430) { batch_move_begin(); return APP_REDRAW_PAGE; }
                if (ui_rect_hit(files_selectall_rect(), ev->x0, ev->y0)) { bool all = selected_count() == s_count; for (int i = 0; i < s_count; ++i) s_files[i].selected = !all; s_batch_confirm = false; return APP_REDRAW_PAGE; }
                if (ui_rect_hit(files_batchdel_rect(), ev->x0, ev->y0)) {
                    if (!selected_count()) { snprintf(s_message, sizeof(s_message), "未选择文件"); return APP_REDRAW_PAGE; }
                    if (!s_batch_confirm) { s_batch_confirm = true; snprintf(s_message, sizeof(s_message), "再点一次删除所选（%d 项）", selected_count()); return APP_REDRAW_PAGE; }
                    s_batch_confirm = false; batch_delete_selected(); return APP_REDRAW_PAGE;
                }
            } else {
                if (ui_rect_hit(files_grid_rect(), ev->x0, ev->y0)) { s_file_grid = !s_file_grid; s_page = 0; return APP_REDRAW_PAGE; }
                if (ui_rect_hit(files_search_entry_rect(), ev->x0, ev->y0)) { search_begin(); return APP_REDRAW_FULL; }
                if (ui_rect_hit(files_sort_rect(), ev->x0, ev->y0)) { s_sort = (file_sort_t)((s_sort + 1) % SORT_COUNT); qsort(s_files, s_count, sizeof(s_files[0]), compare_files); s_page = 0; return APP_REDRAW_PAGE; }
            }
        }
        int row = files_entry_hit(ev->x0, ev->y0);
        int index = s_page * visible_rows() + row;
        if (row >= 0 && index < s_count) {
            if (s_select_mode) { s_files[index].selected = !s_files[index].selected; s_batch_confirm = false; return APP_REDRAW_PAGE; }
            if (long_press) {
                s_selected = s_files[index]; s_view = FILE_VIEW_ACTIONS;
                s_delete_confirm = false; s_message[0] = 0;
                return APP_REDRAW_PAGE;
            }
            if (s_folder == 3 && s_files[index].is_dir) {
                if (strlen(s_files[index].path) >= sizeof(s_dir)) {
                    snprintf(s_message, sizeof(s_message), "目录路径过长"); return APP_REDRAW_PAGE;
                }
                snprintf(s_dir, sizeof(s_dir), "%s", s_files[index].path);
                scan_folder(3);
                return APP_REDRAW_PAGE;
            }
            s_selected = s_files[index];
            open_selected(ctx);
            return ctx->request_app ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    // 存储卡卡片：只有"卡在但读不了"时响应，点第一次问、点第二次格。
    // The storage card answers only when the card is present but unreadable: the first tap asks,
    // the second one formats.
    if (!long_press && sd_needs_format() && ui_rect_hit(storage_card_rect(), ev->x0, ev->y0)) {
        if (!s_sd_format_confirm) {
            s_sd_format_confirm = true;
            s_sd_notice[0] = 0;
        } else {
            s_sd_format_confirm = false;
            snprintf(s_sd_notice, sizeof(s_sd_notice), "正在格式化…");
            esp_err_t err = (book_cover_jobs_quiesce(2000) && ui_avatar_quiesce(2000)) ? read_pico_sd_format() : ESP_ERR_NOT_FINISHED;
            read_pico_sd_get_info(&s_sd);
            if (err == ESP_OK) {
                s_sd_notice[0] = 0;
                s_scan_pending = true;
            } else {
                snprintf(s_sd_notice, sizeof(s_sd_notice), "格式化失败：%s", esp_err_to_name(err));
            }
        }
        return APP_REDRAW_PAGE;
    }
    // 点别处就撤销确认，免得提示一直停在"再点一次"。
    // A tap elsewhere cancels the confirmation, so the prompt does not stay armed.
    bool format_cancelled = !long_press && s_sd_format_confirm;
    if (format_cancelled) {
        s_sd_format_confirm = false;
        s_sd_notice[0] = 0;
    }
    for (int i = 0; !long_press && i < 3; ++i) if (ui_rect_hit(home_transfer_rect(i), ev->x0, ev->y0)) {
        extern const app_desc_t app_transfer;
        if (i == 0) app_transfer_request_wifi_upload();
        else if (i == 1) app_transfer_request_hotspot_start();
        else app_transfer_request_usb_start();
        ctx->request_app = &app_transfer;
        return APP_REDRAW_NONE;
    }
    // 根页右上角 多选/网格/搜索：进入 TF 卡目录（/sdcard）并套用对应模式。
    // Root-page top-right shortcuts: enter the TF-card list (/sdcard) with the chosen mode.
    if (!long_press) {
        if (ui_rect_hit(files_multi_rect(), ev->x0, ev->y0)) {
            s_folder = 3; copy_utf8(s_dir, sizeof(s_dir), "/sdcard"); scan_folder(3); s_select_mode = true; return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(files_grid_rect(), ev->x0, ev->y0)) {
            s_folder = 3; copy_utf8(s_dir, sizeof(s_dir), "/sdcard"); scan_folder(3); s_file_grid = !s_file_grid; s_page = 0; return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(files_search_entry_rect(), ev->x0, ev->y0)) {
            s_folder = 3; copy_utf8(s_dir, sizeof(s_dir), "/sdcard"); scan_folder(3); search_begin(); return APP_REDRAW_FULL;
        }
    }
    if (ev->y0 >= 520 && ev->y0 < 975) {
        int row = (ev->y0 - 520) / 91;
        int index = s_page * root_rows() + row;
        if (row >= 0 && row < root_rows() && index < s_count) {
            if (long_press) {
                s_selected = s_files[index]; s_view = FILE_VIEW_ACTIONS;
                s_delete_confirm = false; s_message[0] = 0;
                return APP_REDRAW_PAGE;
            }
            if (s_files[index].is_dir) {
                s_folder = 3;
                copy_utf8(s_dir, sizeof(s_dir), s_files[index].path);
                scan_folder(3);
                return APP_REDRAW_PAGE;
            }
            s_selected = s_files[index];
            open_selected(ctx);
            return ctx->request_app ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
        }
    }
    return format_cancelled ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    s_sd_format_confirm=false;
    if (s_select_mode) { s_select_mode = false; clear_selection(); s_batch_confirm = false; return APP_REDRAW_PAGE; }
    if (s_view == FILE_VIEW_MOVE) { leave_move(false); return APP_REDRAW_PAGE; }
    if (key == 1) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    if (s_view == FILE_VIEW_RENAME) { ui_keyboard_end(); s_view = FILE_VIEW_ACTIONS; return APP_REDRAW_PAGE; }
    if (s_view == FILE_VIEW_SEARCH) { ui_keyboard_end(); s_view = FILE_VIEW_LIST; return APP_REDRAW_PAGE; }
    if (s_view == FILE_VIEW_ACTIONS) {
        s_view = FILE_VIEW_LIST; s_delete_confirm = false; rescan_current(); return APP_REDRAW_PAGE;
    }
    if (s_folder >= 0) {
        if (s_folder == 3 && strcmp(s_dir, "/sdcard")) {
            char *slash = strrchr(s_dir, '/'); if (slash) *slash = 0;
            scan_folder(3);
        } else s_folder = -1;
        return APP_REDRAW_PAGE;
    }
    ui_nav_request(ctx, 0);
    return APP_REDRAW_NONE;
}
static bool no_menu_handle(app_ctx_t *ctx) { (void)ctx; return false; }
static bool main_page_visible(app_ctx_t *ctx) {
    (void)ctx;
    return s_view == FILE_VIEW_LIST && s_folder < 0 && !strcmp(s_dir, "/sdcard") && !s_delete_confirm;
}
static bool files_edge_back(app_ctx_t *ctx) { return !main_page_visible(ctx); }
static bool files_back_swipe_area(app_ctx_t *ctx,const ui_gesture_event_t *event) {
    (void)ctx;
    return s_view!=FILE_VIEW_RENAME || event->y0<UI_SYSTEM_KEYBOARD_TOP;
}
static void files_before_lock(app_ctx_t *ctx) { (void)ctx;s_pager_pressed=UI_PAGER_NONE;s_pager_paint_pending=false; }
static void files_exit(app_ctx_t *ctx) { files_before_lock(ctx); ui_keyboard_end(); search_close(); s_query[0] = 0; s_select_mode = false; s_batch_confirm = false; batch_src_clear(); if (s_book_stat_fs) { file_search_close(s_book_stat_fs); s_book_stat_fs = NULL; } }

const app_desc_t app_files = {
    .binary_ui = false,
    .title = "文件管理 Files", .detail = "TF 卡文件与传输", .enter_full = false,
    .owns_keys = true, .menu_handle_enabled = no_menu_handle,
    .main_page_visible = main_page_visible, .edge_back_enabled = files_edge_back,
    .back_swipe_area=files_back_swipe_area,
    .persistent_navigation=true,
    .on_enter = on_enter, .on_media_lost = on_media_lost, .on_media_ready = on_media_ready,
    .on_exit = files_exit, .on_before_lock=files_before_lock, .render = render, .on_tick = on_tick,
    .on_gesture = on_gesture, .on_key = on_key, .present = files_present,
};
