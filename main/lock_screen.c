/* SPDX-License-Identifier: Apache-2.0
 * 中文：独立锁屏输入循环；禁止触摸键、蓝牙、拿起或冷启动绕过密码。
 * English: Isolated locked-input loop; touch keys, BLE, pickup and cold starts cannot bypass authentication.
 * 用户修订：读取失败或确认未开启时直接返回；正常读取的已有密码必须摘要验证，模糊缓存与局部输入保持。
 * User revision: return for read failure or confirmed disabled state; require digest verification for readable enabled PINs, retaining cached frost and local input.
 */
#include "lock_screen.h"
#include "lock_pin.h"
#include "settings.h"
#include "ui_pinpad.h"
#include "ui_kit.h"
#include "ui_lock_feedback.h"
#include "sleep.h"
#include "boot_state.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "read_pico_pmu.h"
#include "ble_page_turner.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define FRAME_BYTES (684u*1216u/2u)
#ifndef LOCK_SCREEN_CACHE_PARENT
#define LOCK_SCREEN_CACHE_PARENT "/sdcard/.readpico"
#endif
#define LOCK_ART_PATH LOCK_SCREEN_CACHE_PARENT "/locks/pin-background.bin"
// 旧缓存可能保存了临时圆锁；透明书封的合成/边界算法也已变化，代号必须再次作废旧图。
// 壁纸锁屏改为"按画布朝向旋转"后，旧的裁切脸图也要作废。
// Old caches may contain temporary badges; the transparent-cover compositing/bounds changed too,
// so this generation must invalidate the older artwork again. The wallpaper lock now rotates to the
// canvas orientation, so the old centre-cropped face frames must go too.
#define ART_MAGIC UINT32_C(0x50414437)
typedef struct {uint32_t magic,rotation,style,checksum;uint64_t wallpaper;} art_header_t;
static uint32_t checksum(const uint8_t *bytes){uint32_t h=2166136261u;for(unsigned i=0;i<FRAME_BYTES;++i)h=(h^bytes[i])*16777619u;return h;}
static uint64_t wallpaper_key(void){
    if(app_settings_lock_style()!=1 && app_settings_lock_style()!=3)return 0;
    // 壁纸缓存键必须跟着实际用的那张图：横屏且设了横屏壁纸时用横屏那张。
    // The art cache key follows the image actually used: on the landscape canvas with a dedicated
    // landscape image set, that one.
    const char *path=app_settings_lock_style()==3?app_settings_bookmark_path():app_settings_wallpaper_path();
    const char *land=app_settings_wallpaper_landscape_path();
    if(app_settings_lock_style()==1&&land[0]&&epd_rotated_display_width()>epd_rotated_display_height())path=land;
    uint64_t key=UINT64_C(14695981039346656037);
    for(const unsigned char *p=(const unsigned char*)path;*p;++p)key=(key^*p)*UINT64_C(1099511628211);
    struct stat st;
    if(stat(path,&st)==0){
        uint64_t info[2]={(uint64_t)st.st_size,(uint64_t)st.st_mtime};
        const unsigned char *bytes=(const unsigned char*)info;
        for(unsigned i=0;i<sizeof(info);++i)key=(key^bytes[i])*UINT64_C(1099511628211);
    }
    return key;
}
bool lock_screen_restore(uint8_t *frame){
    if(!frame)return false;
    FILE *f=fopen(LOCK_ART_PATH,"rb");if(!f)return false;art_header_t h;
    // 跨样式/换壁纸后先拒绝旧图，再由锁屏入口绘制当前选择；不多分配一帧。
    // Reject old art before reading pixels after a style/wallpaper change; the lock entry draws the current choice without another frame.
    bool ok=fread(&h,1,sizeof(h),f)==sizeof(h)&&h.magic==ART_MAGIC&&h.rotation==(uint32_t)epd_get_rotation()&&
        h.style==app_settings_lock_style()&&h.wallpaper==wallpaper_key()&&
        fread(frame,1,FRAME_BYTES,f)==FRAME_BYTES&&fgetc(f)==EOF&&h.checksum==checksum(frame);
    fclose(f);return ok;
}
bool lock_screen_save(const uint8_t *frame){
    (void)mkdir(LOCK_SCREEN_CACHE_PARENT,0775);(void)mkdir(LOCK_SCREEN_CACHE_PARENT "/locks",0775);
    FILE *f=fopen(LOCK_ART_PATH ".tmp","wb");if(!f)return false;
    art_header_t h={.magic=ART_MAGIC,.rotation=(uint32_t)epd_get_rotation(),
        .style=app_settings_lock_style(),.checksum=checksum(frame),.wallpaper=wallpaper_key()};
    bool ok=fwrite(&h,1,sizeof(h),f)==sizeof(h)&&fwrite(frame,1,FRAME_BYTES,f)==FRAME_BYTES&&fflush(f)==0;
    if(fclose(f))ok=false;
    if(ok) {
        // FatFS不支持覆盖rename；旧图保留为备份，失败时回滚，不把失败保存标记为可恢复。
        // FatFS cannot replace through rename; keep old art in a backup and roll back failures without marking them cached.
        if(rename(LOCK_ART_PATH ".tmp",LOCK_ART_PATH)==0)return true;
        (void)remove(LOCK_ART_PATH ".bak");
        if(rename(LOCK_ART_PATH,LOCK_ART_PATH ".bak")==0) {
            if(rename(LOCK_ART_PATH ".tmp",LOCK_ART_PATH)==0){(void)remove(LOCK_ART_PATH ".bak");return true;}
            (void)rename(LOCK_ART_PATH ".bak",LOCK_ART_PATH);
        }
    }
    (void)remove(LOCK_ART_PATH ".tmp");return false;
}
// 回到锁屏面要整屏 GC16 定稿（见 present）。/ A return to the lock face settles with a full GC16 (see present).
static bool s_art_settle;
static void restore_art(uint8_t *fb,const uint8_t *original){
    if(original)memcpy(fb,original,FRAME_BYTES);
    else if(!lock_screen_restore(fb))ui_lock_fallback_draw(fb);
    // 回到锁屏面必须整屏定稿，否则密码键盘留下的圈圈会印在壁纸上。
    // Returning to the lock face needs a full settle or the keypad dots stay burned into the art.
    s_art_settle = true;
}
// 从密码界面回到锁屏面时要整屏定稿（GC16）：GL16 保留白底，键盘的圈圈会作为旧像素留在壁纸上。
// Returning to the lock face from the passcode screen needs a full GC16: GL16 keeps the white
// ground and would leave the keypad dots as stale pixels on the art.
static void present(EpdiyHighlevelState *hl,EpdRect area){
    epd_poweron();
    if (s_art_settle) {
        // 定稿走整屏 GC16（update_display_full），不走区域 GL16。
        // The settle goes through a whole-frame GC16 (update_display_full), not an area GL16.
        s_art_settle = false;
        guard_draw_result(hl, update_display_full(hl));
        return;
    }
    enum EpdDrawError result=update_display_area_diff_with(hl,&E0470_WAVEFORM,MODE_GL16,area);
    guard_draw_result(hl,result);
    // 灰阶差分失败只重建同一密码画面，绝不进入未验证的页面。
    // On a display failure rebuild this same protected screen, never an unauthenticated page.
    if(result!=EPD_DRAW_SUCCESS)guard_draw_result(hl,update_display_full(hl));
}
static void present_input(EpdiyHighlevelState *hl,EpdRect area){
    enum EpdDrawError result=update_display_area_diff_with(hl,&E0470_FOLLOW_WAVEFORM,MODE_DU,area);
    guard_draw_result(hl,result);
    if(result!=EPD_DRAW_SUCCESS)present(hl,ui_pinpad_full());
}

