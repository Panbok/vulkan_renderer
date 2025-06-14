#pragma once

#include "defines.h"
#include "pch.h"

void *platform_mem_reserve(uint64_t size);

bool32_t platform_mem_commit(void *ptr, uint64_t size);

void platform_mem_decommit(void *ptr, uint64_t size);

void platform_mem_release(void *ptr, uint64_t size);

uint64_t platform_get_page_size();

uint64_t platform_get_large_page_size();

void platform_sleep(uint64_t milliseconds);

float64_t platform_get_absolute_time();
