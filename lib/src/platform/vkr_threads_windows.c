#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "platform/vkr_platform.h"

#if defined(PLATFORM_WINDOWS)
struct s_VkrThread {
  HANDLE handle;
  VkrThreadFunc func;
  void *arg;
  void *result;
  bool32_t joined;
  bool32_t detached;
  VkrAtomicBool cancel_requested;
  VkrAtomicBool active;
  VkrThreadId id;
};

struct s_VkrMutex {
  CRITICAL_SECTION section;
};

struct s_VkrCondVar {
  CONDITION_VARIABLE variable;
};

// Atomically read the thread's active flag.
vkr_internal inline bool32_t thread_active_read(VkrThread thread) {
  return vkr_atomic_bool_load(&thread->active, VKR_MEMORY_ORDER_ACQUIRE);
}

// Windows thread wrapper to handle signature differences and bookkeeping.
vkr_internal DWORD WINAPI thread_wrapper(LPVOID param) {
  VkrThread thread = (VkrThread)param;
  if (thread == NULL || thread->func == NULL) {
    return 0;
  }

  // Exit early if a cancellation request was issued before the thread ran.
  if (vkr_atomic_bool_load(&thread->cancel_requested, VKR_MEMORY_ORDER_ACQUIRE)) {
    vkr_atomic_bool_store(&thread->active, false_v, VKR_MEMORY_ORDER_RELEASE);
    return 0;
  }

  thread->result = thread->func(thread->arg);
  vkr_atomic_bool_store(&thread->active, false_v, VKR_MEMORY_ORDER_RELEASE);
  return 0;
}

