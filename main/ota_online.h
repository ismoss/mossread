/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：联网更新的版本清单与可取消后台任务。/ English: release metadata and cancellable online update jobs.
 * 冻结：只写空闲应用槽，完整校验后由 UI 确认启动。/ Frozen: only write the inactive app slot; UI commits boot after verification.
 * 用户修订：固件擦写与屏幕推送互斥，进度只追加变化像素；不并行驱动 PSRAM 画面和闪存擦写。
 * User revision: serialize firmware flash operations with display output; append progress changes without concurrent PSRAM scans and flash writes.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define PICO_OTA_LAYOUT "pico-dual-4m-v1"
#define PICO_OTA_BOARD "RDP-G01-W"
#define PICO_OTA_CAPABILITY "PICO_HTTPS_OTA_V1"
// 自建更新源：主地址固定指向自己的 GitHub Pages；官方 fallback 故意留空，
// 官方清单一旦出现更高版本号会把官方整镜像推进来，等于自动顶掉二改。
// Self-hosted feed: the primary URL is fixed to our GitHub Pages; the official fallback is
// intentionally empty, because a higher upstream version would push a stock image that wipes the mods.
#define PICO_OTA_FEED_URL "https://ismoss.github.io/mossread/update.json"
// No automatic upstream fallback: an official image could overwrite the downstream mod.
// Keep this empty until a separately authenticated mod mirror is available.
#define PICO_OTA_FEED_FALLBACK_URL ""
// 自建源上的公共 DNS 列表，供「DNS 设置」页在线获取。/ Public DNS list on the self-hosted origin for the DNS settings page.
#define PICO_OTA_DNS_URL "https://ismoss.github.io/mossread/dns.json"

typedef enum { PICO_UPDATE_IDLE, PICO_UPDATE_CHECKING, PICO_UPDATE_AVAILABLE,
               PICO_UPDATE_LATEST, PICO_UPDATE_DOWNLOADING, PICO_UPDATE_READY,
               PICO_UPDATE_FAILED, PICO_UPDATE_CANCELLED } pico_update_state_t;
typedef struct {
    char version[33], project[33], board[24], layout[32], minimum_base_version[33];
    char url[256], sha256[65], notes[384];
    uint32_t size;
} pico_release_t;
typedef struct {
    pico_update_state_t state;
    pico_release_t release;
    uint32_t revision, received;
    char message[96];
    bool busy;
} pico_update_status_t;

/// 有界解析、型号与版本验证；不接触网络。/ Bounded parsing, board and version validation without networking.
esp_err_t pico_release_parse(const char *json, size_t length, pico_release_t *release);
int pico_version_compare(const char *a, const char *b);
bool pico_release_url_valid(const char *url);
/// 检查或下载唯一后台任务；离页、锁屏先取消并等待。/ Start one worker; cancel and join on exit or lock.
esp_err_t pico_online_check(void);
esp_err_t pico_online_download(void);
bool pico_online_busy(void);
void pico_online_get_status(pico_update_status_t *status);
void pico_online_cancel_join(void);
/// 只置取消标志，不等后台线程结束（界面立刻可用）。/ Set the cancel flag only; never wait for the worker.
void pico_online_cancel(void);
/// 刷屏与 OTA 擦写共用递归锁；未初始化 OTA 时不分配内存。/ Recursive display/OTA exclusion; no allocation before OTA initialization.
bool pico_online_display_begin(void);
void pico_online_display_end(bool held);
/// 已验证镜像切换启动槽；失败不切换。/ Select a verified image; failures keep the existing boot slot.
esp_err_t pico_online_commit(void);
