#include "vkr_threads.h"

#if defined(PLATFORM_WINDOWS)
struct s_VkrThread {
  HANDLE handle;
  VkrThreadFunc func;
  void *arg;
  void *result;
};

struct s_VkrMutex {
  CRITICAL_SECTION section;
};

struct s_VkrCondVar {
  CONDITION_VARIABLE variable;
};

// Windows thread wrapper to handle signature differences
static DWORD WINAPI thread_wrapper(LPVOID param) {
  VkrThread thread = (VkrThread)param;
  thread->result = thread->func(thread->arg);
  return 0;
}

bool32_t vkr_thread_create(Arena *arena, VkrThread *thread, VkrThreadFunc func,
                           void *arg) {
  *thread =
      arena_alloc(arena, sizeof(struct s_VkrThread), ARENA_MEMORY_TAG_STRUCT);
  if (*thread == NULL) {
    return false;
  }

  MemZero(*thread, sizeof(struct s_VkrThread));

  (*thread)->func = func;
  (*thread)->arg = arg;
  (*thread)->result = NULL;

  (*thread)->handle = CreateThread(NULL, 0, thread_wrapper, *thread, 0, NULL);
  if ((*thread)->handle == NULL) {
    return false;
  }

  return true;
}

bool32_t vkr_thread_join(VkrThread thread) {
  if (thread == NULL || thread->handle == NULL) {
    return false;
  }

  DWORD result = WaitForSingleObject(thread->handle, INFINITE);
  return result == WAIT_OBJECT_0;
}

bool32_t vkr_thread_destroy(Arena *arena, VkrThread *thread) {
  if (thread == NULL || *thread == NULL) {
    return false;
  }

  bool32_t success = true;
  if ((*thread)->handle != NULL) {
    success = CloseHandle((*thread)->handle);
    (*thread)->handle = NULL;
  }

  MemZero(*thread, sizeof(struct s_VkrThread));

  // Note: We don't free the arena allocation since arenas use bulk deallocation
  *thread = NULL;
  return success;
}

bool32_t vkr_mutex_create(Arena *arena, VkrMutex *mutex) {
  *mutex =
      arena_alloc(arena, sizeof(struct s_VkrMutex), ARENA_MEMORY_TAG_STRUCT);
  if (*mutex == NULL) {
    return false;
  }

  MemZero(*mutex, sizeof(struct s_VkrMutex));

  InitializeCriticalSection(&(*mutex)->section);
  return true;
}

bool32_t vkr_mutex_lock(VkrMutex mutex) {
  if (mutex == NULL) {
    return false;
  }

  EnterCriticalSection(&mutex->section);
  return true;
}

bool32_t vkr_mutex_unlock(VkrMutex mutex) {
  if (mutex == NULL) {
    return false;
  }

  LeaveCriticalSection(&mutex->section);
  return true;
}

bool32_t vkr_mutex_destroy(Arena *arena, VkrMutex *mutex) {
  if (mutex == NULL || *mutex == NULL) {
    return false;
  }

  DeleteCriticalSection(&(*mutex)->section);

  MemZero(*mutex, sizeof(struct s_VkrMutex));

  // Note: We don't free the arena allocation since arenas use bulk deallocation
  *mutex = NULL;
  return true;
}

bool32_t vkr_cond_create(Arena *arena, VkrCondVar *cond) {
  *cond =
      arena_alloc(arena, sizeof(struct s_VkrCondVar), ARENA_MEMORY_TAG_STRUCT);
  if (*cond == NULL) {
    return false;
  }

  MemZero(*cond, sizeof(struct s_VkrCondVar));

  InitializeConditionVariable(&(*cond)->variable);
  return true;
}

bool32_t vkr_cond_wait(VkrCondVar cond, VkrMutex mutex) {
  if (cond == NULL || mutex == NULL) {
    return false;
  }

  BOOL result =
      SleepConditionVariableCS(&cond->variable, &mutex->section, INFINITE);
  return result != 0;
}

bool32_t vkr_cond_signal(VkrCondVar cond) {
  if (cond == NULL) {
    return false;
  }

  WakeConditionVariable(&cond->variable);
  return true;
}

bool32_t vkr_cond_destroy(Arena *arena, VkrCondVar *cond) {
  if (cond == NULL || *cond == NULL) {
    return false;
  }

  MemZero(*cond, sizeof(struct s_VkrCondVar));

  // Condition variables on Windows don't need explicit cleanup
  // Note: We don't free the arena allocation since arenas use bulk deallocation
  *cond = NULL;
  return true;
}
#endif