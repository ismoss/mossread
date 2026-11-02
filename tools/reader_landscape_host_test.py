#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：书内横屏的触摸/手势换算与旋转对账状态机。
/ English: In-book landscape touch/gesture remap and the rotation reconcile state machine."""
from pathlib import Path
import subprocess, tempfile
from home_cover_cache_host_test import function
ROOT = Path(__file__).resolve().parents[1]
book = ROOT / 'main/apps/app_book.c'

unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {int x,y,width,height;} EpdRect;
// 面板物理矩阵 1216x684；竖屏逻辑 684x1216，横屏逻辑 1216x684。命名枚举让真实函数
// 里的 "enum EpdRotation" 写法保持完整类型。
// Physical matrix 1216x684; portrait-logical 684x1216, landscape-logical 1216x684. The named
// enum keeps "enum EpdRotation" complete in the extracted bodies.
typedef enum EpdRotation { EPD_ROT_LANDSCAPE, EPD_ROT_PORTRAIT, EPD_ROT_INVERTED_LANDSCAPE,
                           EPD_ROT_INVERTED_PORTRAIT } EpdRotation;
// epdiy 的 width/height 返回物理尺寸；旋转只改逻辑映射。
// epdiy width/height report physical sizes; rotation only changes the logical mapping.
static int epd_width(void){return 1216;}
static int epd_height(void){return 684;}
static EpdRotation s_rotation = EPD_ROT_INVERTED_PORTRAIT;
static EpdRotation epd_get_rotation(void){return s_rotation;}
static void epd_set_rotation(EpdRotation r){s_rotation=r;}
typedef enum { UI_GESTURE_SWIPE_L, UI_GESTURE_SWIPE_R, UI_GESTURE_SWIPE_U,
               UI_GESTURE_SWIPE_D, UI_GESTURE_TAP, UI_GESTURE_CANCEL,
               UI_GESTURE_PRESS, UI_GESTURE_MOVE, UI_GESTURE_LONG_PRESS } ui_gesture_type_t;
'''
unit += r'''
// 对账状态机的替身：记录锚定重排与旋转设置，正文几何用横屏标记推导。
// State-machine stands-ins: record anchored relayouts; viewport follows the layout flag.
static bool s_reader_landscape_layout;
static int s_view, s_reader_panel;
static const char *s_text;
static bool s_clear_confirm;
static int build_calls;
static unsigned landscape_mode;
static int s_orient_desired = EPD_ROT_INVERTED_PORTRAIT;
static bool app_settings_reader_adaptive(void){return false;}
static int reader_w(void){return s_reader_landscape_layout?epd_width():684;}
static int reader_h(void){return s_reader_landscape_layout?epd_height():1216;}
static EpdRect body_rect(void){return (EpdRect){0,0,reader_w(),reader_h()};}
static size_t page_start, page_for;
static size_t s_page;
static size_t s_text_len, s_block_count;
static void *s_blocks;
static int s_px = 48;
static size_t book_layout_page_start_offset(size_t page){(void)page;return page_start;}
static size_t book_layout_page_for_offset(size_t off){(void)off;return page_for;}
static bool book_layout_build_blocks(const char *t,size_t len,void *b,size_t c,EpdRect r,int px){
 (void)t;(void)len;(void)b;(void)c;(void)px;++build_calls;assert(r.width==reader_w());return true;}
static unsigned app_settings_reader_landscape(void){return landscape_mode;}
static void lock_draw(void){} static void unlock_draw(void){}
static void invalidate_prep(void){}
static void free_book(void){assert(false);}
static void copy_text(char *d,size_t n,const char *s){snprintf(d,n,"%s",s);}
static char s_message[64];
enum { READING, SHELF, TOC, READER_PANEL_NONE, READER_PANEL_TOOLS, READER_PANEL_STATS,
       READER_PANEL_CHAPTER_JUMP, READER_PANEL_STATS_RECENT, READER_PANEL_REFRESH_SETTINGS,
       READER_PANEL_TURN_SETTINGS, READER_PANEL_KEY_SETTINGS, READER_PANEL_FONT_SETTINGS,
       READER_PANEL_WEIGHT_SETTINGS, READER_PANEL_LAYOUT_SETTINGS, READER_PANEL_RULE_SETTINGS,
       READER_PANEL_FONT_PICKER, READER_PANEL_BOOKMARKS, READER_PANEL_BACKGROUND,
       READER_PANEL_BACKGROUND_PREVIEW, READER_PANEL_KEY_PICKER, READER_PANEL_FONT_RECOVERY };
'''
for name in ('reader_rot_is_landscape', 'reader_want_landscape',
             'reader_panel_landscape', 'reader_landscape_active', 'reader_rotation_now',
             'reader_relayout_keep_page', 'reader_sync_rotation', 'reader_touch_to_landscape',
             'reader_gesture_to_landscape'):
    unit += function(name, book) + '\n'
unit += r'''
int main(void){
 // 触摸换算：竖屏四角点在两个横屏方向上的落点，以及往返一致性。
 // Touch remap: the four portrait corners land on the expected landscape corners and round-trip.
 s_rotation=EPD_ROT_LANDSCAPE;
 uint16_t x,y;
 x=0;y=0;reader_touch_to_landscape(&x,&y);assert(x==0&&y==683);
 x=683;y=0;reader_touch_to_landscape(&x,&y);assert(x==0&&y==0);
 x=0;y=1215;reader_touch_to_landscape(&x,&y);assert(x==1215&&y==683);
 x=683;y=1215;reader_touch_to_landscape(&x,&y);assert(x==1215&&y==0);
 s_rotation=EPD_ROT_INVERTED_LANDSCAPE;
 x=0;y=0;reader_touch_to_landscape(&x,&y);assert(x==1215&&y==0);
 x=683;y=1215;reader_touch_to_landscape(&x,&y);assert(x==0&&y==683);
 // 竖屏旋转不换算。
 // Portrait rotations never remap.
 s_rotation=EPD_ROT_INVERTED_PORTRAIT;
 x=17;y=900;reader_touch_to_landscape(&x,&y);assert(x==17&&y==900);
 // 手势方向：两个横屏方向各自成对换向，竖屏原样。
 // Gesture directions swap in pairs per landscape arm; portrait passes through.
 s_rotation=EPD_ROT_LANDSCAPE;
 assert(reader_gesture_to_landscape(UI_GESTURE_SWIPE_L)==UI_GESTURE_SWIPE_D);
 assert(reader_gesture_to_landscape(UI_GESTURE_SWIPE_R)==UI_GESTURE_SWIPE_U);
 assert(reader_gesture_to_landscape(UI_GESTURE_SWIPE_U)==UI_GESTURE_SWIPE_L);
 assert(reader_gesture_to_landscape(UI_GESTURE_SWIPE_D)==UI_GESTURE_SWIPE_R);
 assert(reader_gesture_to_landscape(UI_GESTURE_TAP)==UI_GESTURE_TAP);
 s_rotation=EPD_ROT_INVERTED_LANDSCAPE;
 assert(reader_gesture_to_landscape(UI_GESTURE_SWIPE_L)==UI_GESTURE_SWIPE_U);
 assert(reader_gesture_to_landscape(UI_GESTURE_SWIPE_R)==UI_GESTURE_SWIPE_D);
 assert(reader_gesture_to_landscape(UI_GESTURE_SWIPE_U)==UI_GESTURE_SWIPE_R);
 assert(reader_gesture_to_landscape(UI_GESTURE_SWIPE_D)==UI_GESTURE_SWIPE_L);
 s_rotation=EPD_ROT_INVERTED_PORTRAIT;
 assert(reader_gesture_to_landscape(UI_GESTURE_SWIPE_L)==UI_GESTURE_SWIPE_L);
 // 对账状态机：开横屏锚定重排并切旋转；横屏面板保持同向不重排，恢复面板回落竖屏；
 // 快切方向只换旋转。
 // Reconcile: enabling re-anchors and swaps; landscape panels keep the orientation without a
 // relayout, the recovery sheet falls back to portrait; switching direction only swaps rotation.
 s_view=READING;s_text="t";s_reader_panel=READER_PANEL_NONE;s_clear_confirm=false;
 landscape_mode=0;page_start=700;page_for=9;
 reader_sync_rotation();
 assert(s_rotation==EPD_ROT_INVERTED_PORTRAIT&&!s_reader_landscape_layout&&build_calls==0);
 landscape_mode=1;
 reader_sync_rotation();
 assert(build_calls==1&&s_reader_landscape_layout&&
        s_rotation==EPD_ROT_INVERTED_LANDSCAPE);
 // 统计面板与正文同向：打开不再回落竖屏，也不触发重排。
 // The stats sheet shares the body orientation: opening it neither falls back nor re-anchors.
 s_reader_panel=READER_PANEL_STATS;
 reader_sync_rotation();
 assert(build_calls==1&&s_reader_landscape_layout&&
        s_rotation==EPD_ROT_INVERTED_LANDSCAPE);
 s_reader_panel=READER_PANEL_FONT_RECOVERY;
 reader_sync_rotation();
 assert(build_calls==2&&!s_reader_landscape_layout&&
        s_rotation==EPD_ROT_INVERTED_PORTRAIT);
 s_reader_panel=READER_PANEL_TOOLS;
 reader_sync_rotation();
 assert(build_calls==3&&s_reader_landscape_layout&&
        s_rotation==EPD_ROT_INVERTED_LANDSCAPE);
 landscape_mode=2;
 reader_sync_rotation();
 assert(build_calls==3&&s_rotation==EPD_ROT_LANDSCAPE);
 s_rotation=EPD_ROT_INVERTED_PORTRAIT;  // 模拟全局浮层借走竖屏后回到正文。
                                        // A global overlay borrowed portrait; body repaints.
 reader_sync_rotation();
 assert(s_rotation==EPD_ROT_LANDSCAPE&&build_calls==3);
 // 无书或非正文视图永不全横屏。/ Bookless or non-body views never go landscape.
 s_text=NULL;landscape_mode=1;
 reader_sync_rotation();
 assert(!s_reader_landscape_layout&&s_rotation==EPD_ROT_INVERTED_PORTRAIT);
 puts("PASS: landscape touch corners, gesture pairs, anchored reconcile, panel orientation keep/fallback and direction swap");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp) / 'test.c'
    p.write_text(unit)
    exe = Path(tmp) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(p), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
