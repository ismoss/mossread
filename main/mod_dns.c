/*
 * SPDX-FileCopyrightText: 2026 ismoss
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：二改 DNS 选择的数据面：内置公共列表、在线列表（自建源 dns.json）与应用到传输组件。
 * English: Data plane of the mod DNS choice: the built-in public list, the online list
 * (dns.json on the self-hosted origin) and applying the selection to the transfer stack.
 * 冻结：不做任何 UI；联网仅显式触发；列表最多 MOD_DNS_MAX 项，条目 0 恒为路由器直通。
 * Frozen: no UI here; networking only on explicit request; at most MOD_DNS_MAX entries with entry 0 always the router passthrough.
 */
#include "mod_dns.h"

#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "read_pico_transfer.h"
#include "settings.h"
#include "ota_online.h"

static const char* TAG = "read_pico";

static const mod_dns_entry_t k_base_entries[] = {
    {"跟随路由器 DNS", "", ""},
    {"公共 DNS 默认", "223.5.5.5", "119.29.29.29"},
    {"阿里公共 DNS", "223.5.5.5", "223.6.6.6"},
    {"腾讯公共 DNS", "119.29.29.29", "119.28.28.28"},
    {"114 公共 DNS", "114.114.114.114", "114.114.115.115"},
    {"谷歌公共 DNS", "8.8.8.8", "8.8.4.4"},
};
static mod_dns_entry_t s_entries[MOD_DNS_MAX] = {
    {"跟随路由器 DNS", "", ""},
    {"公共 DNS 默认", "223.5.5.5", "119.29.29.29"},
    {"阿里公共 DNS", "223.5.5.5", "223.6.6.6"},
    {"腾讯公共 DNS", "119.29.29.29", "119.28.28.28"},
    {"114 公共 DNS", "114.114.114.114", "114.114.115.115"},
    {"谷歌公共 DNS", "8.8.8.8", "8.8.4.4"},
};
static int s_count = 6;
static char s_saved_label[sizeof(s_entries[0].name)];
static volatile mod_dns_fetch_state_t s_fetch_state;
// 在线 DNS 列表抓取缓冲（一次性、非热路径）放 PSRAM，省内部 SRAM。
// The one-shot online DNS list fetch buffer lives in PSRAM to save internal SRAM.
static EXT_RAM_BSS_ATTR char s_fetch_body[4096];
static size_t s_fetch_length;
static char s_fetch_error[80];

int mod_dns_count(void) { return s_count; }

const mod_dns_entry_t* mod_dns_entry(int index) {
    if (index < 0 || index >= s_count || index >= MOD_DNS_MAX) index = 0;
    return &s_entries[index];
}

static int find_pair(const char *main_ip, const char *backup_ip) {
    if (!main_ip || !main_ip[0]) return 0;
    for (int i = 1; i < s_count; ++i) {
        if (!strcmp(s_entries[i].main_ip, main_ip) && !strcmp(s_entries[i].backup_ip, backup_ip))
            return i;
    }
    return -1;
}

static void keep_saved_pair_visible(const char *name, const char *main_ip, const char *backup_ip) {
    if (!main_ip[0] || find_pair(main_ip, backup_ip) >= 0 || s_count >= MOD_DNS_MAX) return;
    mod_dns_entry_t *entry = &s_entries[s_count++];
    snprintf(entry->name, sizeof(entry->name), "%s", name[0] ? name : "自定义 DNS");
    snprintf(entry->main_ip, sizeof(entry->main_ip), "%s", main_ip);
    snprintf(entry->backup_ip, sizeof(entry->backup_ip), "%s", backup_ip);
}

int mod_dns_saved_index(void) {
    if (app_settings_dns_pair_known()) {
        char name[sizeof(s_saved_label)], main_ip[sizeof(s_entries[0].main_ip)];
        char backup_ip[sizeof(s_entries[0].backup_ip)];
        app_settings_dns_choice(name, sizeof(name), main_ip, sizeof(main_ip),
                                backup_ip, sizeof(backup_ip));
        return find_pair(main_ip, backup_ip);
    }
    int index = app_settings_dns_index();
    return index >= 0 && index < s_count ? index : 0;
}

