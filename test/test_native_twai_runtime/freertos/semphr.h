#pragma once
#include "FreeRTOS.h"
using SemaphoreHandle_t=void *;
inline bool fakeMutexBusy=false;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return reinterpret_cast<void *>(1);}
inline int xSemaphoreTake(SemaphoreHandle_t,uint32_t ticks){return fakeMutexBusy && ticks==0 ? pdFALSE : pdTRUE;}
inline void xSemaphoreGive(SemaphoreHandle_t){}
