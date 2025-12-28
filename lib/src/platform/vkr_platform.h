#pragma once

#include "defines.h"
#include "vkr_pch.h"

typedef struct VkrTime {
  int32_t seconds;  // seconds after the minute [0-60]
  int32_t minutes;  // minutes after the hour [0-59]
  int32_t hours;    // hours since midnight [0-23]
  int32_t day;      // day of the month [1-31]
  int32_t month;    // months since January [0-11]
  int32_t year;     // years since 1900
  int32_t weekday;  // days since Sunday [0-6]
  int32_t year_day; // days since January 1 [0-365]
  int32_t is_dst;   // Daylight Savings Time flag
  int32_t gmtoff;   // offset from UTC in seconds
  char *timezone_name;
} VkrTime;

void *vkr_platform_mem_reserve(uint64_t size);

bool32_t vkr_platform_mem_commit(void *ptr, uint64_t size);

void vkr_platform_mem_decommit(void *ptr, uint64_t size);

void vkr_platform_mem_release(void *ptr, uint64_t size);

uint64_t vkr_platform_get_page_size();

uint64_t vkr_platform_get_large_page_size();

uint32_t vkr_platform_get_logical_core_count(void);

void vkr_platform_sleep(uint64_t milliseconds);

float64_t vkr_platform_get_absolute_time();

VkrTime vkr_platform_get_local_time();

void vkr_platform_console_write(const char *message, uint8_t colour);

bool8_t vkr_platform_init();

void vkr_platform_shutdown();
