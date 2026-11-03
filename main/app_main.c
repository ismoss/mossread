/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 用户修订：硬件前建立持久化启动保护；深睡恢复入口先消费再开书，中断图书进入可操作恢复页。
 * User revision: persist startup protection before hardware; consume deep-sleep checkpoints before opening books, and show an operable recovery page after interrupted opening.
 * 只做开机装配：拉起板级硬件、读设置、定 VCOM、放开机图、开字体；
 * 未标定则先拦住进设定页。然后把控制权交给 app_loop。
 *
 * Boot wiring only: board init, settings, PMU RTC restore, VCOM, splash, fonts. If VCOM
 * is unset, the factory page blocks first. Then control goes to app_loop.
 */

#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "app_loop.h"
#include "app_content_open.h"
#include "boot_state.h"
#include "mod_monitor.h"
#include "lock_screen.h"
#include "ui_lock_feedback.h"
#include "lock_pin.h"
#include "ttf_font.h"
#include "book_store.h"
#include "esp_system.h"
#include "read_pico_sd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/stat.h>
#include "app_registry.h"
#include "display.h"
#include "epd_highlevel.h"
#include "epdiy.h"
#include "esp_log.h"
#include "ota_update.h"
#include "pmu_selftest.h"
#include "soc/rtc_cntl_reg.h"
#include "read_pico_board.h"
#include "read_pico_crashlog.h"
#include "read_pico_init.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "settings.h"
#include "mod_dns.h"
#include "read_pico_transfer.h"
#include "sleep.h"
#include "ttf_font.h"
#include "usb_storage.h"
#include "app_font_context.h"
#include "ui_kit.h"
#include "ui_font.h"
#include "vcom_setup.h"

static const char* TAG = "read_pico";
extern const app_desc_t app_boot_recovery;

// 软复位保留有效的 S3 RTC；断电才用 PMU 的实时秒和毫秒恢复。
// Retain a valid S3 RTC across soft reset; restore fresh PMU seconds and milliseconds after power loss.
static void restore_time_from_pmu(void) {
    struct timeval current;
    if (gettimeofday(&current, NULL) == 0 && current.tv_sec >= 1704067200 &&
        current.tv_sec <= 4102444799U) return;
    if (read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) != ESP_OK) return;
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (!pmu || !pmu->time_ok || !pmu->time_synced || pmu->unix_millis >= 1000 ||
        pmu->unix_sec < 1704067200 || pmu->unix_sec > 4102444799U) return;
    struct timeval tv = {.tv_sec = (time_t)pmu->unix_sec, .tv_usec = pmu->unix_millis * 1000};
    if (settimeofday(&tv, NULL) == 0) ESP_LOGI(TAG, "system clock restored from PMU");
}


// 已标定则开机读一次喂给 epdiy。未标定先用板级默认出开机图，随后拦住进标定页。
// Load VCOM once when set. Otherwise keep the board default for the splash,
// then gate into the setup page.
static bool resolve_vcom_at_boot(void) {
    int mv = 0;
    for (int i = 0; i < 3; i++) {
        if (read_pico_pmu_vcom_get(&mv) == ESP_OK) {
            epd_set_vcom((uint16_t)mv);
            ESP_LOGI(TAG, "panel VCOM loaded from PMU");
            return true;
        }
    }
    ESP_LOGW(TAG, "panel VCOM unset, keeping board default until setup");
    return false;
}

