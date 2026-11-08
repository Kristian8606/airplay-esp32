#pragma once
#include "FreeRTOS.h"
UBaseType_t uxTaskPriorityGet(void*);
void vTaskPrioritySet(void*,UBaseType_t);
void vTaskDelay(unsigned);
