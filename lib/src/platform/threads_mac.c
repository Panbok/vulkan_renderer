#include "threads.h"

#if defined(PLATFORM_APPLE)
struct s_Thread {
  pthread_t handle;
  ThreadFunc func;
  void *arg;
  void *result;
  bool32_t joined;
};

struct s_Mutex {
  pthread_mutex_t mutex;
};

struct s_CondVar {
  pthread_cond_t cond;
};

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
  (*thread)->joined = false;

  int32_t result = pthread_create(&(*thread)->handle, NULL, func, arg);
  if (result != 0) {
    return false;
  }

  return true;
}

bool32_t vkr_thread_join(Thread thread) {
  if (thread == NULL || thread->joined) {
    return false;
  }

  int32_t result = pthread_join(thread->handle, &thread->result);
  if (result == 0) {
    thread->joined = true;
    return true;
  }
  return false;
}

bool32_t vkr_thread_destroy(Arena *arena, Thread *thread) {
  if (thread == NULL || *thread == NULL) {
    return false;
  }

  bool32_t success = true;

  // If thread hasn't been joined, detach it
  if (!(*thread)->joined) {
    int result = pthread_detach((*thread)->handle);
    if (result != 0) {
      success = false;
    }
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

  int32_t result = pthread_mutex_init(&(*mutex)->mutex, NULL);
  return result == 0;
}

bool32_t vkr_mutex_lock(Mutex mutex) {
  if (mutex == NULL) {
    return false;
  }

  int32_t result = pthread_mutex_lock(&mutex->mutex);
  return result == 0;
}

bool32_t vkr_mutex_unlock(Mutex mutex) {
  if (mutex == NULL) {
    return false;
  }

  int32_t result = pthread_mutex_unlock(&mutex->mutex);
  return result == 0;
}

bool32_t vkr_mutex_destroy(Arena *arena, Mutex *mutex) {
  if (mutex == NULL || *mutex == NULL) {
    return false;
  }

  int32_t result = pthread_mutex_destroy(&(*mutex)->mutex);

  MemZero(*mutex, sizeof(struct s_Mutex));

  // Note: We don't free the arena allocation since arenas use bulk deallocation
  *mutex = NULL;
  return result == 0;
}

bool32_t vkr_cond_create(Arena *arena, CondVar *cond) {
  *cond = arena_alloc(arena, sizeof(struct s_CondVar), ARENA_MEMORY_TAG_STRUCT);
  if (*cond == NULL) {
    return false;
  }

  MemZero(*cond, sizeof(struct s_CondVar));

  int32_t result = pthread_cond_init(&(*cond)->cond, NULL);
  return result == 0;
}

bool32_t vkr_cond_wait(CondVar cond, Mutex mutex) {
  if (cond == NULL || mutex == NULL) {
    return false;
  }

  int32_t result = pthread_cond_wait(&cond->cond, &mutex->mutex);
  return result == 0;
}

bool32_t vkr_cond_signal(CondVar cond) {
  if (cond == NULL) {
    return false;
  }

  int32_t result = pthread_cond_signal(&cond->cond);
  return result == 0;
}

bool32_t vkr_cond_destroy(Arena *arena, CondVar *cond) {
  if (cond == NULL || *cond == NULL) {
    return false;
  }

  int32_t result = pthread_cond_destroy(&(*cond)->cond);

  MemZero(*cond, sizeof(struct s_CondVar));

  // Note: We don't free the arena allocation since arenas use bulk deallocation
  *cond = NULL;
  return result == 0;
}
#endif