// 极简启动画面直接借内建字形绘制，不读TF、不借阅读字体、不保存整屏位图。
// Draw the minimal cold splash with built-in glyphs, without TF access, reader faces or a full-screen asset.
static void __attribute__((unused)) draw_startup_frame(uint8_t *framebuffer) {
    ui_clear_page(framebuffer);
    const int cx = epd_rotated_display_width() / 2;
    const int cy = epd_rotated_display_height() / 2;
    static const char *const wordmark[] = {"m", "o", "s", "s", "r", "e", "a", "d"};
    static const char *const caption[] = {"正", "在", "唤", "醒"};
    // 星图：确定性伪随机撒点（同一个种子每次开机都一样）+ 上半区一段星座连线。撒点避开中间
    // 的文字带，连线只走上半区，保证字标周围是干净的。全部用内建图元画，不读卡、不占 flash。
    // Star chart: deterministic scatter from a fixed seed plus one constellation line in the upper
    // band. Dots avoid the text band and lines stay high, so the wordmark sits on clean paper. Drawn
    // with built-in primitives only: no card access and no flashed bitmap.
    {
        const int w = epd_rotated_display_width(), h = epd_rotated_display_height();
        const int text_top = cy - 110, text_bottom = cy + 70;
        uint32_t seed = UINT32_C(0x6d6f7373); // "moss"
        int px_[6], py_[6], stars = 0;
        for (int i = 0; i < 150; ++i) {
            seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
            const int sx = (int)(seed % (uint32_t)w);
            seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
            const int sy = (int)(seed % (uint32_t)h);
            if (sy > text_top && sy < text_bottom) continue;   // 文字带留白 / keep the wordmark band clean
            epd_fill_circle(sx, sy, (i % 9 == 0) ? 2 : 1, (i % 9 == 0) ? 0x98 : 0xc8, framebuffer);
            if (stars < 6 && sy < h / 3) { px_[stars] = sx; py_[stars] = sy; ++stars; }
        }
        for (int i = 0; i + 1 < stars; ++i)
            epd_draw_line(px_[i], py_[i], px_[i + 1], py_[i + 1], 0xb0, framebuffer);
    }
    const struct {
        const char *text;
        const char *const *letters;
        int count, px, tracking, gray, center_y;
    } lines[] = {
        {"mossread", wordmark, 8, 72, 2, 0, cy - 50},
        {"正在唤醒", caption, 4, 24, 8, 7, cy + 20},
    };
    // 以实际字宽居中，疏朗品牌字距与浅灰提示形成层次，不引入新的字体或位图。
    // Center measured advances; tracked branding and a muted caption add hierarchy without new fonts or bitmaps.
    for (size_t row = 0; row < sizeof(lines) / sizeof(lines[0]); ++row) {
        int above = 0, below = 0;
        ui_font_measure_line_px(lines[row].px, lines[row].text, &above, &below);
        int width = (lines[row].count - 1) * lines[row].tracking;
        for (int i = 0; i < lines[row].count; ++i)
            width += ui_font_text_width_px(lines[row].px, lines[row].letters[i]);
        int x = cx - width / 2;
        const int baseline = lines[row].center_y + (above - below) / 2;
        for (int i = 0; i < lines[row].count; ++i) {
            ui_font_draw_text_px(framebuffer, x, baseline, lines[row].px,
                                lines[row].letters[i], EPD_DRAW_ALIGN_LEFT,
                                lines[row].gray, 15, false);
            x += ui_font_text_width_px(lines[row].px, lines[row].letters[i]) + lines[row].tracking;
        }
    }
}

