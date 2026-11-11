/* 中文：调度测试替身。/ English: Scheduler test shim. */
#pragma once
#include "common.h"

/* 只读电池快照的慢节拍入口；宿主测试用空实现。 */
esp_err_t read_pico_pmu_poll_battery(void);
