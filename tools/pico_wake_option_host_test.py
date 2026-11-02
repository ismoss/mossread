#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：真实唤醒选项、设置命中与深睡开机门。/ English: Actual wake preferences, setting hits and the deep-boot gate."""
from pathlib import Path
import subprocess,tempfile,re
from home_cover_cache_host_test import function
ROOT=Path(__file__).resolve().parents[1]
settings=ROOT/'main/settings.c'
ui=(ROOT/'main/apps/app_device_settings.c').read_text()
boot_path=ROOT/'boards/pico/src/main/app_main.c'
if not boot_path.exists(): boot_path=ROOT/'main/app_main.c'
boot=boot_path.read_text()
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define NVS_KEY_WAKE_PRESSES "wake_press"
#define NVS_KEY_WAKE_MIGRATED "wake_migr"
#define NVS_NS "settings"
#define NVS_READWRITE 1
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND -2
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
typedef int nvs_handle_t;
static esp_err_t nvs_put_wake_presses(uint8_t presses);
static uint8_t stored,staged_stored;static unsigned writes,migration_writes,gate,ready,held;
static bool pin,boot_press,fail_wake_write,fail_migration_write,fail_wake_read,fail_commit;
static bool staged_value,staged_marker;
static esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h){
 assert(!strcmp(ns,NVS_NS)&&mode==NVS_READWRITE&&h);*h=1;return ESP_OK;
}
static esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t value){
 assert(h==1);
 if(!strcmp(key,NVS_KEY_WAKE_PRESSES)){++writes;if(fail_wake_write)return -1;staged_stored=value;staged_value=true;return ESP_OK;}
 assert(!strcmp(key,NVS_KEY_WAKE_MIGRATED));++migration_writes;if(fail_migration_write)return -1;
 staged_marker=true;return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h){
 assert(h==1);if(fail_commit)return -1;if(staged_value)stored=staged_stored;
 staged_value=staged_marker=false;return ESP_OK;
}
static void nvs_close(nvs_handle_t h){assert(h==1);staged_value=staged_marker=false;}
'''
unit+=re.search(r'^static uint8_t s_wake_presses = [12];',settings.read_text(),re.M)[0]+'\n'
for name in ('app_settings_wake_presses','app_settings_set_wake_presses'):
    unit+=function(name,settings)+'\n'
unit+=function('nvs_put_wake_presses',settings)+'\n'
unit+=r'''
#include "ui_nav_layout.h"
#define KIIKO_BOARD_METALIO47 0
typedef struct {int x,y,width,height;}EpdRect;
enum {SETTINGS_MAIN,SETTINGS_POWER_SLEEP,SETTINGS_AUTO_LOCK,SETTINGS_SYSTEM_FONT,
 SETTINGS_SYSTEM_CONTRAST,SETTINGS_MAIN_REFRESH,SETTINGS_FRONTLIGHT,APP_REDRAW_PAGE,APP_REDRAW_NONE};