const char *mod_dns_saved_name(void) {
    if (!app_settings_dns_pair_known()) return mod_dns_entry(mod_dns_saved_index())->name;
    char main_ip[sizeof(s_entries[0].main_ip)];
    char backup_ip[sizeof(s_entries[0].backup_ip)];
    app_settings_dns_choice(s_saved_label, sizeof(s_saved_label), main_ip, sizeof(main_ip),
                            backup_ip, sizeof(backup_ip));
    if (!main_ip[0]) return mod_dns_entry(0)->name;
    return s_saved_label[0] ? s_saved_label : "自定义 DNS";
}

// 把当前选择压给传输组件；条目 0 = 停用覆盖（跟随路由器）。
// Push the selection to the transfer stack; entry 0 disables the override (router passthrough).
static void apply_selection(int index) {
    const mod_dns_entry_t* entry = mod_dns_entry(index);
    bool override = entry->main_ip[0] != 0;
    read_pico_transfer_set_dns_servers(override, entry->main_ip, entry->backup_ip);
}

bool mod_dns_select(int index) {
    if (index < 0 || index >= s_count) return false;
    const mod_dns_entry_t* entry = mod_dns_entry(index);
    esp_err_t err = app_settings_set_dns_choice((uint8_t)index, entry->name, entry->main_ip,
                                                entry->backup_ip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dns save failed: %s", esp_err_to_name(err));
        return false;
    }
    apply_selection(index);
    ESP_LOGI(TAG, "dns select %d %s", index, s_entries[index].name);
    return true;
}

void mod_dns_init(void) {
    if (app_settings_dns_pair_known()) {
        char name[sizeof(s_saved_label)], main_ip[sizeof(s_entries[0].main_ip)];
        char backup_ip[sizeof(s_entries[0].backup_ip)];
        app_settings_dns_choice(name, sizeof(name), main_ip, sizeof(main_ip),
                                backup_ip, sizeof(backup_ip));
        ip4_addr_t main_addr, backup_addr;
        if (!main_ip[0]) {
            read_pico_transfer_set_dns_servers(false, NULL, NULL);
            return;
        }
        if (ip4addr_aton(main_ip, &main_addr) && ip4addr_aton(backup_ip, &backup_addr) &&
            main_addr.addr && backup_addr.addr) {
            // 在线列表只在本次开机保留；把持久化的自定义地址对补成可见项。
            // The online list lasts only for this boot; expose the persisted custom pair as a row.
            keep_saved_pair_visible(name, main_ip, backup_ip);
            read_pico_transfer_set_dns_servers(true, main_ip, backup_ip);
            return;
        }
    }
    int index = mod_dns_saved_index();
    if (index < 0) index = 0;
    const mod_dns_entry_t *entry = mod_dns_entry(index);
    (void)app_settings_set_dns_choice((uint8_t)index, entry->name, entry->main_ip,
                                      entry->backup_ip);
    apply_selection(index);
}

// 单次 HTTPS GET 到内存；按 OTA 客户端规则不跟随重定向。
// One HTTPS GET into memory; redirects are refused exactly like the OTA client.
static bool https_get(const char* url, char* body, size_t cap, size_t* length) {
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 7000, .buffer_size = 1024, .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    bool ok = false;
    esp_http_client_set_header(client, "User-Agent", PICO_OTA_CAPABILITY);
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (esp_http_client_open(client, 0) == ESP_OK) {
        int64_t content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200 && content_length > 0 && (size_t)content_length < cap) {
            size_t received = 0;
            int64_t deadline = esp_timer_get_time() + 7000000;
            // TLS 已声明 Content-Length 时也要循环读；一次 read 可能只返回一段。
            // Even with Content-Length, one TLS read may return only part of the body.
            while (received < (size_t)content_length && esp_timer_get_time() < deadline) {
                int read = esp_http_client_read(client, body + received,
                                                (int)((size_t)content_length - received));
                if (read > 0) {
                    received += (size_t)read;
                    continue;
                }
                if (read == 0 && esp_http_client_is_complete_data_received(client)) break;
                if (read < 0 && read != -ESP_ERR_HTTP_EAGAIN) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (received == (size_t)content_length) {
                body[received] = 0;
                *length = received;
                ok = true;
            }
        }
    }
    esp_http_client_cleanup(client);
    return ok;
}

