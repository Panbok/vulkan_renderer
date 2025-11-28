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
  bool32_t cancel_requested;
  bool32_t active;
  VkrThreadId id;
};

struct s_VkrMutex {
  CRITICAL_SECTION section;
};

struct s_VkrCondVar {
  CONDITION_VARIABLE variable;
};

// Windows thread wrapper to handle signature differences and bookkeeping.
static DWORD WINAPI thread_wrapper(LPVOID param) {
  VkrThread thread = (VkrThread)param;
  if (thread == NULL || thread->func == NULL) {
    return 0;
  }

  thread->result = thread->func(thread->arg);
  thread->active = false_v;
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
  (*thread)->cancel_requested = false_v;
  (*thread)->active = true_v;
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
  if (thread == NULL || thread->handle == NULL) {
    return false_v;
  }

  BOOL terminated = TerminateThread(thread->handle, 1);
  if (terminated) {
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    thread->handle = NULL;
    thread->cancel_requested = true_v;
    thread->active = false_v;
    thread->joined = true_v;
    return true_v;
  }

  return false_v;
}

bool32_t vkr_thread_is_active(VkrThread thread) {
  if (thread == NULL || !thread->active) {
    return false_v;
  }

  if (thread->handle == NULL) {
    return thread->active;
  }

  DWORD result = WaitForSingleObject(thread->handle, 0);
  if (result == WAIT_TIMEOUT) {
    return true_v;
  }

  thread->active = false_v;
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
  if (thread == NULL || thread->joined || thread->handle == NULL) {
    return false_v;
  }

  DWORD result = WaitForSingleObject(thread->handle, INFINITE);
  if (result == WAIT_OBJECT_0) {
    thread->joined = true_v;
    thread->active = false_v;
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