// 深睡保持面板上原锁屏；挂卡与参考帧恢复由锁屏输入循环继续，不能用圆锁覆盖。
// Keep the retained lock on deep boot; the locked-input loop resumes pending media/reference restoration without a badge replacement.
static bool prepare_startup_frame(EpdiyHighlevelState *hl,uint8_t *framebuffer,bool resuming,bool vcom_ok) {
    if(resuming && vcom_ok)return app_lock_resume_frame(hl);
    epd_poweron();
    if(!resuming)read_pico_i2c_census_take();
    epd_clear();
    epd_hl_set_all_white(hl);
    // 样式4关闭锁屏画面：续启无缓存时只留白底，不画兜底锁面。
    // Style 4 disables the lock face: a cache-less resume keeps plain white without the fallback badge.
    // 冷启动 splash 一律按竖屏画：它只在这台机器"不会恢复进书"时出现（有恢复检查点就走锁屏续帧），
    // 也就是说落地界面一定是竖屏。以前按"上次锁屏在书内"猜横屏，于是出现"竖屏主页面配一张横屏开机图"。
    // The cold splash is always portrait: it only appears when this boot will not resume into a book
    // (a pending checkpoint goes through the retained lock frame), so the UI it hands over to is
    // portrait. It used to guess landscape from "the last lock was inside a book", which produced a
    // landscape splash in front of a portrait UI.
    // 透明书封没有可恢复的底图时不要先画白色圆锁；让启动门等 SD/缓存后再一次性出图。
    // Do not show the white badge before a transparent-cover boot; let the gate wait for media
    // and draw the final face once.
    // 冷启动 / 深睡唤醒不再画「准备资源」开机图（mossread 大字+星图），统一只显示小锁脸，
    // 与 PageIsle 一致，消除唤醒时 "mossread → 壁纸" 的闪现。样式 4（关闭锁屏）仍只留白底，
    // 样式 3（透明书封）续启时交给启动门等 SD 就绪后再一次性出图。
    // No preparing-resources splash on cold boot / deep wake any more: draw the small lock face
    // instead (as PageIsle does), removing the "mossread -> wallpaper" flash. Style 4 stays white;
    // style 3's transparent cover is drawn later by the gate after the SD card is ready.
    uint8_t boot_style = app_settings_lock_style();
    if (boot_style == 4) {
        // 关闭锁屏画面：只留白底 / lock face disabled: plain white
    } else if (resuming && boot_style == 3) {
        // 透明书封：由启动门在 SD 就绪后出图 / transparent cover: drawn by the gate after SD
    } else {
        ui_lock_fallback_draw(framebuffer);
    }
    update_display_from_white(hl);
    return false;
}

