#include "threads.h"

#if defined(PLATFORM_WINDOWS)
struct s_Thread {
  HANDLE handle;
  ThreadFunc func;
  void *arg;
  void *result;
};

struct s_Mutex {
  CRITICAL_SECTION section;
};

struct s_CondVar {
  CONDITION_VARIABLE variable;
};

// Windows thread wrapper to handle signature differences
static DWORD WINAPI thread_wrapper(LPVOID param) {
  Thread thread = (Thread)param;
  thread->result = thread->func(thread->arg);
  return 0;
}

bool32_t vkr_thread_create(Arena *arena, Thread *thread, ThreadFunc func,
                           void *arg) {
  *thread =
      arena_alloc(arena, sizeof(struct s_Thread), ARENA_MEMORY_TAG_STRUCT);
  if (*thread == NULL) {
    return false;
  }

  MemZero(*thread, sizeof(struct s_Thread));

  (*thread)->func = func;
  (*thread)->arg = arg;
  (*thread)->result = NULL;

  (*thread)->handle = CreateThread(NULL, 0, thread_wrapper, *thread, 0, NULL);
  if ((*thread)->handle == NULL) {
    return false;
  }

  return true;
}

bool32_t vkr_thread_join(Thread thread) {
  if (thread == NULL || thread->handle == NULL) {
    return false;
  }

  DWORD result = WaitForSingleObject(thread->handle, INFINITE);
  return result == WAIT_OBJECT_0;
}

bool32_t vkr_thread_destroy(Arena *arena, Thread *thread) {
  if (thread == NULL || *thread == NULL) {
    return false;
  }

  bool32_t success = true;
  if ((*thread)->handle != NULL) {
    success = CloseHandle((*thread)->handle);
    (*thread)->handle = NULL;
  }

  MemZero(*thread, sizeof(struct s_Thread));

  // Note: We don't free the arena allocation since arenas use bulk deallocation
  *thread = NULL;
  return success;
}

bool32_t vkr_mutex_create(Arena *arena, Mutex *mutex) {
  *mutex = arena_alloc(arena, sizeof(struct s_Mutex), ARENA_MEMORY_TAG_STRUCT);
  if (*mutex == NULL) {
    return false;
  }

  MemZero(*mutex, sizeof(struct s_Mutex));

  InitializeCriticalSection(&(*mutex)->section);
  return true;
}

bool32_t vkr_mutex_lock(Mutex mutex) {
  if (mutex == NULL) {
    return false;
  }

  EnterCriticalSection(&mutex->section);
  return true;
}

bool32_t vkr_mutex_unlock(Mutex mutex) {
  if (mutex == NULL) {
    return false;
  }

  LeaveCriticalSection(&mutex->section);
  return true;
}

bool32_t vkr_mutex_destroy(Arena *arena, Mutex *mutex) {
  if (mutex == NULL || *mutex == NULL) {
    return false;
  }

  DeleteCriticalSection(&(*mutex)->section);

  MemZero(*mutex, sizeof(struct s_Mutex));

  // Note: We don't free the arena allocation since arenas use bulk deallocation
  *mutex = NULL;
  return true;
}

bool32_t vkr_cond_create(Arena *arena, CondVar *cond) {
  *cond = arena_alloc(arena, sizeof(struct s_CondVar), ARENA_MEMORY_TAG_STRUCT);
  if (*cond == NULL) {
    return false;
  }

  MemZero(*cond, sizeof(struct s_CondVar));

  InitializeConditionVariable(&(*cond)->variable);
  return true;
}

bool32_t vkr_cond_wait(CondVar cond, Mutex mutex) {
  if (cond == NULL || mutex == NULL) {
    return false;
  }

  BOOL result =
      SleepConditionVariableCS(&cond->variable, &mutex->section, INFINITE);
  return result != 0;
}

bool32_t vkr_cond_signal(CondVar cond) {
  if (cond == NULL) {
    return false;
  }

  WakeConditionVariable(&cond->variable);
  return true;
}

bool32_t vkr_cond_destroy(Arena *arena, CondVar *cond) {
  if (cond == NULL || *cond == NULL) {
    return false;
  }

  MemZero(*cond, sizeof(struct s_CondVar));

  // Condition variables on Windows don't need explicit cleanup
  // Note: We don't free the arena allocation since arenas use bulk deallocation
  *cond = NULL;
  return true;
}
#endif