#include "core/vkr_threads.h"
#include "platform/vkr_platform.h"

#if defined(PLATFORM_APPLE)
struct s_VkrThread {
  pthread_t handle;
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
  pthread_mutex_t mutex;
};

struct s_VkrCondVar {
  pthread_cond_t cond;
};

// Thread entry wrapper that updates bookkeeping when the user function returns.
vkr_internal void *vkr_thread_entry(void *param) {
  VkrThread thread = (VkrThread)param;
  if (thread == NULL || thread->func == NULL) {
    return NULL;
  }

  if (thread->cancel_requested) {
    thread->active = false_v;
    return NULL;
  }

  void *result = thread->func(thread->arg);
  thread->result = result;
  thread->active = false_v;
  return result;
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
  (*thread)->joined = false_v;
  (*thread)->detached = false_v;
  (*thread)->cancel_requested = false_v;
  (*thread)->active = true_v;
  (*thread)->id = 0;

  int32_t result =
      pthread_create(&(*thread)->handle, NULL, vkr_thread_entry, *thread);
  if (result != 0) {
    vkr_allocator_free(allocator, *thread, sizeof(struct s_VkrThread),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    *thread = NULL;
    return false_v;
  }

  uint64_t tid = 0;
  if (pthread_threadid_np((*thread)->handle, &tid) == 0) {
    (*thread)->id = tid;
  }

  return true_v;
}

bool32_t vkr_thread_detach(VkrThread thread) {
  if (thread == NULL || thread->detached || thread->joined) {
    return false_v;
  }

  int result = pthread_detach(thread->handle);
  if (result == 0) {
    thread->detached = true_v;
    return true_v;
  }

  return false_v;
}

bool32_t vkr_thread_cancel(VkrThread thread) {
  if (thread == NULL) {
    return false_v;
  }

  int result = pthread_cancel(thread->handle);
  if (result == 0) {
    thread->cancel_requested = true_v;
    if (!thread->detached && !thread->joined) {
      pthread_join(thread->handle, &thread->result);
      thread->joined = true_v;
    }
    thread->active = false_v;
    return true_v;
  }

  if (result == ESRCH) {
    thread->active = false_v;
  }

  return false_v;
}

bool32_t vkr_thread_cancel_requested(VkrThread thread) {
  if (thread == NULL) {
    return false_v;
  }

  return thread->cancel_requested;
}

bool32_t vkr_thread_is_active(VkrThread thread) {
  if (thread == NULL || !thread->active) {
    return false_v;
  }

  int kill_result = pthread_kill(thread->handle, 0);
  if (kill_result == 0) {
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

  uint64_t tid = 0;
  if (pthread_threadid_np(thread->handle, &tid) == 0) {
    thread->id = tid;
    return tid;
  }

  return 0;
}

VkrThreadId vkr_thread_current_id(void) {
  uint64_t tid = 0;
  pthread_threadid_np(0, &tid);
  return tid;
}

bool32_t vkr_thread_join(VkrThread thread) {
  if (thread == NULL || thread->joined || thread->detached) {
    return false_v;
  }

  int32_t result = pthread_join(thread->handle, &thread->result);
  if (result == 0) {
    thread->joined = true_v;
    thread->active = false_v;
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

  if (!(*thread)->joined && !(*thread)->detached) {
    int result = pthread_detach((*thread)->handle);
    if (result != 0) {
      success = false_v;
    }
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

  int32_t result = pthread_mutex_init(&(*mutex)->mutex, NULL);
  if (result != 0) {
    vkr_allocator_free(allocator, *mutex, sizeof(struct s_VkrMutex),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    *mutex = NULL;
    return false_v;
  }

  return true_v;
}

bool32_t vkr_mutex_lock(VkrMutex mutex) {
  if (mutex == NULL) {
    return false_v;
  }

  int32_t result = pthread_mutex_lock(&mutex->mutex);
  return result == 0;
}

bool32_t vkr_mutex_unlock(VkrMutex mutex) {
  if (mutex == NULL) {
    return false_v;
  }

  int32_t result = pthread_mutex_unlock(&mutex->mutex);
  return result == 0;
}

bool32_t vkr_mutex_destroy(VkrAllocator *allocator, VkrMutex *mutex) {
  if (allocator == NULL || mutex == NULL || *mutex == NULL) {
    return false_v;
  }

  int32_t result = pthread_mutex_destroy(&(*mutex)->mutex);
  if (result != 0) {
    return false_v;
  }

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

  int32_t result = pthread_cond_init(&(*cond)->cond, NULL);
  if (result != 0) {
    vkr_allocator_free(allocator, *cond, sizeof(struct s_VkrCondVar),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    *cond = NULL;
    return false_v;
  }
  return true_v;
}

bool32_t vkr_cond_wait(VkrCondVar cond, VkrMutex mutex) {
  if (cond == NULL || mutex == NULL) {
    return false_v;
  }

  int32_t result = pthread_cond_wait(&cond->cond, &mutex->mutex);
  return result == 0;
}

bool32_t vkr_cond_signal(VkrCondVar cond) {
  if (cond == NULL) {
    return false_v;
  }

  int32_t result = pthread_cond_signal(&cond->cond);
  return result == 0;
}

bool32_t vkr_cond_destroy(VkrAllocator *allocator, VkrCondVar *cond) {
  if (allocator == NULL || cond == NULL || *cond == NULL) {
    return false_v;
  }

  int32_t result = pthread_cond_destroy(&(*cond)->cond);
  if (result != 0) {
    return false_v;
  }

  MemZero(*cond, sizeof(struct s_VkrCondVar));
  vkr_allocator_free(allocator, *cond, sizeof(struct s_VkrCondVar),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  *cond = NULL;
  return true_v;
}
#endif