static bool apply_online_list(const char* body, size_t length, char* notice, size_t notice_cap) {
    cJSON* root = cJSON_ParseWithLength(body, length);
    if (!root || !cJSON_IsArray(root)) {
        snprintf(notice, notice_cap, "列表解析失败，保留内置列表");
        if (root) cJSON_Delete(root);
        return false;
    }
    // 先在临时表构造完整列表；解析或保存失败时不破坏当前选择。
    // Build the full list off-side; a parse or save failure leaves the current choice untouched.
    mod_dns_entry_t next[MOD_DNS_MAX] = {0};
    next[0] = k_base_entries[0];
    next[1] = k_base_entries[1];
    int filled = 2;
    const cJSON* item;
    cJSON_ArrayForEach(item, root) {
        if (filled >= MOD_DNS_MAX) break;
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON* main_ip = cJSON_GetObjectItemCaseSensitive(item, "main");
        const cJSON* backup_ip = cJSON_GetObjectItemCaseSensitive(item, "backup");
        ip4_addr_t main_addr, backup_addr;
        if (!cJSON_IsString(name) || !cJSON_IsString(main_ip) || !cJSON_IsString(backup_ip) ||
            !name->valuestring || !main_ip->valuestring || !backup_ip->valuestring ||
            !name->valuestring[0] || strlen(name->valuestring) >= sizeof(next[0].name) ||
            !ip4addr_aton(main_ip->valuestring, &main_addr) ||
            !ip4addr_aton(backup_ip->valuestring, &backup_addr) ||
            !main_addr.addr || !backup_addr.addr) continue;
        mod_dns_entry_t* entry = &next[filled++];
        snprintf(entry->name, sizeof(entry->name), "%s", name->valuestring);
        snprintf(entry->main_ip, sizeof(entry->main_ip), "%s", main_ip->valuestring);
        snprintf(entry->backup_ip, sizeof(entry->backup_ip), "%s", backup_ip->valuestring);
    }
    cJSON_Delete(root);
    if (filled > 2) {
        char selected_name[sizeof(s_saved_label)];
        char selected_main[sizeof(s_entries[0].main_ip)];
        char selected_backup[sizeof(s_entries[0].backup_ip)];
        app_settings_dns_choice(selected_name, sizeof(selected_name),
                                selected_main, sizeof(selected_main),
                                selected_backup, sizeof(selected_backup));
        if (!app_settings_dns_pair_known()) {
            int index = mod_dns_saved_index();
            const mod_dns_entry_t *entry = mod_dns_entry(index);
            strlcpy(selected_name, entry->name, sizeof(selected_name));
            strlcpy(selected_main, entry->main_ip, sizeof(selected_main));
            strlcpy(selected_backup, entry->backup_ip, sizeof(selected_backup));
        }
        int selection = 0;
        if (selected_main[0]) {
            selection = -1;
            for (int i = 1; i < filled; ++i) {
                if (!strcmp(next[i].main_ip, selected_main) &&
                    !strcmp(next[i].backup_ip, selected_backup)) {
                    selection = i;
                    break;
                }
            }
            if (selection < 0) {
                // 列表换序或远端删项时保留旧选择；必要时让最后一个在线项腾位。
                // Preserve the old pick across reordering/removal, evicting the last online item if needed.
                if (filled >= MOD_DNS_MAX) filled = MOD_DNS_MAX - 1;
                selection = filled++;
                strlcpy(next[selection].name,
                        selected_name[0] ? selected_name : "自定义 DNS",
                        sizeof(next[selection].name));
                strlcpy(next[selection].main_ip, selected_main, sizeof(next[selection].main_ip));
                strlcpy(next[selection].backup_ip, selected_backup, sizeof(next[selection].backup_ip));
            }
        }
        const mod_dns_entry_t *selected = &next[selection];
        esp_err_t save_err = app_settings_set_dns_choice((uint8_t)selection, selected->name,
                                                          selected->main_ip, selected->backup_ip);
        if (save_err != ESP_OK) {
            snprintf(notice, notice_cap, "保存 DNS 选择失败，保留原列表");
            return false;
        }
        memcpy(s_entries, next, sizeof(s_entries));
        s_count = filled;
        apply_selection(selection);
        snprintf(notice, notice_cap, "已更新 %d 项在线 DNS", filled - 2);
    } else {
        snprintf(notice, notice_cap, "在线列表为空，保留内置列表");
    }
    return notice[0] && strncmp(notice, "已更新", 3) == 0;
}

