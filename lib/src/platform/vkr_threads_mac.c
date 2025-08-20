#include "core/vkr_threads.h"

#if defined(PLATFORM_APPLE)
struct s_VkrThread {
  pthread_t handle;
  VkrThreadFunc func;
  void *arg;
  void *result;
  bool32_t joined;
};

struct s_VkrMutex {
  pthread_mutex_t mutex;
};

struct s_VkrCondVar {
  pthread_cond_t cond;
};

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
  (*thread)->joined = false;

  int32_t result = pthread_create(&(*thread)->handle, NULL, func, arg);
  if (result != 0) {
    return false;
  }

  return true;
}

bool32_t vkr_thread_join(VkrThread thread) {
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

bool32_t vkr_thread_destroy(Arena *arena, VkrThread *thread) {
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

  int32_t result = pthread_mutex_init(&(*mutex)->mutex, NULL);
  return result == 0;
}

bool32_t vkr_mutex_lock(VkrMutex mutex) {
  if (mutex == NULL) {
    return false;
  }

  int32_t result = pthread_mutex_lock(&mutex->mutex);
  return result == 0;
}

bool32_t vkr_mutex_unlock(VkrMutex mutex) {
  if (mutex == NULL) {
    return false;
  }

  int32_t result = pthread_mutex_unlock(&mutex->mutex);
  return result == 0;
}

bool32_t vkr_mutex_destroy(Arena *arena, VkrMutex *mutex) {
  if (mutex == NULL || *mutex == NULL) {
    return false;
  }

  int32_t result = pthread_mutex_destroy(&(*mutex)->mutex);

  MemZero(*mutex, sizeof(struct s_VkrMutex));

  // Note: We don't free the arena allocation since arenas use bulk deallocation
  *mutex = NULL;
  return result == 0;
}

bool32_t vkr_cond_create(Arena *arena, VkrCondVar *cond) {
  *cond =
      arena_alloc(arena, sizeof(struct s_VkrCondVar), ARENA_MEMORY_TAG_STRUCT);
  if (*cond == NULL) {
    return false;
  }

  MemZero(*cond, sizeof(struct s_VkrCondVar));

  int32_t result = pthread_cond_init(&(*cond)->cond, NULL);
  return result == 0;
}

bool32_t vkr_cond_wait(VkrCondVar cond, VkrMutex mutex) {
  if (cond == NULL || mutex == NULL) {
    return false;
  }

  int32_t result = pthread_cond_wait(&cond->cond, &mutex->mutex);
  return result == 0;
}

bool32_t vkr_cond_signal(VkrCondVar cond) {
  if (cond == NULL) {
    return false;
  }

  int32_t result = pthread_cond_signal(&cond->cond);
  return result == 0;
}

bool32_t vkr_cond_destroy(Arena *arena, VkrCondVar *cond) {
  if (cond == NULL || *cond == NULL) {
    return false;
  }

  int32_t result = pthread_cond_destroy(&(*cond)->cond);

  MemZero(*cond, sizeof(struct s_VkrCondVar));

  // Note: We don't free the arena allocation since arenas use bulk deallocation
  *cond = NULL;
  return result == 0;
}
#endif