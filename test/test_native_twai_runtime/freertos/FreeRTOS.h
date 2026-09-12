#pragma once
#include <cstdint>
inline uint32_t fakeMs=0;
inline uint32_t millis() { return fakeMs; }
using TickType_t=uint32_t;
constexpr int pdTRUE=1, pdFALSE=0;
constexpr uint32_t portMAX_DELAY=UINT32_MAX;
#define pdMS_TO_TICKS(x) (x)
inline void vTaskDelay(uint32_t ms) { fakeMs+=ms; }