// 后台只下载，不持有 WiFi 服务生命周期；解析、保存和换表仍在 UI 线程完成。
// The worker downloads only and owns no WiFi lifecycle; parsing, persistence and the list swap stay on the UI thread.
static void online_fetch_task(void* arg) {
    (void)arg;
    size_t length = 0;
    bool ok = https_get(PICO_OTA_DNS_URL, s_fetch_body, sizeof(s_fetch_body), &length);
    s_fetch_length = length;
    if (!ok) snprintf(s_fetch_error, sizeof(s_fetch_error), "下载失败，保留原列表");
    __sync_synchronize();
    s_fetch_state = ok ? MOD_DNS_FETCH_DONE : MOD_DNS_FETCH_FAILED;
    vTaskDelete(NULL);
}

bool mod_dns_fetch_online_start(char* notice, size_t notice_cap) {
    if (s_fetch_state == MOD_DNS_FETCH_RUNNING) {
        snprintf(notice, notice_cap, "正在获取，请稍候");
        return false;
    }
    if (s_fetch_state == MOD_DNS_FETCH_DONE || s_fetch_state == MOD_DNS_FETCH_FAILED) {
        snprintf(notice, notice_cap, "上次结果待处理，请稍候");
        return false;
    }
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (!(status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready)) {
        snprintf(notice, notice_cap, "请先连接 WiFi 再更新列表");
        return false;
    }
    s_fetch_body[0] = 0;
    s_fetch_length = 0;
    s_fetch_error[0] = 0;
    __sync_synchronize();
    s_fetch_state = MOD_DNS_FETCH_RUNNING;
    if (xTaskCreate(online_fetch_task, "dns_list", 12288, NULL, 3, NULL) != pdPASS) {
        s_fetch_state = MOD_DNS_FETCH_IDLE;
        snprintf(notice, notice_cap, "无法启动更新任务");
        return false;
    }
    snprintf(notice, notice_cap, "正在联网获取 DNS 列表…");
    return true;
}

mod_dns_fetch_state_t mod_dns_fetch_online_poll(char* notice, size_t notice_cap) {
    mod_dns_fetch_state_t state = s_fetch_state;
    if (state == MOD_DNS_FETCH_IDLE || state == MOD_DNS_FETCH_RUNNING) return state;
    __sync_synchronize();
    if (state == MOD_DNS_FETCH_FAILED) {
        snprintf(notice, notice_cap, "%s", s_fetch_error[0] ? s_fetch_error : "更新失败，保留原列表");
        s_fetch_state = MOD_DNS_FETCH_IDLE;
        return MOD_DNS_FETCH_FAILED;
    }
    bool ok = apply_online_list(s_fetch_body, s_fetch_length, notice, notice_cap);
    s_fetch_state = MOD_DNS_FETCH_IDLE;
    return ok ? MOD_DNS_FETCH_DONE : MOD_DNS_FETCH_FAILED;
}