// 清白整块画布：横版锁屏时画布是 1216×684，用旋转感知尺寸才不会只白掉竖屏那一块。
// 这里直接用 epdiy 的旋转查询（不经过 ui_kit），宿主测试切片编译本文件时只带 epdiy 替身。
// White the whole canvas: a landscape lock canvas is 1216×684, so the rotation-aware size is what
// keeps the portrait constants from blanking only part of the panel. The epdiy queries are used
// directly (not via ui_kit) because the host test slices this file with only an epdiy shim.
static void blank_canvas(uint8_t *fb){
    ui_fill_rect_fast((EpdRect){0,0,epd_rotated_display_width(),epd_rotated_display_height()},UI_GRAY_WHITE,fb);
}

void lock_screen_authenticate(EpdiyHighlevelState *hl,cst836u_handle_t touch,sc7a20h_handle_t acc,
                              bool boot,bool boot_key_unlock,bool boot_face_settled){
    if(!lock_pin_enabled())return;
    uint8_t *fb=epd_hl_get_framebuffer(hl);
    (void)acc;
    (void)ble_pt_stop(2000);
    // Pico由锁屏入口保存确认成功的样式；启动/PIN不得覆盖它。
    // Pico persists validated art at the lock entry; boot/PIN must not overwrite it.
    uint8_t *original=heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)>FRAME_BYTES+1024u*1024u
        ?heap_caps_malloc(FRAME_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
    if(original)memcpy(original,fb,FRAME_BYTES);
    // 二改：密码提示画面用纯白底——锁屏画面不垫在密码控件下。等待期间仍显示原锁屏样式；
    // 按键弹出提示时才刷白画键盘，取消则由 restore_art 恢复原样式。键盘的玻璃圆键在
    // begin 时采样白底。
    // Mod: the PIN prompt takes a solid white base instead of layering the lock art beneath
    // the controls. The waiting phase still shows the original lock face; the prompt phase
    // whites the panel before painting the pad, and cancel restores the art via restore_art.
    // The pad's glass keys sample the white ground at begin time.
    blank_canvas(fb);
    ui_pinpad_t pad={0};ui_pinpad_begin(&pad,fb,"输入密码");
    restore_art(fb,original);
    // boot 只表示“这是启动门”；boot_key_unlock 才表示这次启动确实由电源键触发。
    // OTA/复位没有按键证据时必须先显示锁面，不能直接弹密码。
    // `boot` only marks the startup gate; `boot_key_unlock` carries the actual key-wake
    // evidence. An OTA/reset must show the face and wait instead of opening the PIN.
    bool waiting=!boot_key_unlock;
    // OTA/冷启动的启动图不是锁面；先完整推一次锁面再进入按键等待。
    // A cold/OTA splash is not the lock face: settle the face once before waiting.
    bool need_paint=boot&&waiting&&!boot_face_settled;
    unsigned failures=0;int64_t retry_at=0;
    bool wait_for_release=false;
    for(;;){
        // 只有需要重画时才刷锁屏面：取消/超时后要重画，亮屏唤醒时面板本来就还是锁屏图。
        // Repaint the lock face only when needed: after cancel/timeout it must come back, but a wake
        // already shows it, so repainting just produced a pointless flash.
        if (!waiting) need_paint = true;
        if(waiting){
            if(need_paint){ restore_art(fb,original);present(hl,ui_pinpad_full()); need_paint=false; }
            // 首次锁屏已经由 enter_lock_and_sleep() 等过一次按键释放；这里只在电源键取消
            // 返回锁面后补等松键，避免连续两个 800ms 窗口吞掉首击。
            // The lock entry already waited once; only a key-cancelled prompt needs another
            // release wait here, so two consecutive 800ms windows cannot eat the first press.
            if(wait_for_release){app_lock_wait_key_idle(800);wait_for_release=false;}
            epd_poweroff();
            const int woke = (int)app_light_sleep_wait_timed(NULL,10u*60u*1000u);
            if(woke!=APP_WAKE_TIMEOUT){
                // 亮屏就是"我要解锁"：直接进密码界面，不再多吃一次按键、不再闪一下。
                // Waking is the unlock request: go straight to the passcode screen without eating a
                // second press or flashing the wallpaper.
                waiting=false;
                need_paint=true;
            }
            if(woke==APP_WAKE_TIMEOUT){
                ui_pinpad_end(&pad);
                // 深睡前把锁屏画面整屏定稿一次：面板会一直是最后一帧，所以既不要白屏，也别带残影。
                // Settle the lock art once before deep sleep: the panel keeps this last frame, so it
                // should neither go blank nor keep key ghosts.
                // 透明书封启动不会落锁图缓存；优先使用本循环开始时的内存锁面，
                // 否则这里会退化成兜底圆锁，成为深睡前最后一帧。
                // The transparent-cover boot path deliberately does not write the art cache; use the
                // in-memory face captured at entry instead of falling back to the generic lock.
                restore_art(fb, original);
                free(original);
                (void)update_display_full(hl);
                s_art_settle = false;
                // 启动门尚未显示密码就再次深睡时，也要把一次性恢复检查点重新落盘；
                // 否则十分钟后唤醒会把原本可续读的页面丢掉。
                // Re-persist the one-shot resume checkpoint before deep sleep even if the PIN
                // screen was never shown; otherwise a later wake loses the resumable page.
                if(boot){pico_boot_ready();pico_boot_hold_resume();boot=false;}
                app_enter_host_sleep(APP_SLEEP_DEEP);
                // 防御性处理：电源交接意外返回也继续验证。/ Defensive: a returning power handoff still requires verification.
                // 深睡回来 fb 是锁屏画面：先刷白让玻璃圆键重新采样，再由等待阶段的 restore_art 恢复样式。
                // After deep sleep fb holds the lock art: white it so the glass keys resample,
                // then the waiting phase restores the art again.
                original=NULL;blank_canvas(fb);
                ui_pinpad_begin(&pad,fb,"输入密码");
                need_paint=true;
                continue;
            }
        }
        ui_pinpad_reset(&pad,"输入密码","");
        if(esp_timer_get_time()/1000<retry_at){pad.blocked=true;snprintf(pad.notice,sizeof(pad.notice),"请等待 10 秒后重试");}
        // 弹出提示才刷白：等待阶段 fb 是锁屏画面，这里整屏置白再画键盘；取消由 restore_art 恢复。
        // White only when the prompt appears: fb holds the lock art during waiting, so blank
        // the panel here before painting the pad; cancel restores the art via restore_art.
        blank_canvas(fb);
        // 直接推送完整密码画面一次，不再补刷第二段背景。/ Present the complete PIN screen once, without a second backdrop stage.
        ui_pinpad_paint(fb,&pad,ui_pinpad_full());present(hl,ui_pinpad_full());
        // 密码等待不是启动失败；待验证时保留一次性恢复，不提前开书。
        // Waiting for a PIN is not a failed boot; hold one-shot resume without opening the book.
        if(boot){pico_boot_ready();pico_boot_hold_resume();boot=false;}
        ui_gesture_t gesture={0};bool was_down=false,draining=true;
        cst836u_touch_t latest={0};int64_t active_at=esp_timer_get_time()/1000,last_poll=active_at;
        bool cancel=false,key_cancel=false;
        while(!cancel){
            int64_t now=esp_timer_get_time()/1000;
            if(now-active_at>=30000){cancel=true;break;}
            if(now-last_poll>=40){last_poll=now;(void)read_pico_pmu_poll();if(read_pico_pmu_take_key_action()!=READ_PICO_PMU_KEY_NONE){cancel=true;key_cancel=true;break;}}
            if(pad.blocked&&lock_pin_available()&&now>=retry_at){ui_pinpad_reset(&pad,NULL,"请重新输入");ui_pinpad_paint(fb,&pad,ui_pinpad_entry_area());present(hl,ui_pinpad_entry_area());}
            cst836u_touch_t sample={0};esp_err_t error=cst836u_read(touch,&sample);
            bool down=error==ESP_OK&&sample.touched;
            if(draining){if(!down&&error==ESP_OK)draining=false;was_down=down;vTaskDelay(pdMS_TO_TICKS(5));continue;}
            if(down)latest=sample;else{latest.touched=false;latest.count=0;}
            app_ctx_t ctx={.now_ms=now,.touch=&latest,.pressed=down&&!was_down,.released=!down&&was_down,.consumed=error!=ESP_OK};
            ui_gesture_event_t event;
            if(ui_gesture_feed(&gesture,&ctx,&event)){
                active_at=now;EpdRect dirty;ui_pin_result_t r=ui_pinpad_handle(&pad,&event,&dirty);
                if(r==UI_PIN_CANCEL){cancel=true;break;}
                if(r!=UI_PIN_NONE){ui_pinpad_paint_input(fb,&pad);present_input(hl,dirty);}
                if(r==UI_PIN_COMPLETE){
                    bool verified=lock_pin_verify(pad.digits);
                    if(verified){
                        // 密码键盘是按局部 DU 刷的，直接放行会让解锁后的第一帧带着键位残影；
                        // 让下一页整屏 GC16 定稿，把键盘和圆点一次清干净。
                        // The pad is presented with partial DU updates, so releasing straight away leaves
                        // key ghosts in the first frame after unlock; the next full GC16 settles it.
                        display_request_full_refresh();
                        // 解锁成功即作废锁屏图缓存：哪怕历史上有过把键盘画进去的帧，也不会再被复用。
                        // Drop the cached art on a successful unlock so a polluted frame can never return.
                        (void)remove(LOCK_ART_PATH);
                        ui_pinpad_end(&pad);free(original);read_pico_pmu_drain_events();ble_pt_event_t e;while(ble_pt_pop_key(&e)){}ble_pt_raw_t raw;while(ble_pt_pop_raw(&raw)){}return;
                    }
                    ui_pinpad_reset(&pad,NULL,"密码不正确，请重试");
                    if(++failures>=3){failures=0;retry_at=now+10000;pad.blocked=true;snprintf(pad.notice,sizeof(pad.notice),"请等待 10 秒后重试");}
                    ui_pinpad_paint(fb,&pad,ui_pinpad_entry_area());present(hl,ui_pinpad_entry_area());
                }
            }
            was_down=down;vTaskDelay(pdMS_TO_TICKS(5));
        }
        ui_pinpad_reset(&pad,NULL,"");
        // 取消/超时回到锁屏前，先把整屏刷成纯白并定稿一次：密码键盘是用局部 DU 画的，
        // 部分面板上单次 GC16 仍会留下浅痕，两面白底 + 定稿能彻底抹掉。
        // Before going back to the lock face, push one whole white frame: the pad is drawn with
        // partial DU updates and a single GC16 leaves faint traces on some panels.
        blank_canvas(fb);
        (void)update_display_full(hl);
        need_paint=true;
        waiting=true;
        wait_for_release=key_cancel;
    }
}
