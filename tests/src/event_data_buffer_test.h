#pragma once

#include "core/event_data_buffer.h"
#include "core/logger.h"
#include "pch.h"
#include "platform/vkr_platform.h"

// Test function declarations
void test_event_data_buffer_create_destroy(void);
void test_event_data_buffer_alloc_simple(void);
void test_event_data_buffer_alloc_zero_size(void);
void test_event_data_buffer_alloc_full(void);
void test_event_data_buffer_alloc_wrap_around(void);
void test_event_data_buffer_alloc_fragmented(void);
void test_event_data_buffer_free_simple(void);
void test_event_data_buffer_free_empty_buffer(void);
void test_event_data_buffer_free_consistency_checks(void);
void test_event_data_buffer_multiple_alloc_free(void);
void test_event_data_buffer_rollback_simple(void);
void test_event_data_buffer_rollback_to_empty(void);
void test_event_data_buffer_rollback_no_alloc(void);
void test_event_data_buffer_complex_interleave(void); // Alloc, free, rollback

bool32_t run_event_data_buffer_tests(void);
