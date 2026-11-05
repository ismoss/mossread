/* SPDX-License-Identifier: Apache-2.0
 * 中文：原生字体宿主回归的 ui_hanzi 桩：只保证链接，不提供真实汉字补字；
 * 补字覆盖由 tools/ui_hanzi_host_test.py 单独验证。
 * English: ui_hanzi stub for native-font host regressions: it only satisfies the linker;
 * supplement coverage is validated by tools/ui_hanzi_host_test.py instead.
 */
#include "ui_hanzi.h"

bool ui_hanzi_get_cached(ui_hanzi_cache_t *cache, uint32_t cp, uint8_t record[UI_HANZI_RECORD]) {
    (void)cache; (void)cp; (void)record;
    return false;
}

bool ui_hanzi_get_aa_cached(ui_hanzi_cache_t *cache, uint32_t cp, uint8_t record[UI_HANZI_AA_RECORD]) {
    (void)cache; (void)cp; (void)record;
    return false;
}

size_t ui_hanzi_cache_release(ui_hanzi_cache_t *cache) {
    (void)cache;
    return 0;
}

bool ui_hanzi_has(uint32_t cp) {
    (void)cp;
    return false;
}

bool ui_hanzi_get(uint32_t cp, uint8_t record[UI_HANZI_RECORD]) {
    (void)cp; (void)record;
    return false;
}