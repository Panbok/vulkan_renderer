/**
 * @file vkr_threads.h
 * @brief Platform-agnostic threading interface for creating and managing
 * threads, mutexes, and condition variables.
 *
 * This system provides a common C API for threading operations across different
 * platforms. It uses arena-based memory allocation for thread, mutex, and
 * condition variable structures.
 *
 * Key Features:
 * - **Platform Abstraction:** Common API with platform-specific implementations
 * - **Arena Memory Management:** All structures are allocated from provided
 * arenas
 * - **VkrThread Management:** Create, join, and destroy threads with custom
 * functions
 * - **Synchronization:** Mutexes and condition variables for thread
 * coordination
 *
 * Architecture:
 * - **Opaque Types:** VkrThread, VkrMutex, and VkrCondVar are opaque pointers
 * to platform-specific structures
 * - **Arena Allocation:** All structures are allocated from arenas for
 * efficient bulk deallocation
 * - **Error Handling:** All functions return bool32_t indicating
 * success/failure
 */
#pragma once

#include "defines.h"
#include "memory/arena.h"

// NOTE; We should think about re-working the current threading system. So
// that each thread has its own arena and threads communicate with each other
// by copying data between arenas.

/**
 * @brief Function pointer type for thread entry points.
 * @param arg User-provided argument passed to the thread function.
 * @return Pointer to thread result data (can be NULL).
 */
typedef void *(*VkrThreadFunc)(void *);

/** @brief Opaque thread handle. */
typedef struct s_VkrThread *VkrThread;
/** @brief Opaque mutex handle. */
typedef struct s_VkrMutex *VkrMutex;
/** @brief Opaque condition variable handle. */
typedef struct s_VkrCondVar *VkrCondVar;

/**
 * @brief Creates a new thread.
 * @param arena Arena to allocate thread structure from.
 * @param thread Pointer to receive the created thread handle.
 * @param func Function to execute in the new thread.
 * @param arg Argument to pass to the thread function.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_thread_create(Arena *arena, VkrThread *thread, VkrThreadFunc func,
                           void *arg);

/**
 * @brief Waits for a thread to complete execution.
 * @param thread VkrThread to wait for.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_thread_join(VkrThread thread);

/**
 * @brief Destroys a thread and releases its resources.
 * @param arena Arena that was used to create the thread.
 * @param thread VkrThread to destroy.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_thread_destroy(Arena *arena, VkrThread *thread);

/**
 * @brief Creates a new mutex.
 * @param arena Arena to allocate mutex structure from.
 * @param mutex Pointer to receive the created mutex handle.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_mutex_create(Arena *arena, VkrMutex *mutex);

/**
 * @brief Locks a mutex, blocking if already locked.
 * @param mutex VkrMutex to lock.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_mutex_lock(VkrMutex mutex);

/**
 * @brief Unlocks a mutex.
 * @param mutex VkrMutex to unlock.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_mutex_unlock(VkrMutex mutex);

/**
 * @brief Destroys a mutex and releases its resources.
 * @param arena Arena that was used to create the mutex.
 * @param mutex VkrMutex to destroy.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_mutex_destroy(Arena *arena, VkrMutex *mutex);

/**
 * @brief Creates a new condition variable.
 * @param arena Arena to allocate condition variable structure from.
 * @param cond Pointer to receive the created condition variable handle.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_cond_create(Arena *arena, VkrCondVar *cond);

/**
 * @brief Waits on a condition variable, atomically releasing the mutex.
 * @param cond Condition variable to wait on.
 * @param mutex VkrMutex to release while waiting.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_cond_wait(VkrCondVar cond, VkrMutex mutex);

/**
 * @brief Signals a condition variable, waking one waiting thread.
 * @param cond Condition variable to signal.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_cond_signal(VkrCondVar cond);

/**
 * @brief Destroys a condition variable and releases its resources.
 * @param arena Arena that was used to create the condition variable.
 * @param cond Condition variable to destroy.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_cond_destroy(Arena *arena, VkrCondVar *cond);
