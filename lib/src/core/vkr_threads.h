/**
 * @file vkr_threads.h
 * @brief Platform-agnostic threading interface for creating and managing
 * threads, mutexes, and condition variables.
 *
 * This system provides a common C API for threading operations across different
 * platforms. It uses the allocator interface for thread, mutex, and condition
 * variable structures.
 *
 * Key Features:
 * - **Platform Abstraction:** Common API with platform-specific implementations
 * - **Allocator-backed Memory:** All structures are allocated from provided
 *   allocators
 * - **VkrThread Management:** Create, join, and destroy threads with custom
 * functions
 * - **Synchronization:** Mutexes and condition variables for thread
 * coordination
 *
 * Architecture:
 * - **Opaque Types:** VkrThread, VkrMutex, and VkrCondVar are opaque pointers
 * to platform-specific structures
 * - **Allocator Allocation:** All structures are allocated from allocators for
 * flexible memory management
 * - **Error Handling:** All functions return bool32_t indicating
 * success/failure
 */
#pragma once

#include "defines.h"
#include "memory/vkr_allocator.h"

// NOTE: Consider giving each thread its own allocator if arena isolation is
// required between workers.

/**
 * @brief Function pointer type for thread entry points.
 * @param arg User-provided argument passed to the thread function.
 * @return Pointer to thread result data (can be NULL).
 */
typedef void *(*VkrThreadFunc)(void *);

/** @brief Opaque thread handle. */
typedef struct s_VkrThread *VkrThread;
/** @brief Platform thread identifier. */
typedef uint64_t VkrThreadId;
/** @brief Opaque mutex handle. */
typedef struct s_VkrMutex *VkrMutex;
/** @brief Opaque condition variable handle. */
typedef struct s_VkrCondVar *VkrCondVar;

/**
 * @brief Creates a new thread.
 * @param allocator Allocator to back the thread structure.
 * @param thread Pointer to receive the created thread handle.
 * @param func Function to execute in the new thread.
 * @param arg Argument to pass to the thread function.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_thread_create(VkrAllocator *allocator, VkrThread *thread,
                           VkrThreadFunc func, void *arg);

/**
 * @brief Detaches a thread so resources are reclaimed automatically on exit.
 * @param thread VkrThread to detach.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_thread_detach(VkrThread thread);

/**
 * @brief Attempts to cancel a running thread.
 * @param thread VkrThread to cancel.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_thread_cancel(VkrThread thread);

/**
 * @brief Checks whether the thread is still active.
 * @param thread VkrThread to query.
 * @return true_v if active, false_v otherwise.
 */
bool32_t vkr_thread_is_active(VkrThread thread);

/**
 * @brief Sleeps the calling thread for the given duration.
 * @param milliseconds Duration to sleep in milliseconds.
 */
void vkr_thread_sleep(uint64_t milliseconds);

/**
 * @brief Retrieves the id of the provided thread handle.
 * @param thread VkrThread to query.
 * @return Thread id on success, 0 on failure.
 */
VkrThreadId vkr_thread_get_id(VkrThread thread);

/**
 * @brief Retrieves the id of the calling thread.
 * @return Thread id.
 */
VkrThreadId vkr_thread_current_id(void);

/**
 * @brief Waits for a thread to complete execution.
 * @param thread VkrThread to wait for.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_thread_join(VkrThread thread);

/**
 * @brief Destroys a thread and releases its resources.
 * @param allocator Allocator that was used to create the thread.
 * @param thread VkrThread to destroy.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_thread_destroy(VkrAllocator *allocator, VkrThread *thread);

/**
 * @brief Creates a new mutex.
 * @param allocator Allocator to back the mutex structure.
 * @param mutex Pointer to receive the created mutex handle.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_mutex_create(VkrAllocator *allocator, VkrMutex *mutex);

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
 * @param allocator Allocator that was used to create the mutex.
 * @param mutex VkrMutex to destroy.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_mutex_destroy(VkrAllocator *allocator, VkrMutex *mutex);

/**
 * @brief Creates a new condition variable.
 * @param allocator Allocator to back the condition variable structure.
 * @param cond Pointer to receive the created condition variable handle.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_cond_create(VkrAllocator *allocator, VkrCondVar *cond);

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
 * @param allocator Allocator that was used to create the condition variable.
 * @param cond Condition variable to destroy.
 * @return true_v on success, false_v on failure.
 */
bool32_t vkr_cond_destroy(VkrAllocator *allocator, VkrCondVar *cond);
