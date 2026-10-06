#pragma once

#include "defines.h"

/* Local stream sockets (AF_UNIX) for the editor's agent channel, vkr_mcp and
 * the `vkr_bakery serve` daemon. POSIX hosts use <sys/un.h>; Windows 10 1803
 * and later use <afunix.h> through Winsock, which this module starts once.
 *
 * Paths are UTF-8 on every host. Windows AF_UNIX reads `sun_path` as UTF-8,
 * not in the ANSI code page, and accepts '/' separators, so a user directory
 * with non-ASCII names needs no conversion. A path, including its
 * terminator, must fit `sun_path`: 104 bytes on macOS and 108 on Linux and
 * Windows.
 *
 * Every socket is a stream socket. Sends never raise SIGPIPE; an interrupted
 * call retries. A socket file outlives its listener on every host, so the
 * owner removes it after closing the listener. */

typedef uint64_t VkrLocalSocket;

#define VKR_LOCAL_SOCKET_INVALID UINT64_MAX
/* The most sockets one vkr_local_socket_poll call watches. */
#define VKR_LOCAL_SOCKET_POLL_MAX 64u

typedef enum VkrLocalSocketStatus {
  VKR_LOCAL_SOCKET_OK = 0,
  /* A nonblocking socket has nothing to read, no room to send or no client
     to accept, or a send timeout expired. */
  VKR_LOCAL_SOCKET_WOULD_BLOCK,
  /* The peer closed or reset the connection. */
  VKR_LOCAL_SOCKET_CLOSED,
  VKR_LOCAL_SOCKET_ERROR,
} VkrLocalSocketStatus;

typedef enum VkrLocalSocketListenStatus {
  VKR_LOCAL_SOCKET_LISTEN_OK = 0,
  /* The path does not fit `sun_path`. */
  VKR_LOCAL_SOCKET_LISTEN_TOO_LONG,
  /* Another listener answers on the path; its socket file stays. */
  VKR_LOCAL_SOCKET_LISTEN_IN_USE,
  /* Socket creation, bind or listen failed; see
     vkr_local_socket_error_text. */
  VKR_LOCAL_SOCKET_LISTEN_FAILED,
} VkrLocalSocketListenStatus;

typedef struct VkrLocalSocketPoll {
  VkrLocalSocket socket;
  /* Output: data or a connection waits, or the peer hung up or failed. */
  bool8_t readable;
} VkrLocalSocketPoll;

/* True when `path` and its terminator fit `sun_path`. */
bool8_t vkr_local_socket_path_fits(const char *path);

/* Binds and listens on `path` with a blocking listener. A socket file that
 * no listener answers is stale and is replaced; a live one returns
 * VKR_LOCAL_SOCKET_LISTEN_IN_USE. POSIX binds under a 0177 umask and sets
 * mode 0600, so only the owner connects; on Windows the socket file takes the
 * DACL of its directory (vkr_local_socket_user_directory admits only its
 * user). */
VkrLocalSocketListenStatus vkr_local_socket_listen(const char *path,
                                                   int32_t backlog,
                                                   VkrLocalSocket *out_socket);

/* Connects a blocking socket to `path`; false when the path does not fit or
 * no listener answers. */
bool8_t vkr_local_socket_connect(const char *path, VkrLocalSocket *out_socket);

/* Accepts one client. A nonblocking listener without a waiting client returns
 * VKR_LOCAL_SOCKET_WOULD_BLOCK. The client socket blocks on every host. */
VkrLocalSocketStatus vkr_local_socket_accept(VkrLocalSocket listener,
                                             VkrLocalSocket *out_socket);

/* Sends up to `length` bytes and stores how many were sent. */
VkrLocalSocketStatus vkr_local_socket_send(VkrLocalSocket socket,
                                           const void *data, uint64_t length,
                                           uint64_t *out_sent);

/* Sends every byte; false when a send fails, times out or would block. */
bool8_t vkr_local_socket_send_all(VkrLocalSocket socket, const void *data,
                                  uint64_t length);

/* Receives up to `capacity` bytes and stores how many arrived. An orderly
 * close by the peer returns VKR_LOCAL_SOCKET_CLOSED. */
VkrLocalSocketStatus vkr_local_socket_recv(VkrLocalSocket socket, void *buffer,
                                           uint64_t capacity,
                                           uint64_t *out_received);

/* Closes a socket; VKR_LOCAL_SOCKET_INVALID is ignored. */
void vkr_local_socket_close(VkrLocalSocket socket);

bool8_t vkr_local_socket_set_nonblocking(VkrLocalSocket socket);

/* A blocking send that cannot finish within `milliseconds` returns
 * VKR_LOCAL_SOCKET_WOULD_BLOCK. On Windows the socket may then be unusable
 * for further sends. */
bool8_t vkr_local_socket_set_send_timeout_ms(VkrLocalSocket socket,
                                             uint32_t milliseconds);

/* Waits up to `timeout_ms` (negative waits without limit) until one of up to
 * VKR_LOCAL_SOCKET_POLL_MAX sockets is readable and sets each entry's
 * `readable`. Returns the readable count, 0 on timeout or interruption, and
 * -1 on failure. */
int32_t vkr_local_socket_poll(VkrLocalSocketPoll *entries, uint32_t count,
                              int32_t timeout_ms);

/* Two connected blocking sockets, for waking a poll from another thread.
 * Windows connects them through a private listener in the user directory. */
bool8_t vkr_local_socket_pair(VkrLocalSocket out_sockets[2]);

/* Removes a socket file; a missing file succeeds. */
bool8_t vkr_local_socket_remove_path(const char *path);

/* The per-user directory for sockets and other private files, with '/'
 * separators: `<temp>/vkr`, where <temp> is $TMPDIR or /tmp on POSIX and
 * GetTempPathW on Windows. It is created when missing. POSIX creates it with
 * mode 0700 and refuses one another user owns or others can open. Windows
 * creates it owned by this user with a protected DACL that grants only this
 * user, refuses one another account owns, and restores that DACL on one this
 * user owns. */
bool8_t vkr_local_socket_user_directory(char *out, uint64_t capacity);

/* `<user directory>/<name>`; false unless the directory is usable and the
 * path fits `sun_path`. */
bool8_t vkr_local_socket_user_path(const char *name, char *out,
                                   uint64_t capacity);

/* The user's numeric identity for default socket names: the uid on POSIX and
 * the relative identifier (the last subauthority of the account SID) on
 * Windows. */
uint32_t vkr_local_socket_user_id(void);

/* Describes the last failed socket call of this thread. */
void vkr_local_socket_error_text(char *out, uint64_t capacity);