bool32_t vkr_thread_create(VkrAllocator *allocator, VkrThread *thread,
                           VkrThreadFunc func, void *arg) {
  if (allocator == NULL || thread == NULL || func == NULL) {
    return false_v;
  }

  *thread = vkr_allocator_alloc(allocator, sizeof(struct s_VkrThread),
                                VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (*thread == NULL) {
    return false_v;
  }

  MemZero(*thread, sizeof(struct s_VkrThread));

  (*thread)->func = func;
  (*thread)->arg = arg;
  (*thread)->result = NULL;
  (*thread)->joined = false_v;
  (*thread)->detached = false_v;
  vkr_atomic_bool_store(&(*thread)->cancel_requested, false_v,
                        VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_bool_store(&(*thread)->active, true_v, VKR_MEMORY_ORDER_RELAXED);
  (*thread)->id = 0;

  (*thread)->handle = CreateThread(NULL, 0, thread_wrapper, *thread, 0, NULL);
  if ((*thread)->handle == NULL) {
    vkr_allocator_free(allocator, *thread, sizeof(struct s_VkrThread),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    *thread = NULL;
    return false_v;
  }

  (*thread)->id = (VkrThreadId)GetThreadId((*thread)->handle);
  return true_v;
}

bool32_t vkr_thread_detach(VkrThread thread) {
  if (thread == NULL || thread->detached || thread->joined) {
    return false_v;
  }

  if (thread->handle != NULL) {
    BOOL closed = CloseHandle(thread->handle);
    if (!closed) {
      return false_v;
    }
    thread->handle = NULL;
  }

  thread->detached = true_v;
  return true_v;
}

bool32_t vkr_thread_cancel(VkrThread thread) {
  if (thread == NULL) {
    return false_v;
  }

  // Cooperative cancellation: thread function must check this flag and exit.
  vkr_atomic_bool_store(&thread->cancel_requested, true_v,
                        VKR_MEMORY_ORDER_RELEASE);
  return true_v;
}

bool32_t vkr_thread_cancel_requested(VkrThread thread) {
  if (thread == NULL) {
    return false_v;
  }

  return vkr_atomic_bool_load(&thread->cancel_requested,
                              VKR_MEMORY_ORDER_ACQUIRE) != 0;
}

bool32_t vkr_thread_is_active(VkrThread thread) {
  if (thread == NULL) {
    return false_v;
  }

  bool32_t active = thread_active_read(thread);
  if (!active) {
    return false_v;
  }

  if (thread->handle == NULL) {
    return active != 0;
  }

  DWORD result = WaitForSingleObject(thread->handle, 0);
  if (result == WAIT_TIMEOUT) {
    return true_v;
  }

  vkr_atomic_bool_store(&thread->active, false_v, VKR_MEMORY_ORDER_RELEASE);
  return false_v;
}

void vkr_thread_sleep(uint64_t milliseconds) {
  vkr_platform_sleep(milliseconds);
}

VkrThreadId vkr_thread_get_id(VkrThread thread) {
  if (thread == NULL) {
    return 0;
  }

  if (thread->id != 0) {
    return thread->id;
  }

  if (thread->handle != NULL) {
    thread->id = (VkrThreadId)GetThreadId(thread->handle);
  }

  return thread->id;
}

VkrThreadId vkr_thread_current_id(void) {
  return (VkrThreadId)GetCurrentThreadId();
}

bool32_t vkr_thread_join(VkrThread thread) {
  if (thread == NULL || thread->joined || thread->detached ||
      thread->handle == NULL) {
    return false_v;
  }

  DWORD result = WaitForSingleObject(thread->handle, INFINITE);
  if (result == WAIT_OBJECT_0) {
    thread->joined = true_v;
    vkr_atomic_bool_store(&thread->active, false_v, VKR_MEMORY_ORDER_RELEASE);
    CloseHandle(thread->handle);
    thread->handle = NULL;
    return true_v;
  }
  return false_v;
}

bool32_t vkr_thread_destroy(VkrAllocator *allocator, VkrThread *thread) {
  if (allocator == NULL || thread == NULL || *thread == NULL) {
    return false_v;
  }

  if (vkr_thread_is_active(*thread)) {
    return false_v;
  }

  bool32_t success = true_v;
  if ((*thread)->handle != NULL) {
    if (!CloseHandle((*thread)->handle)) {
      success = false_v;
    }
    (*thread)->handle = NULL;
  }

  MemZero(*thread, sizeof(struct s_VkrThread));

  vkr_allocator_free(allocator, *thread, sizeof(struct s_VkrThread),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  *thread = NULL;
  return success;
}

bool32_t vkr_mutex_create(VkrAllocator *allocator, VkrMutex *mutex) {
  if (allocator == NULL || mutex == NULL) {
    return false_v;
  }

  *mutex = vkr_allocator_alloc(allocator, sizeof(struct s_VkrMutex),
                               VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (*mutex == NULL) {
    return false_v;
  }

  MemZero(*mutex, sizeof(struct s_VkrMutex));

  InitializeCriticalSection(&(*mutex)->section);
  return true_v;
}

bool32_t vkr_mutex_lock(VkrMutex mutex) {
  if (mutex == NULL) {
    return false_v;
  }

  EnterCriticalSection(&mutex->section);
  return true_v;
}

bool32_t vkr_mutex_unlock(VkrMutex mutex) {
  if (mutex == NULL) {
    return false_v;
  }

  LeaveCriticalSection(&mutex->section);
  return true_v;
}

bool32_t vkr_mutex_destroy(VkrAllocator *allocator, VkrMutex *mutex) {
  if (allocator == NULL || mutex == NULL || *mutex == NULL) {
    return false_v;
  }

  DeleteCriticalSection(&(*mutex)->section);

  MemZero(*mutex, sizeof(struct s_VkrMutex));

  vkr_allocator_free(allocator, *mutex, sizeof(struct s_VkrMutex),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  *mutex = NULL;
  return true_v;
}

bool32_t vkr_cond_create(VkrAllocator *allocator, VkrCondVar *cond) {
  if (allocator == NULL || cond == NULL) {
    return false_v;
  }

  *cond = vkr_allocator_alloc(allocator, sizeof(struct s_VkrCondVar),
                              VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (*cond == NULL) {
    return false_v;
  }

  MemZero(*cond, sizeof(struct s_VkrCondVar));

  InitializeConditionVariable(&(*cond)->variable);
  return true_v;
}

bool32_t vkr_cond_wait(VkrCondVar cond, VkrMutex mutex) {
  if (cond == NULL || mutex == NULL) {
    return false_v;
  }

  BOOL result =
      SleepConditionVariableCS(&cond->variable, &mutex->section, INFINITE);
  return result != 0;
}

bool32_t vkr_cond_signal(VkrCondVar cond) {
  if (cond == NULL) {
    return false_v;
  }

  WakeConditionVariable(&cond->variable);
  return true_v;
}

bool32_t vkr_cond_broadcast(VkrCondVar cond) {
  if (cond == NULL) {
    return false_v;
  }

  WakeAllConditionVariable(&cond->variable);
  return true_v;
}

bool32_t vkr_cond_destroy(VkrAllocator *allocator, VkrCondVar *cond) {
  if (allocator == NULL || cond == NULL || *cond == NULL) {
    return false_v;
  }

  MemZero(*cond, sizeof(struct s_VkrCondVar));

  // Condition variables on Windows don't need explicit cleanup
  vkr_allocator_free(allocator, *cond, sizeof(struct s_VkrCondVar),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  *cond = NULL;
  return true_v;
}
#endif