void app_main(void) {
    // 共享 TF 卡期间复位后，先把内部 PHY 交回串口，再清理已用完的 BOOT 请求。
    // Reclaim Serial/JTAG after a reset during card sharing, then clear the served BOOT request.
    usb_storage_phy_init();
    REG_CLR_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);

    const esp_reset_reason_t reset = esp_reset_reason();
    ESP_LOGI(TAG, "startup reset=%d stack=%u", (int)reset, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    app_settings_init();
    // 二改（mod29）：自动连接"老设备继承、新设备默认关"。mod27 把默认从开改成关，而开关值只有
    // 用户拨动时才写 NVS，于是"从没拨过"的旧设备升级后会被判成关。这里只在键还缺失的开机里判定
    // 一次并落盘：有已保存的 WiFi 凭据 = 这台设备之前确实在自动连 → 开；没有任何凭据 = 新设备
    // → 关。落盘后不再判断，用户以后怎么拨就怎么存。
    // Mod (mod29): auto-connect inherits the old default exactly once. mod27 flipped the default to
    // off, but the key is only written on a toggle, so a device that never toggled read as off after
    // the upgrade. On a boot where the key is still absent, decide from the saved WiFi credential
    // (present -> it really was auto-connecting, keep it on; none -> new device, off) and persist it,
    // so a network saved later can never flip the switch behind the user's back.
    if (!app_settings_wifi_auto_stored()) {
        char legacy_ssid[33];
        bool legacy_configured = false;
        if (read_pico_transfer_get_saved_wifi(legacy_ssid, &legacy_configured) == ESP_OK)
            app_settings_set_wifi_auto_connect(legacy_configured);
    }
    // 应用「DNS 设置」里保存的选择，赶在任何联网之前。/ Apply the saved DNS choice before any networking.
    mod_dns_init();
    // 远程监看开关在 NVS；服务本体由主循环按联网状态起停。
    // The remote-monitor switch lives in NVS; the main loop starts/stops the server.
    mod_monitor_init();
    // 在大缓冲和字体之前读取小型锁屏设置；读取失败不阻塞，之后的检查仍可重试。
    // Read the tiny lock setting before large buffers/fonts; keep failures nonblocking and let subsequent checks retry.
    (void)lock_pin_enabled();
    pico_boot_init(reset == ESP_RST_PANIC || reset == ESP_RST_INT_WDT ||
                   reset == ESP_RST_TASK_WDT || reset == ESP_RST_WDT);

    read_pico_handle_t hw;
    if (read_pico_init(&hw) != ESP_OK) return;
    rp_boot_mark("hw");
    if (hw.pmu_ready) restore_time_from_pmu();
    const bool vcom_ok = resolve_vcom_at_boot();
    rp_boot_mark("vcom");
    pmu_selftest_bind(hw.sensor);
    app_sleep_bind(hw.touch, hw.sensor);


    EpdiyHighlevelState hl = hw.hl;
    uint8_t* framebuffer = hw.framebuffer;

    // 深睡保留屏幕已有画面：验证缓存后同时恢复前后帧，省去清白和开机图两次推屏。
    // Deep sleep retains the panel image: validate its cache and restore both references, skipping white erase and splash output.
    const bool resuming = pico_boot_has_resume();
    bool restored_lock=prepare_startup_frame(&hl,framebuffer,resuming,vcom_ok);
    rp_boot_mark("splash");
    // 直接续读不先加载整份系统字体；密码验证仍先使用所选系统字体。
    // Direct reader resume skips loading the whole system face first; PIN authentication still uses the selected UI face.
    if(!resuming || lock_pin_enabled())app_font_activate_system();
    else (void)ttf_font_open_builtin();
    rp_boot_mark("font");

    if (!hw.touch_ready) {
        ESP_LOGE(TAG, "No touch controller, UI cannot run");
        return;
    }

    if (hw.pmu_ready && !vcom_ok) {
        vcom_setup_run(&hl, framebuffer, hw.touch);
    }

    // 硬件、显示、触摸与必要的出厂设置均成功后确认新槽。
    // Confirm a new slot only after hardware, display, touch, and required setup succeed.
    pico_ota_confirm_running();

    // 单击开机：开机那一次短按就算开机，直接续读；双击开机仍停在锁屏等第二次。
    // One-press boot treats the powering short press as the boot; double-click boot still waits on the lock face.
    const pmu_snapshot_t *boot_pmu = read_pico_pmu_get();
    const bool key_boot_reset = reset == ESP_RST_POWERON || reset == ESP_RST_DEEPSLEEP;
    const bool pmu_says_key_boot = boot_pmu != NULL && boot_pmu->wake_reason == 2;
    const bool pmu_wake_unknown = boot_pmu != NULL && boot_pmu->wake_reason == 0;
    // 放宽（对齐 PageIsle）：电源键复位（POWERON/DEEPSLEEP）就尝试取一次开机短按证据，不再强求
    // wake_reason==2；但充电(5)/闹钟(6)启动不取锁存，OTA/软复位/异常复位仍被 key_boot_reset 排除。
    // Relaxed (as PageIsle): capture the boot short-press on any POWERON/DEEPSLEEP reset instead of
    // only wake_reason==2, but not on charger(5)/alarm(6) boots; OTA/soft/abnormal resets stay excluded.
    const bool boot_key_candidate = key_boot_reset &&
        !(boot_pmu && (boot_pmu->wake_reason == 5 || boot_pmu->wake_reason == 6));
    const bool one_press_boot = resuming && !lock_pin_enabled() && hw.pmu_ready &&
        app_settings_wake_presses() == 1;
    // 充电/闹钟/OTA/软复位启动时不要提前取走短按锁存；事件要留给锁面等待器。
    // Do not take the short-press latch on charger/alarm/OTA/software-reset boots; the event
    // must remain available to the lock-face waiter.
    bool boot_press = boot_key_candidate && read_pico_pmu_take_boot_short_press();
    // 按住稍久会被 PMU 记成长按，释放也可能晚于握手；有界补等一次按键事件，避免已经按过还要再按。
    // 设了密码时也保留这枚真实按键证据，供启动门判断是否可以直达密码。
    // A slightly long hold is logged as a long press and its release can land after the handshake.
    // Keep the same bounded wait for PIN boots so the real key event can carry the unlock evidence.
    const bool single_pin_boot = resuming && lock_pin_enabled() &&
        app_settings_wake_presses() == 1;
    // 只有“这次复位本来就有按键证据”时才补等迟到事件。OTA/软复位/异常复位以及充电、
    // 闹钟启动都不能把用户后来按下的第一次键偷偷消费掉；那一次键应留给锁面。
    // Wait for a late event only when this reset already carries key evidence. OTA/software/
    // fault, charger and alarm boots must not consume the user's first press; leave it for the
    // lock face.
    const bool await_late_key = key_boot_reset && !boot_press &&
        (pmu_says_key_boot || pmu_wake_unknown);
    for (unsigned wait = 0;
         (one_press_boot || single_pin_boot) && await_late_key && !boot_press && wait < 50;
         ++wait) {
        boot_press = read_pico_pmu_take_key_wakeup_single();
        if (!boot_press) vTaskDelay(pdMS_TO_TICKS(20));
    }
    // “看到过按键”不能脱离复位原因：OTA/esp_restart 期间 PMU 也可能残留或收到短按，
    // 但那次复位本身不是用户从关机/深睡中按键启动，不能因此把恢复检查点当作已解锁。
    // A key event without a physical power/deep-sleep reset is not unlock evidence: OTA and
    // esp_restart can retain or receive a short press while the app is already rebooting.
    const bool physical_key_boot = key_boot_reset && (boot_press || pmu_says_key_boot);
    const uint8_t wake_presses = app_settings_wake_presses();
    const bool boot_with_one_press = one_press_boot && physical_key_boot;
    const bool double_press_boot = physical_key_boot && resuming && wake_presses == 2 &&
        read_pico_pmu_take_key_wakeup();
    const bool boot_key_unlock = resuming && hw.pmu_ready && physical_key_boot &&
        (wake_presses == 1 || (wake_presses == 2 && double_press_boot));
    if (boot_with_one_press) {
        ESP_LOGI(TAG, "resume lock cache=%d; one-press boot", restored_lock);
        pico_boot_ready();
        pico_boot_hold_resume();
    } else {
        if(resuming && !lock_pin_enabled())
            ESP_LOGI(TAG,"resume lock cache=%d; power presses=%u",restored_lock,
                (unsigned)app_settings_wake_presses());
        app_lock_boot_gate(&hl, hw.touch, hw.sensor, boot_key_unlock);
    }
    (void)app_lock_resume_frame(NULL);
    rp_boot_mark("lock");
    const app_desc_t *first = app_home_page();
    pico_resume_t resume = {0};
    if (pico_boot_take_resume(&resume)) {
        if (!resume.reader) first = app_at(resume.tab);
        else {
            // 挂载等待有界；恢复入口已消费，无卡或资源失败不会反复自动开书。
            // Bound mount waiting after consuming the checkpoint; absent media or failed resources cannot auto-open in a loop.
            if (!strncmp(resume.path, "/sdcard/", 8)) {
                read_pico_sd_info_t info;
                for (unsigned wait = 0; wait < 150 && read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED; ++wait)
                    vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (!strncmp(resume.path, "/flash/books/", 13) && !book_store_flash_ready()) {
                book_store_root_t roots[BOOK_STORE_ROOT_MAX]; int count;
                (void)book_store_roots(roots, &count);
            }
            struct stat st;
            if (!stat(resume.path, &st) && S_ISREG(st.st_mode) &&
                app_book_request_resume(resume.path, resume.fullscreen)) first = app_at(1);
        }
    }
    char interrupted[PICO_BOOT_PATH_MAX];
    if (pico_boot_interrupted_book(interrupted, sizeof(interrupted))) first = &app_boot_recovery;
    if(resuming && !lock_pin_enabled()) {
        if(first==app_at(1) && resume.reader)app_font_activate_reading();
        else app_font_activate_system();
    }
    rp_boot_mark("resume");
    app_loop_run(&(app_loop_config_t){
        .hl = &hl,
        .fb = framebuffer,
        .acc = hw.sensor,
        .tp = hw.touch,
        .sensor_ready = hw.sensor_ready,
        // 用户修订：深睡一次性恢复上次阅读或主页面；异常启动安全回首页。
        // User revision: resume the last reader/main tab once after deep sleep; abnormal starts safely enter Home.
        .first_app = first,
    });
}
