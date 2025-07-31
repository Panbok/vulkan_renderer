/**
 * @file threads.h
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
 * - **Thread Management:** Create, join, and destroy threads with custom
 * functions
 * - **Synchronization:** Mutexes and condition variables for thread
 * coordination
 *
 * Architecture:
 * - **Opaque Types:** Thread, Mutex, and CondVar are opaque pointers to
 * platform-specific structures
 * - **Arena Allocation:** All structures are allocated from arenas for
 * efficient bulk deallocation
 * - **Error Handling:** All functions return bool32_t indicating
 * success/failure
 */
#pragma once

#include "defines.h"
#include "memory/arena.h"

/**
 * @brief Function pointer type for thread entry points.
 * @param arg User-provided argument passed to the thread function.
 * @return Pointer to thread result data (can be NULL).
 */
typedef void *(*ThreadFunc)(void *);

/** @brief Opaque thread handle. */
typedef struct s_Thread *Thread;
/** @brief Opaque mutex handle. */
typedef struct s_Mutex *Mutex;
/** @brief Opaque condition variable handle. */
typedef struct s_CondVar *CondVar;

/**
 * @brief Creates a new thread.
 * @param arena Arena to allocate thread structure from.
 * @param thread Pointer to receive the created thread handle.
 * @param func Function to execute in the new thread.
 * @param arg Argument to pass to the thread function.
 * @return true_v on success, false_v on failure.
 */
bool32_t thread_create(Arena *arena, Thread *thread, ThreadFunc func,
                       void *arg);

/**
 * @brief Waits for a thread to complete execution.
 * @param thread Thread to wait for.
 * @return true_v on success, false_v on failure.
 */
bool32_t thread_join(Thread thread);

/**
 * @brief Destroys a thread and releases its resources.
 * @param arena Arena that was used to create the thread.
 * @param thread Thread to destroy.
 * @return true_v on success, false_v on failure.
 */
bool32_t thread_destroy(Arena *arena, Thread *thread);

/**
 * @brief Creates a new mutex.
 * @param arena Arena to allocate mutex structure from.
 * @param mutex Pointer to receive the created mutex handle.
 * @return true_v on success, false_v on failure.
 */
bool32_t mutex_create(Arena *arena, Mutex *mutex);

/**
 * @brief Locks a mutex, blocking if already locked.
 * @param mutex Mutex to lock.
 * @return true_v on success, false_v on failure.
 */
bool32_t mutex_lock(Mutex mutex);

/**
 * @brief Unlocks a mutex.
 * @param mutex Mutex to unlock.
 * @return true_v on success, false_v on failure.
 */
bool32_t mutex_unlock(Mutex mutex);

/**
 * @brief Destroys a mutex and releases its resources.
 * @param arena Arena that was used to create the mutex.
 * @param mutex Mutex to destroy.
 * @return true_v on success, false_v on failure.
 */
bool32_t mutex_destroy(Arena *arena, Mutex *mutex);

/**
 * @brief Creates a new condition variable.
 * @param arena Arena to allocate condition variable structure from.
 * @param cond Pointer to receive the created condition variable handle.
 * @return true_v on success, false_v on failure.
 */
bool32_t cond_create(Arena *arena, CondVar *cond);

/**
 * @brief Waits on a condition variable, atomically releasing the mutex.
 * @param cond Condition variable to wait on.
 * @param mutex Mutex to release while waiting.
 * @return true_v on success, false_v on failure.
 */
bool32_t cond_wait(CondVar cond, Mutex mutex);

/**
 * @brief Signals a condition variable, waking one waiting thread.
 * @param cond Condition variable to signal.
 * @return true_v on success, false_v on failure.
 */
bool32_t cond_signal(CondVar cond);

/**
 * @brief Destroys a condition variable and releases its resources.
 * @param arena Arena that was used to create the condition variable.
 * @param cond Condition variable to destroy.
 * @return true_v on success, false_v on failure.
 */
bool32_t cond_destroy(Arena *arena, CondVar *cond);