static int s_page=SETTINGS_MAIN,s_font_page;static char s_notice[96];
static bool home_full;
static void ttf_font_scan(void){}
static bool app_settings_home_full_refresh(void){return home_full;}
static void app_settings_set_home_full_refresh(bool on){home_full=on;}
static bool app_settings_reader_adaptive(void){return false;}
static void app_settings_set_reader_adaptive(bool on){(void)on;}
'''
for name in ('settings_profile_height','settings_main_row_height'):
    unit+=function(name,ROOT/'main/apps/app_device_settings.c')+'\n'
a=ui.index('#define SETTINGS_ROW_H');b=ui.index('// 设置行图标',a)
unit+=ui[a:b]
a=ui.index('    if(y>=SETTINGS_DISPLAY_Y &&',ui.index('static app_redraw_t on_gesture'))
b=ui.index('    if(y>=SETTINGS_PERSONAL_Y',a)
unit+=r'''
static int setting_tap(int x,int input_y,int scroll){
 int y=input_y,row;
 if (y<160 || y>=UI_NAV_TOP || x<36 || x>=648) return APP_REDRAW_NONE;
 y+=scroll;
'''+ui[a:b]+'return APP_REDRAW_NONE;}\n'
unit+=r'''
static bool load_exists,migrated_exists;static uint8_t load_value,migrated_value;
static int nvs_get_u8(int h,const char *key,uint8_t *value){
 (void)h;
 if(!strcmp(key,NVS_KEY_WAKE_PRESSES)){if(fail_wake_read)return -1;if(!load_exists)return ESP_ERR_NVS_NOT_FOUND;*value=load_value;return ESP_OK;}
 assert(!strcmp(key,NVS_KEY_WAKE_MIGRATED));if(!migrated_exists)return ESP_ERR_NVS_NOT_FOUND;*value=migrated_value;return ESP_OK;
}
static void reload_wake(bool exists,uint8_t value,bool migrated,uint8_t migrated_flag,bool fail_migration_value_write,bool fail_read){
 load_exists=exists;load_value=value;migrated_exists=migrated;migrated_value=migrated_flag;
 writes=migration_writes=0;fail_wake_write=fail_migration_value_write;fail_migration_write=false;
 fail_wake_read=fail_read;fail_commit=false;staged_value=staged_marker=false;int h=0;uint8_t raw;
'''
a=settings.read_text().index('    raw = 1;\n    const esp_err_t wake_read = nvs_get_u8(h, NVS_KEY_WAKE_PRESSES,')
b=settings.read_text().index('    s_wake_presses = raw;',a)+len('    s_wake_presses = raw;')
unit+=settings.read_text()[a:b]+'}\n'
unit+=r'''
static bool read_pico_pmu_take_boot_short_press(void){bool value=boot_press;boot_press=false;return value;}
static bool late_press,double_press;
static bool read_pico_pmu_take_key_wakeup_single(void){bool value=late_press;late_press=false;return value;}
static bool read_pico_pmu_take_key_wakeup(void){bool value=double_press;double_press=false;return value;}
static void vTaskDelay(unsigned ticks){(void)ticks;}
#define pdMS_TO_TICKS(ms) (ms)
static bool lock_pin_enabled(void){return pin;}
typedef struct {uint8_t wake_reason;} pmu_snapshot_t;
static pmu_snapshot_t boot_pmu;
static const pmu_snapshot_t *read_pico_pmu_get(void){return &boot_pmu;}
enum {ESP_RST_POWERON=1,ESP_RST_DEEPSLEEP=2,ESP_RST_SW=3};
typedef int esp_reset_reason_t;
static void pico_boot_ready(void){++ready;}
static void pico_boot_hold_resume(void){++held;}
static bool app_lock_resume_frame(void *hl){(void)hl;return false;}
static bool gate_boot_key;
static void app_lock_boot_gate(void *a,int b,int c,bool boot_key_unlock){(void)a;(void)b;(void)c;gate_boot_key=boot_key_unlock;++gate;}
static void boot_gate(bool resuming,esp_reset_reason_t reset,uint8_t wake_reason){
 int hl=0;struct {int touch,sensor;bool pmu_ready;}hw={0};hw.pmu_ready=true;bool restored_lock=false;(void)restored_lock;
 boot_pmu.wake_reason=wake_reason;
'''
start=boot.index('    const pmu_snapshot_t *boot_pmu = read_pico_pmu_get();')
end=boot.index('    rp_boot_mark("lock");',start)
unit+=boot[start:end]+'}\n'
unit+=r'''
int main(void){
 assert(app_settings_wake_presses()==1);
 reload_wake(false,2,false,0,false,false);
 assert(app_settings_wake_presses()==1&&writes==1&&migration_writes==1&&stored==1);
 writes=migration_writes=0;
 app_settings_set_wake_presses(0);assert(writes==1&&migration_writes==1&&app_settings_wake_presses()==1);
 writes=migration_writes=0;
 app_settings_set_wake_presses(2);assert(stored==2&&writes==1&&migration_writes==1);
 writes=migration_writes=0;
 app_settings_set_wake_presses(2);assert(stored==2&&writes==1&&migration_writes==1);
 // 标记或最终提交失败时不能把“值 + 标记”只落一半；本次会话仍按新选择运行，重启后保持旧值。
 // A marker/commit failure must not leave a half pair; the session uses the choice, reboot keeps old.
 stored=1;writes=migration_writes=0;fail_migration_write=true;
 app_settings_set_wake_presses(2);
 assert(app_settings_wake_presses()==2&&stored==1&&writes==1&&migration_writes==1);fail_migration_write=false;
 stored=1;writes=migration_writes=0;fail_commit=true;
 app_settings_set_wake_presses(2);
 assert(app_settings_wake_presses()==2&&stored==1&&writes==1&&migration_writes==1);fail_commit=false;
 reload_wake(true,2,false,0,false,false);assert(app_settings_wake_presses()==1&&writes==1&&migration_writes==1);
 reload_wake(true,2,true,1,false,false);assert(app_settings_wake_presses()==2&&!writes&&!migration_writes);
 reload_wake(true,1,true,1,false,false);assert(app_settings_wake_presses()==1&&!writes&&!migration_writes);
 for(unsigned i=0;i<256;i++)if(i!=1&&i!=2){reload_wake(true,i,true,1,false,false);assert(app_settings_wake_presses()==1);}
 // 迁移值写失败时不能落标记；下次开机仍重试，而本次会话仍按单击处理。
 // A failed migration write must not bless the marker; the next boot retries and this session
 // still behaves as single-press.
 reload_wake(true,2,false,0,true,false);
 assert(app_settings_wake_presses()==1&&writes==1&&!migration_writes);
 // 读取旧值失败时不能把它当成“没有历史值”而落迁移标记。
 // A failed old-value read must not be mistaken for an absent legacy value.
 reload_wake(true,2,false,0,false,true);
 assert(app_settings_wake_presses()==1&&!writes&&!migration_writes);
 assert(SETTINGS_DISPLAY_ROWS==8);
 const int top=SETTINGS_DISPLAY_Y+5*SETTINGS_ROW_H,scroll=500;
 assert(setting_tap(36,top-scroll,scroll)==APP_REDRAW_PAGE&&s_wake_presses==2&&s_page==SETTINGS_MAIN);
 assert(setting_tap(647,top+SETTINGS_ROW_H-1-scroll,scroll)==APP_REDRAW_PAGE&&s_wake_presses==1&&s_page==SETTINGS_MAIN);
 assert(setting_tap(35,top-scroll,scroll)==APP_REDRAW_NONE&&s_wake_presses==1);
 assert(setting_tap(648,top-scroll,scroll)==APP_REDRAW_NONE&&s_wake_presses==1);
 assert(setting_tap(300,top-1-scroll,scroll)==APP_REDRAW_PAGE&&s_page==SETTINGS_AUTO_LOCK&&s_wake_presses==1);
 s_page=SETTINGS_MAIN;
 assert(setting_tap(300,top+SETTINGS_ROW_H-scroll,scroll)==APP_REDRAW_PAGE&&s_page==SETTINGS_POWER_SLEEP&&s_wake_presses==1);
 assert(setting_tap(300,UI_NAV_TOP,0)==APP_REDRAW_NONE);
 for(unsigned resume=0;resume<2;++resume)for(unsigned password=0;password<2;++password)
 for(unsigned key=0;key<2;++key)for(unsigned count=1;count<=2;++count){
   s_wake_presses=count;pin=password;boot_press=key;late_press=double_press=false;gate=ready=held=gate_boot_key=0;
  boot_gate(resume,ESP_RST_POWERON,key?2:1);bool direct=resume&&!password&&key&&count==1;
  bool immediate=resume&&password&&key&&count==1;
  assert(gate==!direct&&ready==direct&&held==direct&&!boot_press&&gate_boot_key==immediate);
 }
 s_wake_presses=1;pin=false;boot_press=false;late_press=true;gate=ready=held=gate_boot_key=0;
 boot_gate(true,ESP_RST_POWERON,2);assert(!gate&&ready==1&&held==1&&!late_press&&!gate_boot_key);
 s_wake_presses=2;pin=false;boot_press=false;late_press=true;gate=ready=held=gate_boot_key=0;
 boot_gate(true,ESP_RST_POWERON,2);assert(gate&&!ready&&!held&&late_press&&!gate_boot_key);
 // 双击设置下不能为了“补单击证据”清掉 PMU 已经起步的双击状态。
 // A double-press boot must not clear the PMU's in-progress double-click while looking for single-press evidence.
 s_wake_presses=2;pin=true;boot_press=false;late_press=true;gate=ready=held=gate_boot_key=0;
 boot_gate(true,ESP_RST_POWERON,2);assert(gate&&late_press&&!gate_boot_key);
 // 没有按键证据的 OTA/软复位启动必须等锁面；真正的冷/深睡按键启动才携带
 // direct-unlock 证据。
 // An OTA/software-reset boot has no key evidence and must wait; a real
 // cold/deep key boot carries the direct-unlock evidence.
 s_wake_presses=1;pin=true;boot_press=false;late_press=double_press=false;gate=ready=held=gate_boot_key=0;
 boot_gate(true,ESP_RST_SW,2);assert(gate&&!gate_boot_key);
 boot_gate(true,ESP_RST_POWERON,5);assert(gate&&!gate_boot_key);
 boot_gate(true,ESP_RST_DEEPSLEEP,2);assert(gate&&gate_boot_key);
 // 复位原因不理想时，握手后补等到的短按也不能冒充物理开机证据。
 // A late short press cannot impersonate physical boot evidence on a software reset.
 s_wake_presses=1;pin=true;boot_press=false;late_press=true;gate=ready=held=gate_boot_key=0;
 boot_gate(true,ESP_RST_SW,0);assert(gate&&!gate_boot_key&&late_press);
 s_wake_presses=1;pin=false;boot_press=false;late_press=true;gate=ready=held=gate_boot_key=0;
 boot_gate(true,ESP_RST_SW,0);assert(gate&&!gate_boot_key&&!ready&&!held&&late_press);
 // 物理复位上的迟到按键仍是合法证据；真实完成的双击也能直达。
 // A late key on a physical reset remains valid, and a completed double press is sufficient.
 s_wake_presses=1;pin=true;boot_press=false;late_press=true;gate=ready=held=gate_boot_key=0;
 boot_gate(true,ESP_RST_POWERON,0);assert(gate&&gate_boot_key&&!late_press);
 // 充电/闹钟等已知非按键启动即使有迟到短按也不能直达。
 // A known non-key charger/alarm boot must not unlock from a late short press.
 s_wake_presses=1;pin=true;boot_press=false;late_press=true;gate=ready=held=gate_boot_key=0;
 boot_gate(true,ESP_RST_POWERON,5);assert(gate&&!gate_boot_key&&late_press);
 // 已知非按键启动不能提前消费用户随后按下的短按锁存。
 // A known non-key boot must not consume the following user short-press latch.
 s_wake_presses=1;pin=true;boot_press=true;late_press=false;gate=ready=held=gate_boot_key=0;
 boot_gate(true,ESP_RST_POWERON,5);assert(gate&&!gate_boot_key&&boot_press);
 s_wake_presses=1;pin=true;boot_press=true;late_press=false;gate=ready=held=gate_boot_key=0;
 boot_gate(true,ESP_RST_SW,2);assert(gate&&!gate_boot_key&&boot_press);
 for(unsigned password=0;password<2;++password){
  s_wake_presses=2;pin=password;boot_press=false;late_press=false;double_press=true;
  gate=ready=held=gate_boot_key=0;boot_gate(true,ESP_RST_POWERON,2);
  assert(gate&&gate_boot_key&&!double_press);
 }
 puts("PASS: single-press default/missing/invalid NVS, explicit double choice retained, root switch below auto sleep, scrolled row/nav boundaries, wake/PIN/late press guards");
}
'''
with tempfile.TemporaryDirectory() as folder:
    p=Path(folder);(p/'test.c').write_text(unit)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-I'+str(ROOT/'main/ui'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
