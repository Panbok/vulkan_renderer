#include "platform/vkr_local_socket.h"

#if !defined(_WIN32)

#include "platform/vkr_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int local_socket_fd(VkrLocalSocket socket) { return (int)socket; }

static bool8_t local_socket_address(const char *path,
                                    struct sockaddr_un *out_address) {
  *out_address = (struct sockaddr_un){.sun_family = AF_UNIX};
  if (!path || strlen(path) >= sizeof(out_address->sun_path)) {
    return false_v;
  }
  snprintf(out_address->sun_path, sizeof(out_address->sun_path), "%s", path);
  return true_v;
}

/* macOS reports a write to a closed peer through SIGPIPE unless the socket
   opts out; Linux opts out per send with MSG_NOSIGNAL. */
static void local_socket_no_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
  const int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)fd;
#endif
}

static VkrLocalSocketStatus local_socket_status(int error) {
  if (error == EAGAIN || error == EWOULDBLOCK) {
    return VKR_LOCAL_SOCKET_WOULD_BLOCK;
  }
  if (error == EPIPE || error == ECONNRESET) {
    return VKR_LOCAL_SOCKET_CLOSED;
  }
  return VKR_LOCAL_SOCKET_ERROR;
}

bool8_t vkr_local_socket_path_fits(const char *path) {
  struct sockaddr_un address;
  return path && strlen(path) < sizeof(address.sun_path);
}

bool8_t vkr_local_socket_connect(const char *path, VkrLocalSocket *out_socket) {
  *out_socket = VKR_LOCAL_SOCKET_INVALID;
  struct sockaddr_un address;
  if (!local_socket_address(path, &address)) {
    return false_v;
  }
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false_v;
  }
  local_socket_no_sigpipe(fd);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    const int error = errno;
    close(fd);
    errno = error;
    return false_v;
  }
  *out_socket = (VkrLocalSocket)fd;
  return true_v;
}

VkrLocalSocketListenStatus vkr_local_socket_listen(const char *path,
                                                   int32_t backlog,
                                                   VkrLocalSocket *out_socket) {
  *out_socket = VKR_LOCAL_SOCKET_INVALID;
  struct sockaddr_un address;
  if (!local_socket_address(path, &address)) {
    return VKR_LOCAL_SOCKET_LISTEN_TOO_LONG;
  }

  /* A socket file nobody answers is stale. */
  VkrLocalSocket live = VKR_LOCAL_SOCKET_INVALID;
  if (vkr_local_socket_connect(path, &live)) {
    vkr_local_socket_close(live);
    return VKR_LOCAL_SOCKET_LISTEN_IN_USE;
  }
  (void)unlink(path);

  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return VKR_LOCAL_SOCKET_LISTEN_FAILED;
  }
  /* Owner-only from creation: bind under a restrictive umask. */
  const mode_t previous = umask(0177);
  const int bound = bind(fd, (struct sockaddr *)&address, sizeof(address));
  umask(previous);
  if (bound != 0 || chmod(path, 0600) != 0 || listen(fd, (int)backlog) != 0) {
    const int error = errno;
    close(fd);
    errno = error;
    return VKR_LOCAL_SOCKET_LISTEN_FAILED;
  }
  *out_socket = (VkrLocalSocket)fd;
  return VKR_LOCAL_SOCKET_LISTEN_OK;
}

