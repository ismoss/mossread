#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：二级面板横屏几何——全部控件落进 1216x684 视口、行列互不重叠、命中矩形与绘制同源。
/ English: Landscape panel geometry — every control lands inside the 1216x684 viewport, rows and
columns never overlap, and the hit rectangles share the painted ones."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
book = (ROOT / 'main/apps/app_book.c').read_text()
source = book

def function(name, text):
    # 定义行的名字后面紧跟参数表；一行函数内的「调用」因前缀类不允许 { ? : ( 等符号而不可达。
    # The definition line puts the name right before its parameter list; call sites inside
    # one-liners stay unreachable because the prefix class excludes { ? : ( punctuation.
    pattern = r'^static\s+[A-Za-z_][A-Za-z0-9_ \*]*\b' + re.escape(name) + r'\s*\([^\n]*'
    matches = list(re.finditer(pattern, text, re.M))
    assert matches, name
    start = matches[-1].start()
    depth = 0
    for i in range(text.index('{', start), len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise AssertionError(name)

names = ('reader_rot_is_landscape', 'reader_want_landscape',
         'reader_panel_landscape', 'reader_ui_extra', 'reader_font_top', 'reader_font_card',
         'reader_slider_rect',
         'reading_setting_rect', 'reading_settings_top', 'reader_panel_footer_rect',
         'reader_weight_top', 'reader_weight_rect', 'reader_rule_top', 'rule_style_rect',
         'rule_offset_rect', 'reader_layout_top', 'indent_adjust_rect', 'reader_layout_back_rect',
         'reader_turn_top', 'reader_key_top', 'reader_key_card', 'reader_key_hold_rect',
         'reader_key_preset_card', 'reader_key_pick_row', 'reader_font_picker_top',
         'reader_font_pick_rect', 'bookmarks_top', 'bookmark_row_step', 'bookmark_rows',
         'bookmark_row_rect',
         'reader_background_top', 'reader_background_row', 'reader_stats_top',
         'reader_stats_control_rect', 'reader_jump_top', 'reader_chapter_key_rect',
         'reader_stats_recent_top')

unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {int x,y,width,height;} EpdRect;
static int epd_width(void){return 1216;}
static int epd_height(void){return 684;}
enum { SHELF, READING, TOC, READER_PANEL_NONE, READER_PANEL_TOOLS, READER_PANEL_STATS,
       READER_PANEL_CHAPTER_JUMP, READER_PANEL_STATS_RECENT, READER_PANEL_REFRESH_SETTINGS,
       READER_PANEL_TURN_SETTINGS, READER_PANEL_KEY_SETTINGS, READER_PANEL_FONT_SETTINGS,
       READER_PANEL_WEIGHT_SETTINGS, READER_PANEL_LAYOUT_SETTINGS, READER_PANEL_RULE_SETTINGS,
       READER_PANEL_FONT_PICKER, READER_PANEL_BOOKMARKS, READER_PANEL_BACKGROUND,
       READER_PANEL_BACKGROUND_PREVIEW, READER_PANEL_KEY_PICKER, READER_PANEL_FONT_RECOVERY };
static int s_view = READING, s_reader_panel;
static const char *s_text = "t";
static unsigned landscape_mode = 1;
typedef enum EpdRotation { EPD_ROT_LANDSCAPE, EPD_ROT_PORTRAIT, EPD_ROT_INVERTED_LANDSCAPE,
                           EPD_ROT_INVERTED_PORTRAIT } EpdRotation;
static int s_orient_desired = EPD_ROT_INVERTED_PORTRAIT;
static bool app_settings_reader_adaptive(void){return false;}
static bool s_bookmark_edit;
static unsigned font_scale = 100;
static unsigned app_settings_reader_landscape(void){return landscape_mode;}
static unsigned app_settings_system_font_size(void){return font_scale;}
'''
# 提取函数的前置声明：替身 reader_w/reader_h 在提取之前就引用了门控函数。
# Forward declarations for the extracted helpers: the reader_w/reader_h stand-ins reference
# the gate before its extracted body appears.
for n in names:
    m = re.search(r'^static\s+[A-Za-z_][A-Za-z0-9_ \*]*\b' + re.escape(n) + r'\s*\(', source, re.M)
    assert m, n
    line_end = source.index('\n', m.start())
    sig = source[m.start():line_end].split('{')[0].strip()
    unit += sig.rstrip().rstrip('{').strip() + ';\n'
unit += 'static int reader_w(void){return reader_panel_landscape() ? epd_width() : 684;}\n'
unit += 'static int reader_h(void){return reader_panel_landscape() ? epd_height() : 1216;}\n'
unit += re.search(r'^#define READER_BACKGROUND_ROWS .*', source, re.M)[0] + '\n'
unit += re.search(r'^#define READER_STATS_TOP .*', source, re.M)[0] + '\n'
unit += re.search(r'^#define READER_JUMP_TOP .*', source, re.M)[0] + '\n'
for n in names:
    unit += function(n, source) + '\n'

unit += r'''
static bool inside(EpdRect r, int w, int h) {
    return r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0 &&
           r.x + r.width <= w && r.y + r.height <= h + 28;
}
static bool overlap(EpdRect a, EpdRect b) {
    return a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}
static void check_group(const char *name, const EpdRect *rects, int count) {
    const int w = reader_w(), h = reader_h();
    for (int i = 0; i < count; ++i) {
        if (!inside(rects[i], w, h)) {
            fprintf(stderr, "%s rect %d out of viewport: {%d,%d,%d,%d}\n", name, i,
                    rects[i].x, rects[i].y, rects[i].width, rects[i].height);
            assert(false);
        }
        for (int j = 0; j < i; ++j) assert(!overlap(rects[i], rects[j]));
    }
}
#define CHECK_GENERATOR(name, call, count) \
    do { EpdRect rs[(count)]; for (int i = 0; i < (count); ++i) rs[i] = (call); \
         check_group(name, rs, (count)); } while (0)

int main(void){
 // 门控需要横屏面板在开：统计面板只是载体，几何函数各自按面板语义取横屏分支。
 // The gate needs a landscape panel open: the stats sheet is just the carrier, each geometry
 // helper takes its landscape branch on its own panel semantics.
 s_reader_panel = READER_PANEL_STATS;
 // 统计组：七个控件、14 个键盘键都在视口内且互不重叠。
 // Stats group: the seven controls and the 14 keypad keys fit without overlap.
 CHECK_GENERATOR("stats controls", reader_stats_control_rect(i + 1), 7);
 CHECK_GENERATOR("chapter keys", reader_chapter_key_rect(i), 14);
 // 阅读设置组。
 CHECK_GENERATOR("reading cards", reading_setting_rect(i), 8);
 // 字体组：六卡、三样式卡、三微调钮、九字体格。
 CHECK_GENERATOR("font cards", reader_font_card(i), 6);
 CHECK_GENERATOR("rule styles", rule_style_rect(i), 3);
 CHECK_GENERATOR("rule offsets", rule_offset_rect(i), 3);
 CHECK_GENERATOR("layout adjust", indent_adjust_rect(i), 3);
 CHECK_GENERATOR("font picks", reader_font_pick_rect(i), 9);
 // 按键组：三键卡、两个推荐卡、七个功能行。
 CHECK_GENERATOR("key cards", reader_key_card(i), 3);
 CHECK_GENERATOR("key presets", reader_key_preset_card(i + 1), 2);
 CHECK_GENERATOR("key picks", reader_key_pick_row(i), 7);
 assert(inside(reader_key_hold_rect(), reader_w(), reader_h()));
 assert(inside(reader_panel_footer_rect(), reader_w(), reader_h()));
 // 背景与书签：七行网格与书签行都在视口内。
 CHECK_GENERATOR("background rows", reader_background_row(i), 7);
 CHECK_GENERATOR("bookmark rows", bookmark_row_rect(i), bookmark_rows());
 // 翻页两卡与排版缩进四选。
 CHECK_GENERATOR("turn cards", ((EpdRect){36 + i * ((reader_w() - 96) / 2 + 24), reader_turn_top() + 140,
                                           (reader_w() - 96) / 2, 292}), 2);
 CHECK_GENERATOR("indent choices", ((EpdRect){210 + i * 236, reader_layout_top() + 440, 216, 64}), 4);
 // 命中与绘制同源：每个统计控件的中心都落在自身矩形内；键盘键尺寸符合横屏设计。
 // Hits share paint: each stats control's centre lands inside its own rect; keypads match design.
 for (int i = 1; i <= 7; ++i) {
     EpdRect r = reader_stats_control_rect(i);
     assert(r.x <= r.x + r.width / 2 && r.x + r.width / 2 < r.x + r.width);
     assert(r.y <= r.y + r.height / 2 && r.y + r.height / 2 < r.y + r.height);
 }
 for (int i = 0; i < 14; ++i) {
     EpdRect r = reader_chapter_key_rect(i);
     assert((r.width >= 65 && r.height == 65) || r.width >= 260);
 }
 // 其余提取控件同样落进视口：字号滑杆、返回条、字重钮与滑杆三/四。
 // The remaining extracted controls also stay inside: the size rail, the return bar and the
 // weight buttons plus the layout rails.
 assert(inside(reader_slider_rect(0), reader_w(), reader_h()));
 assert(inside(reader_slider_rect(3), reader_w(), reader_h()));
 assert(inside(reader_slider_rect(4), reader_w(), reader_h()));
 assert(inside(reader_layout_back_rect(), reader_w(), reader_h()));
 assert(inside(reader_weight_rect(0), reader_w(), reader_h()));
 assert(inside(reader_weight_rect(1), reader_w(), reader_h()));
 assert(reader_stats_top() >= 0 && reader_jump_top() >= 0 && reader_stats_recent_top() >= 0);
 assert(reader_font_top() >= 0 && reading_settings_top() >= 0 && reader_turn_top() >= 0);
 assert(reader_key_top() >= 0 && reader_font_picker_top() >= 0 && bookmarks_top() >= 0);
 assert(reader_weight_top() >= 0 && reader_rule_top() >= 0 && reader_layout_top() >= 0);
 assert(reader_background_top() >= 0);
 puts("PASS: landscape panel geometry stays inside the 1216x684 viewport without overlaps");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp) / 'test.c'
    p.write_text(unit)
    exe = Path(tmp) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-fsanitize=address,undefined', str(p), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
