#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 中文：实际锁屏入口按当前样式绘图；自动/手动、深睡与密码缓存回退不改选择。
# English: Actual sleep entries honor the selected artwork for auto/manual locks, deep sleep and PIN cache fallback.
from pathlib import Path
import subprocess
import tempfile
from home_cover_cache_host_test import function
ROOT = Path(__file__).resolve().parents[1]
unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>
#define UI_LOCK_WIDTH 684
#define UI_LOCK_HEIGHT 1216
#define FRAME_BYTES (684u*1216u/2u)
#define MODE_GC16 1
enum EpdDrawError {EPD_DRAW_SUCCESS=0,EPD_DRAW_FAULT=1};
#define APP_WAKE_KEY 1
#define APP_WAKE_TIMEOUT 3
#define APP_SLEEP_DEEP 1
#define APP_LOCK_LIGHT_SLEEP_MS 600000u
#define APP_LOCK_IGNORE_BOOT_MS 1000
#define ESP_ERR_NOT_FINISHED 1
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
typedef struct {int x,y,width,height;} EpdRect;
typedef struct {uint8_t back_fb[FRAME_BYTES];} EpdiyHighlevelState;
typedef struct {bool mounted;} read_pico_sd_info_t;
typedef struct {bool active;} app_lock_font_t;
typedef struct {bool time_synced;int64_t unix_sec;} pmu_snapshot_t;
typedef void *sc7a20h_handle_t;typedef void *cst836u_handle_t;
typedef int app_wake_source_t;
enum EpdRotation {EPD_ROT_LANDSCAPE,EPD_ROT_PORTRAIT,EPD_ROT_INVERTED_LANDSCAPE,EPD_ROT_INVERTED_PORTRAIT};
static uint8_t pixels[FRAME_BYTES],panel[FRAME_BYTES],style,current_background=0x32;
static bool frame_ready, s_lock_feedback_visible, feedback_seen, final_ok=true;
static unsigned full_presents, entry_clears;
static unsigned draws[4],fallback,presents,auths,font_begin,font_end,pause,resume,clear_resume,ready,held,sleep_waits;
static bool pin,draw_ok=true,transfer_ok=true,timeout,reader,boot_cache,cache_ok,boot_key;
static bool boot_resume,boot_in_reader,draw_seen;
static uint8_t reader_landscape;
static enum EpdRotation rotation_state,last_draw_rotation;
static unsigned cached_style;
static jmp_buf deep;
typedef struct {uint32_t serial;} lock_hint_t;
static lock_hint_t s_lock_hint;
static EpdiyHighlevelState *s_lock_hint_hl,*s_boot_lock_hl;
static bool s_boot_lock_active,s_boot_lock_known,s_boot_lock_draw;
static const char *app_settings_wallpaper_path(void){return "/sdcard/wallpaper.jpg";}
static const char *app_settings_wallpaper_landscape_path(void){return "";}
static uint8_t app_settings_lock_style(void){return style;}
static bool lock_pin_enabled(void){return pin;}
static bool draw(unsigned chosen,uint8_t *fb){assert(chosen==style&&panel[0]==(feedback_seen?0xff:current_background));if(chosen==3)assert(fb[0]==(boot_cache?0xff:current_background));draw_seen=true;last_draw_rotation=rotation_state;++draws[chosen];memset(fb,0x10+chosen,FRAME_BYTES);frame_ready=draw_ok;return draw_ok;}
static bool ui_wallpaper_prepare_lock(uint8_t *fb,const char *path){assert(!strcmp(path,app_settings_wallpaper_path()));return draw(1,fb);}
static void ui_wallpaper_cache_commit(const uint8_t *fb){assert(style==1&&draw_ok&&presents==1&&fb==pixels&&!memcmp(panel,pixels,FRAME_BYTES));}
static bool book_ticket_draw(uint8_t *fb,bool background){assert(background==reader);return draw(0,fb);}
static const char *app_settings_bookmark_path(void){return "/sdcard/overlay.png";}
static bool book_lock_bookmark_draw(uint8_t *fb,const char *p){assert(!strcmp(p,app_settings_bookmark_path()));return draw(3,fb);}
static bool lock_screen_save(const uint8_t *fb){assert(fb==pixels&&presents);return true;}
static int pico_boot_mark_frame_cached(void){return 0;}
static bool book_lock_collage_draw(uint8_t *fb){return draw(2,fb);}
static bool book_lock_collage_prepare(uint8_t *fb){return draw(2,fb);}
static bool draw_lock_clock(uint8_t *fb,bool landscape){(void)landscape;memset(fb,0x15,FRAME_BYTES);return true;}
static enum EpdDrawError update_display_full(EpdiyHighlevelState *h){(void)h;++full_presents;assert(!s_lock_feedback_visible);if(feedback_seen)assert(panel[0]==255&&panel[1]==0);if(!final_ok){panel[0]=0x77;return EPD_DRAW_FAULT;}memcpy(panel,pixels,FRAME_BYTES);++presents;return EPD_DRAW_SUCCESS;}
static void guard_draw_result(EpdiyHighlevelState *h,enum EpdDrawError result){(void)h;(void)result;}
static void book_lock_collage_cache_commit(const uint8_t *fb){assert(style==2&&draw_ok&&presents==1&&fb==pixels&&!memcmp(panel,pixels,FRAME_BYTES));}
static void ui_lock_fallback_draw(uint8_t *fb){assert(panel[0]==(feedback_seen?255:0x32));++fallback;memset(fb,0xfe,FRAME_BYTES);frame_ready=true;}
static uint8_t *epd_hl_get_framebuffer(EpdiyHighlevelState *h){memset(h->back_fb,current_background,FRAME_BYTES);return pixels;}
static void epd_poweron(void){}
static void epd_poweroff(void){}
static void epd_clear(void){if(!presents){assert(frame_ready);++entry_clears;}memset(panel,255,FRAME_BYTES);}
static void epd_hl_set_all_white(void *h){(void)h;memset(pixels,255,FRAME_BYTES);}
static enum EpdDrawError epd_hl_update_screen_from_white(void *h,int mode,int temp){(void)h;assert(mode==MODE_GC16&&temp==25&&frame_ready&&panel[0]==255);assert(pixels[0]==(draw_ok?0x10+style:0xfe)||(style==4&&pixels[0]==255));memcpy(panel,pixels,FRAME_BYTES);++presents;return EPD_DRAW_SUCCESS;}
static void app_font_begin_lock(app_lock_font_t *s){assert(panel[0]==(feedback_seen?0xff:0x32));s->active=true;++font_begin;}
static void app_font_end_lock(app_lock_font_t *s){assert(s->active);++font_end;}
static bool read_pico_transfer_pause_for_sleep(void){++pause;return transfer_ok;}
static void read_pico_transfer_resume_after_sleep(void){++resume;}
static void pico_boot_clear_resume(void){++clear_resume;}
static bool pico_boot_has_resume(void){return boot_resume;}
static bool pico_boot_resume_in_reader(void){return boot_in_reader;}
static void pico_boot_ready(void){++ready;}
static void pico_boot_hold_resume(void){++held;}
static void app_lock_wait_key_idle(int ms){assert(ms==800);}
static app_wake_source_t app_light_sleep_wait_timed(void *acc,unsigned ms){assert((acc==NULL||acc==(void*)1)&&(ms==0||ms==600000));++sleep_waits;if(ms==0)return APP_WAKE_KEY;return timeout?APP_WAKE_TIMEOUT:APP_WAKE_KEY;}
static void app_enter_host_sleep(int mode){assert(mode==APP_SLEEP_DEEP);longjmp(deep,1);}
static void read_pico_pmu_drain_events(void){}
static uint32_t read_pico_pmu_lock_key_serial(void){return 0;}
static const pmu_snapshot_t *read_pico_pmu_get(void){return NULL;}
static void app_settings_record_pin_lock(uint32_t now){(void)now;}
static bool lock_pin_grace_active(void){return false;}
static void auto_connect_on_wake(void){}
static int64_t esp_timer_get_time(void){return 2000000;}
static bool lock_screen_restore(uint8_t *fb){if(!cache_ok||cached_style!=style)return false;memset(fb,0x10+style,FRAME_BYTES);return true;}
static void lock_screen_authenticate(void *h,void *tp,void *acc,bool boot,bool boot_key_unlock,bool boot_face_settled){(void)h;(void)boot_face_settled;assert(tp==(void*)2&&acc==(void*)1);assert(pin);assert(boot==boot_cache&&boot_key_unlock==boot_key);assert(pixels[0]==(draw_ok?0x10+style:0xfe)||(style==4&&pixels[0]==255));++auths;}
typedef int esp_err_t;
static esp_err_t read_pico_sd_get_info(read_pico_sd_info_t *i){i->mounted=true;return ESP_OK;}
static void vTaskDelay(int ms){(void)ms;}
static int epd_width(void){return 684;}
static int epd_height(void){return 1216;}
static int epd_rotated_display_width(void){return rotation_state==EPD_ROT_LANDSCAPE||rotation_state==EPD_ROT_INVERTED_LANDSCAPE?1216:684;}
static int epd_rotated_display_height(void){return rotation_state==EPD_ROT_LANDSCAPE||rotation_state==EPD_ROT_INVERTED_LANDSCAPE?684:1216;}
static uint8_t app_settings_reader_landscape(void){return reader_landscape;}
static void lock_resume_service(void){}
// 锁屏入口现在会读当前旋转（横版阅读票根不再先回竖屏），测试固定在竖屏。
// The lock entry now reads the current rotation (the landscape reading ticket no longer returns to
// portrait first); this test stays in portrait.
static enum EpdRotation epd_get_rotation(void){return rotation_state;}
static void epd_set_rotation(enum EpdRotation rotation){rotation_state=rotation;}
'''
for name in ('lock_wallpaper_path', 'boot_lock_landscape', 'boot_lock_rotation',
             'draw_wallpaper', 'enter_lock_and_sleep', 'app_lock_boot_gate'):
    unit += function(name, ROOT/'main/sleep.c') + '\n'
unit += r'''
static void reset(void){current_background=0x32;memset(draws,0,sizeof(draws));fallback=presents=auths=font_begin=font_end=pause=resume=clear_resume=ready=held=sleep_waits=0;memset(pixels,0x32,FRAME_BYTES);memset(panel,0x32,FRAME_BYTES);frame_ready=s_lock_feedback_visible=feedback_seen=draw_seen=false;full_presents=entry_clears=0;final_ok=true;draw_ok=transfer_ok=true;timeout=reader=boot_cache=cache_ok=boot_resume=boot_in_reader=boot_key=false;reader_landscape=0;rotation_state=EPD_ROT_INVERTED_PORTRAIT;last_draw_rotation=rotation_state;}
int main(void){
 EpdiyHighlevelState hl={0};int64_t ignore=0;
 // 两种来源的阅读背景只影响票根内容，不决定样式。/ Reader provenance affects ticket content, never the style.
 for(unsigned s=0;s<4;++s)for(unsigned password=0;password<2;++password)for(unsigned body=0;body<2;++body){
  reset();style=s;pin=password;reader=body;
  enter_lock_and_sleep(&hl,&ignore,(void*)1,(void*)2,reader);
  assert(draws[s]==1&&draws[(s+1)%3]==0&&draws[(s+2)%3]==0&&!fallback&&presents==1);
  assert(auths==password&&pause==1&&resume==1&&clear_resume==1&&ignore==3000);
  assert(font_begin==(s==0||s==2||password)&&font_end==font_begin&&app_settings_lock_style()==s);
 }
 // 横屏正文锁屏：四种可横版样式在绘制期间保持横屏，解锁/验证结束后交还竖屏；
 // 样式4关闭锁面，必须直接回竖屏。
 // Landscape reader lock: all four landscape-capable styles draw on the landscape canvas and
 // hand portrait back after unlock; style 4 disables the face and returns portrait immediately.
 for(unsigned s=0;s<4;++s){
  reset();style=s;pin=false;reader=true;reader_landscape=1;rotation_state=EPD_ROT_LANDSCAPE;
  enter_lock_and_sleep(&hl,&ignore,(void*)1,(void*)2,true);
  assert(draw_seen&&last_draw_rotation==EPD_ROT_LANDSCAPE&&rotation_state==EPD_ROT_INVERTED_PORTRAIT);
 }
 reset();style=4;pin=false;frame_ready=true;rotation_state=EPD_ROT_LANDSCAPE;
 enter_lock_and_sleep(&hl,&ignore,(void*)1,(void*)2,false);
 assert(!draw_seen&&rotation_state==EPD_ROT_INVERTED_PORTRAIT);
 // 非倒置竖屏也不能被宽泛的“非竖屏即横屏”判断误当成横版入口。
 // A non-inverted portrait state must not be treated as landscape by a broad "not portrait" check.
 reset();style=0;pin=false;frame_ready=true;rotation_state=EPD_ROT_PORTRAIT;
 enter_lock_and_sleep(&hl,&ignore,(void*)1,(void*)2,false);
 assert(draw_seen&&last_draw_rotation==EPD_ROT_INVERTED_PORTRAIT&&rotation_state==EPD_ROT_INVERTED_PORTRAIT);
 // 首页/书架/设置/阅读都保留当前帧；没有全屏阅读重排或历史书籍依赖。
 // Home/shelf/settings/reader all preserve their actual frame without fullscreen reflow or book history.
 for(unsigned screen=0;screen<4;++screen){reset();style=3;pin=false;current_background=0x21+screen*17;memset(pixels,current_background,FRAME_BYTES);memset(panel,current_background,FRAME_BYTES);enter_lock_and_sleep(&hl,&ignore,(void*)1,(void*)2,false);assert(draws[3]==1&&presents==1&&!fallback);}
 // 白底圆锁历史一次GC16完成后再缓存/PIN，失败恢复原清屏，自动/其他样式不走提前反馈。/ Present once from true badge history before cache/PIN; faults clear and idle/styles keep the original path.
 for(unsigned success=0;success<2;++success)for(unsigned password=0;password<2;++password){reset();style=2;pin=password;final_ok=success;s_lock_feedback_visible=feedback_seen=true;memset(panel,255,FRAME_BYTES);panel[1]=0;enter_lock_and_sleep(&hl,NULL,(void*)1,(void*)2,false);assert(full_presents==1&&presents==1&&entry_clears==!success&&auths==password&&!s_lock_feedback_visible);}
 reset();style=2;pin=false;transfer_ok=false;s_lock_feedback_visible=feedback_seen=true;enter_lock_and_sleep(&hl,NULL,(void*)1,(void*)2,false);assert(!full_presents&&!s_lock_feedback_visible&&!presents);
 // 失败只用内建回退，绝不改成其他样式；传输忙时不绘锁屏。/ Draw failure falls back only to built-in art; active uploads defer lock.
 for(unsigned s=0;s<4;++s){reset();style=s;pin=false;draw_ok=false;enter_lock_and_sleep(&hl,NULL,(void*)1,(void*)2,false);assert(draws[s]==1&&fallback==1&&style==s);}
 reset();transfer_ok=false;enter_lock_and_sleep(&hl,NULL,(void*)1,(void*)2,false);assert(!presents&&!auths&&!font_begin&&clear_resume==1&&!resume);
 // 超时深睡保留已选择的实际画面，不重绘拼贴。/ Deep timeout retains selected art without redrawing a collage.
 for(unsigned s=0;s<4;++s){reset();style=s;pin=false;timeout=true;if(!setjmp(deep)){enter_lock_and_sleep(&hl,NULL,(void*)1,(void*)2,false);assert(0);}assert(presents==1&&draws[s]==1&&style==s&&!resume);}
 // 密码冷启动：同样式缓存才命中，跨样式/损坏时重画当前选择。/ Cold PIN gate redraws the current choice for stale/corrupt cache.
 for(unsigned s=0;s<4;++s)for(unsigned previous=0;previous<4;++previous){
  reset();style=s;pin=boot_cache=cache_ok=true;cached_style=previous;
  app_lock_boot_gate(&hl,(void*)2,(void*)1,false);
  assert(auths==1&&font_begin==1&&font_end==1&&presents==1&&full_presents==1&&!fallback&&style==s);
  assert(draws[s]==(previous!=s)&&draws[(s+1)%3]==0&&draws[(s+2)%3]==0);
 }
 for(unsigned s=0;s<4;++s){reset();style=s;pin=boot_cache=true;draw_ok=false;app_lock_boot_gate(&hl,(void*)2,(void*)1,false);assert(auths==1&&draws[s]==1&&fallback==1&&style==s);}
 // 开机键已带来解锁证据：锁面只在内存里重建，不能先整屏闪一次再弹密码。
 // A boot key already carries unlock evidence: rebuild the face in memory without flashing it
 // before the PIN screen.
 reset();style=3;pin=boot_cache=true;cache_ok=false;boot_resume=true;boot_key=true;
 app_lock_boot_gate(&hl,(void*)2,(void*)1,true);
 assert(auths==1&&draws[3]==1&&!full_presents&&!presents);
 // 深睡恢复进横屏书：缓存校验/重画前先切到横屏，验证结束后交还竖屏；样式4保持竖屏。
 // Deep-sleep resume into a landscape book: switch to landscape before cache validation/redraw,
 // then hand portrait back after authentication; style 4 stays portrait throughout.
 for(unsigned s=0;s<4;++s){
  reset();style=s;pin=boot_cache=true;cache_ok=false;boot_resume=boot_in_reader=true;reader_landscape=1;
  app_lock_boot_gate(&hl,(void*)2,(void*)1,false);
  assert(draw_seen&&last_draw_rotation==EPD_ROT_INVERTED_LANDSCAPE&&rotation_state==EPD_ROT_INVERTED_PORTRAIT);
  assert(auths==1&&style==s);
 }
 reset();style=4;pin=boot_cache=true;cache_ok=false;boot_resume=boot_in_reader=true;reader_landscape=1;
 app_lock_boot_gate(&hl,(void*)2,(void*)1,false);
 assert(!draw_seen&&rotation_state==EPD_ROT_INVERTED_PORTRAIT&&auths==1&&style==4);
 // 无密码双击启动未完成双击时留在锁面等待，完成后才续读并重新持有检查点。
 // A no-PIN double-press boot without completed evidence waits on the lock face, then resumes
 // and re-holds the checkpoint.
 reset();style=3;pin=false;boot_resume=true;boot_cache=cache_ok=true;cached_style=3;
 app_lock_boot_gate(&hl,(void*)2,(void*)1,false);
 assert(!auths&&font_begin==1&&font_end==1&&ready==1&&held==1&&presents==1);
 // 已经完成双击证据时直接续读，不能被旧启动门再困进一次无限浅睡。
 // With completed double-press evidence, resume directly instead of entering the old unbounded
 // lock wait once more.
 reset();style=3;pin=false;boot_resume=true;boot_cache=cache_ok=true;cached_style=3;
 app_lock_boot_gate(&hl,(void*)2,(void*)1,true);
 assert(!auths&&!font_begin&&ready==1&&held==1&&!sleep_waits);
 reset();pin=false;app_lock_boot_gate(&hl,(void*)2,(void*)1,false);assert(!auths&&!font_begin);
 puts("PASS: real single-pass final collage retains white/badge history, avoids duplicate entry clear, recovers final faults and consumes feedback before cache/PIN; auto/manual lock entry chooses ticket/wallpaper/collage with/without PIN and reader background; upload deferral; deep timeout; cold gate cache/fault fallback keeps selected style");
}
'''
with tempfile.TemporaryDirectory(dir=ROOT/'build') as folder:
    c, exe = Path(folder)/'test.c', Path(folder)/'test'
    c.write_text(unit)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