VkrLocalSocketStatus vkr_local_socket_accept(VkrLocalSocket listener,
                                             VkrLocalSocket *out_socket) {
  *out_socket = VKR_LOCAL_SOCKET_INVALID;
  int fd = -1;
  do {
    fd = accept(local_socket_fd(listener), NULL, NULL);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    return errno == EAGAIN || errno == EWOULDBLOCK
               ? VKR_LOCAL_SOCKET_WOULD_BLOCK
               : VKR_LOCAL_SOCKET_ERROR;
  }
  local_socket_no_sigpipe(fd);
  /* BSD accept copies O_NONBLOCK from the listener; Linux does not. */
  (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
  *out_socket = (VkrLocalSocket)fd;
  return VKR_LOCAL_SOCKET_OK;
}

VkrLocalSocketStatus vkr_local_socket_send(VkrLocalSocket socket,
                                           const void *data, uint64_t length,
                                           uint64_t *out_sent) {
  *out_sent = 0u;
#if defined(MSG_NOSIGNAL)
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  ssize_t sent = -1;
  do {
    sent = send(local_socket_fd(socket), data, (size_t)length, flags);
  } while (sent < 0 && errno == EINTR);
  if (sent < 0) {
    return local_socket_status(errno);
  }
  *out_sent = (uint64_t)sent;
  return VKR_LOCAL_SOCKET_OK;
}

bool8_t vkr_local_socket_send_all(VkrLocalSocket socket, const void *data,
                                  uint64_t length) {
  const uint8_t *bytes = (const uint8_t *)data;
  while (length) {
    uint64_t sent = 0u;
    if (vkr_local_socket_send(socket, bytes, length, &sent) !=
            VKR_LOCAL_SOCKET_OK ||
        !sent) {
      return false_v;
    }
    bytes += sent;
    length -= sent;
  }
  return true_v;
}

VkrLocalSocketStatus vkr_local_socket_recv(VkrLocalSocket socket, void *buffer,
                                           uint64_t capacity,
                                           uint64_t *out_received) {
  *out_received = 0u;
  ssize_t received = -1;
  do {
    received = recv(local_socket_fd(socket), buffer, (size_t)capacity, 0);
  } while (received < 0 && errno == EINTR);
  if (received < 0) {
    return local_socket_status(errno);
  }
  if (received == 0) {
    return VKR_LOCAL_SOCKET_CLOSED;
  }
  *out_received = (uint64_t)received;
  return VKR_LOCAL_SOCKET_OK;
}

void vkr_local_socket_close(VkrLocalSocket socket) {
  if (socket != VKR_LOCAL_SOCKET_INVALID) {
    close(local_socket_fd(socket));
  }
}

bool8_t vkr_local_socket_set_nonblocking(VkrLocalSocket socket) {
  const int fd = local_socket_fd(socket);
  const int flags = fcntl(fd, F_GETFL);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool8_t vkr_local_socket_set_send_timeout_ms(VkrLocalSocket socket,
                                             uint32_t milliseconds) {
  const struct timeval timeout = {
      .tv_sec = (time_t)(milliseconds / 1000u),
      .tv_usec = (suseconds_t)((milliseconds % 1000u) * 1000u)};
  return setsockopt(local_socket_fd(socket), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                    sizeof(timeout)) == 0;
}

int32_t vkr_local_socket_poll(VkrLocalSocketPoll *entries, uint32_t count,
                              int32_t timeout_ms) {
  if (count > VKR_LOCAL_SOCKET_POLL_MAX) {
    return -1;
  }
  struct pollfd fds[VKR_LOCAL_SOCKET_POLL_MAX];
  for (uint32_t i = 0u; i < count; ++i) {
    fds[i] = (struct pollfd){.fd = local_socket_fd(entries[i].socket),
                             .events = POLLIN};
    entries[i].readable = false_v;
  }
  const int ready = poll(fds, (nfds_t)count, (int)timeout_ms);
  if (ready < 0) {
    /* A signal ends the wait; the caller rechecks its state. */
    return errno == EINTR ? 0 : -1;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    entries[i].readable =
        (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) ? true_v : false_v;
  }
  return (int32_t)ready;
}

bool8_t vkr_local_socket_pair(VkrLocalSocket out_sockets[2]) {
  out_sockets[0] = VKR_LOCAL_SOCKET_INVALID;
  out_sockets[1] = VKR_LOCAL_SOCKET_INVALID;
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return false_v;
  }
  local_socket_no_sigpipe(fds[0]);
  local_socket_no_sigpipe(fds[1]);
  out_sockets[0] = (VkrLocalSocket)fds[0];
  out_sockets[1] = (VkrLocalSocket)fds[1];
  return true_v;
}

bool8_t vkr_local_socket_remove_path(const char *path) {
  return unlink(path) == 0 || errno == ENOENT;
}

bool8_t vkr_local_socket_user_directory(char *out, uint64_t capacity) {
  char temp[512];
  if (!vkr_platform_user_directory(VKR_PLATFORM_USER_TEMP, temp,
                                   sizeof(temp))) {
    return false_v;
  }
  const int written = snprintf(out, capacity, "%s/vkr", temp);
  if (written <= 0 || (uint64_t)written >= capacity) {
    return false_v;
  }
  if (mkdir(out, 0700) != 0 && errno != EEXIST) {
    return false_v;
  }
  struct stat info;
  return lstat(out, &info) == 0 && S_ISDIR(info.st_mode) &&
         info.st_uid == getuid() && (info.st_mode & 0077) == 0;
}

bool8_t vkr_local_socket_user_path(const char *name, char *out,
                                   uint64_t capacity) {
  char directory[512];
  if (!vkr_local_socket_user_directory(directory, sizeof(directory))) {
    return false_v;
  }
  const int written = snprintf(out, capacity, "%s/%s", directory, name);
  return written > 0 && (uint64_t)written < capacity &&
         vkr_local_socket_path_fits(out);
}

uint32_t vkr_local_socket_user_id(void) { return (uint32_t)getuid(); }

void vkr_local_socket_error_text(char *out, uint64_t capacity) {
  snprintf(out, capacity, "%s", strerror(errno));
}

#endif
