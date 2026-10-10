#if defined(_WIN32)

/* Winsock 2 must precede windows.h: vkr_pch.h includes windows.h without
   WIN32_LEAN_AND_MEAN, which pulls the conflicting Winsock 1 header. */
// clang-format off
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <aclapi.h>
// clang-format on

#include "platform/vkr_local_socket.h"
#include "platform/vkr_platform.h"
#include "platform/vkr_winsock.h"

#include <stdio.h>
#include <string.h>

/* Covers `<temp>/vkr` and socket paths; longer paths cannot bind anyway. */
#define LOCAL_SOCKET_WIDE_PATH 1024

static INIT_ONCE winsock_startup_once = INIT_ONCE_STATIC_INIT;
static bool8_t winsock_started = false_v;
static volatile LONG local_socket_pair_serial = 0;

static BOOL CALLBACK winsock_startup(PINIT_ONCE once, PVOID parameter,
                                     PVOID *context) {
  (void)once;
  (void)parameter;
  (void)context;
  WSADATA data;
  /* Winsock stays started for the life of the process. */
  winsock_started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  return TRUE;
}

bool8_t vkr_winsock_ready(void) {
  (void)InitOnceExecuteOnce(&winsock_startup_once, winsock_startup, NULL,
                            NULL);
  if (!winsock_started) {
    WSASetLastError(WSANOTINITIALISED);
  }
  return winsock_started;
}

static SOCKET local_socket_handle(VkrLocalSocket socket) {
  return (SOCKET)socket;
}

static bool8_t local_socket_address(const char *path,
                                    struct sockaddr_un *out_address) {
  *out_address = (struct sockaddr_un){.sun_family = AF_UNIX};
  if (!path || strlen(path) >= sizeof(out_address->sun_path)) {
    return false_v;
  }
  snprintf(out_address->sun_path, sizeof(out_address->sun_path), "%s", path);
  return true_v;
}

static bool8_t local_socket_wide(const char *path, wchar_t *out,
                                 int32_t capacity) {
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out,
                             capacity) > 0;
}

static VkrLocalSocketStatus local_socket_status(int error) {
  if (error == WSAEWOULDBLOCK || error == WSAETIMEDOUT) {
    return VKR_LOCAL_SOCKET_WOULD_BLOCK;
  }
  if (error == WSAECONNRESET || error == WSAECONNABORTED ||
      error == WSAESHUTDOWN || error == WSAENOTCONN) {
    return VKR_LOCAL_SOCKET_CLOSED;
  }
  return VKR_LOCAL_SOCKET_ERROR;
}

static bool8_t local_socket_set_blocking(SOCKET handle, bool8_t blocking) {
  u_long nonblocking = blocking ? 0u : 1u;
  return ioctlsocket(handle, FIONBIO, &nonblocking) == 0;
}

bool8_t vkr_local_socket_path_fits(const char *path) {
  struct sockaddr_un address;
  return path && strlen(path) < sizeof(address.sun_path);
}

bool8_t vkr_local_socket_connect(const char *path, VkrLocalSocket *out_socket) {
  *out_socket = VKR_LOCAL_SOCKET_INVALID;
  struct sockaddr_un address;
  if (!local_socket_address(path, &address) || !vkr_winsock_ready()) {
    return false_v;
  }
  const SOCKET handle = socket(AF_UNIX, SOCK_STREAM, 0);
  if (handle == INVALID_SOCKET) {
    return false_v;
  }
  if (connect(handle, (struct sockaddr *)&address, sizeof(address)) != 0) {
    const int error = WSAGetLastError();
    closesocket(handle);
    WSASetLastError(error);
    return false_v;
  }
  *out_socket = (VkrLocalSocket)handle;
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
  if (!vkr_winsock_ready()) {
    return VKR_LOCAL_SOCKET_LISTEN_FAILED;
  }

  /* A socket file nobody answers is stale; bind fails while it exists. */
  VkrLocalSocket live = VKR_LOCAL_SOCKET_INVALID;
  if (vkr_local_socket_connect(path, &live)) {
    vkr_local_socket_close(live);
    return VKR_LOCAL_SOCKET_LISTEN_IN_USE;
  }
  (void)vkr_local_socket_remove_path(path);

  const SOCKET handle = socket(AF_UNIX, SOCK_STREAM, 0);
  if (handle == INVALID_SOCKET) {
    return VKR_LOCAL_SOCKET_LISTEN_FAILED;
  }
  if (bind(handle, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(handle, (int)backlog) != 0) {
    const int error = WSAGetLastError();
    closesocket(handle);
    WSASetLastError(error);
    return VKR_LOCAL_SOCKET_LISTEN_FAILED;
  }
  *out_socket = (VkrLocalSocket)handle;
  return VKR_LOCAL_SOCKET_LISTEN_OK;
}

VkrLocalSocketStatus vkr_local_socket_accept(VkrLocalSocket listener,
                                             VkrLocalSocket *out_socket) {
  *out_socket = VKR_LOCAL_SOCKET_INVALID;
  const SOCKET handle = accept(local_socket_handle(listener), NULL, NULL);
  if (handle == INVALID_SOCKET) {
    return WSAGetLastError() == WSAEWOULDBLOCK ? VKR_LOCAL_SOCKET_WOULD_BLOCK
                                               : VKR_LOCAL_SOCKET_ERROR;
  }
  /* An accepted socket inherits the listener's nonblocking mode. */
  (void)local_socket_set_blocking(handle, true_v);
  *out_socket = (VkrLocalSocket)handle;
  return VKR_LOCAL_SOCKET_OK;
}

VkrLocalSocketStatus vkr_local_socket_send(VkrLocalSocket socket,
                                           const void *data, uint64_t length,
                                           uint64_t *out_sent) {
  *out_sent = 0u;
  const int chunk = (int)Min(length, (uint64_t)INT32_MAX);
  const int sent =
      send(local_socket_handle(socket), (const char *)data, chunk, 0);
  if (sent == SOCKET_ERROR) {
    return local_socket_status(WSAGetLastError());
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
  const int chunk = (int)Min(capacity, (uint64_t)INT32_MAX);
  const int received =
      recv(local_socket_handle(socket), (char *)buffer, chunk, 0);
  if (received == SOCKET_ERROR) {
    return local_socket_status(WSAGetLastError());
  }
  if (received == 0) {
    return VKR_LOCAL_SOCKET_CLOSED;
  }
  *out_received = (uint64_t)received;
  return VKR_LOCAL_SOCKET_OK;
}

void vkr_local_socket_close(VkrLocalSocket socket) {
  if (socket != VKR_LOCAL_SOCKET_INVALID) {
    closesocket(local_socket_handle(socket));
  }
}

bool8_t vkr_local_socket_set_nonblocking(VkrLocalSocket socket) {
  return local_socket_set_blocking(local_socket_handle(socket), false_v);
}

bool8_t vkr_local_socket_set_send_timeout_ms(VkrLocalSocket socket,
                                             uint32_t milliseconds) {
  const DWORD timeout = (DWORD)milliseconds;
  return setsockopt(local_socket_handle(socket), SOL_SOCKET, SO_SNDTIMEO,
                    (const char *)&timeout, sizeof(timeout)) == 0;
}

int32_t vkr_local_socket_poll(VkrLocalSocketPoll *entries, uint32_t count,
                              int32_t timeout_ms) {
  if (count > VKR_LOCAL_SOCKET_POLL_MAX) {
    return -1;
  }
  WSAPOLLFD fds[VKR_LOCAL_SOCKET_POLL_MAX];
  for (uint32_t i = 0u; i < count; ++i) {
    fds[i] = (WSAPOLLFD){.fd = local_socket_handle(entries[i].socket),
                         .events = POLLRDNORM};
    entries[i].readable = false_v;
  }
  const int ready = WSAPoll(fds, (ULONG)count, (INT)timeout_ms);
  if (ready == SOCKET_ERROR) {
    return -1;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    entries[i].readable =
        (fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR)) ? true_v : false_v;
  }
  return (int32_t)ready;
}

/* Windows has no socketpair: a private listener in the user directory
   accepts the second end, then its file goes away. */
bool8_t vkr_local_socket_pair(VkrLocalSocket out_sockets[2]) {
  out_sockets[0] = VKR_LOCAL_SOCKET_INVALID;
  out_sockets[1] = VKR_LOCAL_SOCKET_INVALID;
  char name[96];
  char path[512];
  snprintf(name, sizeof(name), "pair-%u-%ld.sock",
           vkr_platform_get_process_id(),
           (long)InterlockedIncrement(&local_socket_pair_serial));
  if (!vkr_local_socket_user_path(name, path, sizeof(path))) {
    return false_v;
  }
  VkrLocalSocket listener = VKR_LOCAL_SOCKET_INVALID;
  if (vkr_local_socket_listen(path, 1, &listener) !=
      VKR_LOCAL_SOCKET_LISTEN_OK) {
    return false_v;
  }
  bool8_t ok =
      vkr_local_socket_connect(path, &out_sockets[1]) &&
      vkr_local_socket_accept(listener, &out_sockets[0]) == VKR_LOCAL_SOCKET_OK;
  vkr_local_socket_close(listener);
  (void)vkr_local_socket_remove_path(path);
  if (!ok) {
    vkr_local_socket_close(out_sockets[0]);
    vkr_local_socket_close(out_sockets[1]);
    out_sockets[0] = VKR_LOCAL_SOCKET_INVALID;
    out_sockets[1] = VKR_LOCAL_SOCKET_INVALID;
  }
  return ok;
}

bool8_t vkr_local_socket_remove_path(const char *path) {
  wchar_t wide[LOCAL_SOCKET_WIDE_PATH];
  if (!local_socket_wide(path, wide, LOCAL_SOCKET_WIDE_PATH)) {
    return false_v;
  }
  return DeleteFileW(wide) || GetLastError() == ERROR_FILE_NOT_FOUND ||
         GetLastError() == ERROR_PATH_NOT_FOUND;
}

// =============================================================================
// User directory
// =============================================================================

/* The account SID of this process, stored in `storage`. */
static PSID local_socket_user_sid(uint64_t *storage, uint32_t capacity) {
  HANDLE token = NULL;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return NULL;
  }
  DWORD needed = 0u;
  const BOOL queried = GetTokenInformation(
      token, TokenUser, storage, (DWORD)(capacity * sizeof(uint64_t)), &needed);
  CloseHandle(token);
  return queried ? ((TOKEN_USER *)storage)->User.Sid : NULL;
}

/* A protected DACL with one inheritable ACE: full control for `sid`. */
static bool8_t local_socket_owner_acl(PSID sid, PACL acl, uint32_t size) {
  return InitializeAcl(acl, size, ACL_REVISION) &&
         AddAccessAllowedAceEx(acl, ACL_REVISION,
                               OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                               FILE_ALL_ACCESS, sid);
}

/* True when the directory's DACL is protected and grants only `sid`. */
static bool8_t local_socket_owner_only(PSECURITY_DESCRIPTOR descriptor,
                                       PACL dacl, PSID sid) {
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0u;
  if (!dacl || !GetSecurityDescriptorControl(descriptor, &control, &revision) ||
      !(control & SE_DACL_PROTECTED)) {
    return false_v;
  }
  bool8_t granted = false_v;
  for (DWORD i = 0u; i < dacl->AceCount; ++i) {
    ACE_HEADER *header = NULL;
    if (!GetAce(dacl, i, (LPVOID *)&header)) {
      return false_v;
    }
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
      continue;
    }
    ACCESS_ALLOWED_ACE *allowed = (ACCESS_ALLOWED_ACE *)header;
    if (!EqualSid((PSID)&allowed->SidStart, sid)) {
      return false_v;
    }
    granted = true_v;
  }
  return granted;
}

bool8_t vkr_local_socket_user_directory(char *out, uint64_t capacity) {
  char temp[512];
  if (!vkr_platform_user_directory(VKR_PLATFORM_USER_TEMP, temp,
                                   sizeof(temp))) {
    return false_v;
  }
  const int written = snprintf(out, capacity, "%s/vkr", temp);
  wchar_t wide[LOCAL_SOCKET_WIDE_PATH];
  if (written <= 0 || (uint64_t)written >= capacity ||
      !local_socket_wide(out, wide, LOCAL_SOCKET_WIDE_PATH)) {
    return false_v;
  }
  uint64_t sid_storage[32];
  const PSID sid = local_socket_user_sid(sid_storage, ArrayCount(sid_storage));
  uint64_t acl_storage[32];
  PACL acl = (PACL)acl_storage;
  if (!sid || !local_socket_owner_acl(sid, acl, sizeof(acl_storage))) {
    return false_v;
  }

  /* Owner-only from creation. */
  SECURITY_DESCRIPTOR descriptor;
  if (!InitializeSecurityDescriptor(&descriptor,
                                    SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorOwner(&descriptor, sid, FALSE) ||
      !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
      !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED,
                                    SE_DACL_PROTECTED)) {
    return false_v;
  }
  SECURITY_ATTRIBUTES attributes = {.nLength = sizeof(attributes),
                                    .lpSecurityDescriptor = &descriptor};
  if (CreateDirectoryW(wide, &attributes)) {
    return true_v;
  }
  if (GetLastError() != ERROR_ALREADY_EXISTS) {
    return false_v;
  }

  /* An existing directory must be a real directory this user owns. */
  const DWORD kind = GetFileAttributesW(wide);
  if (kind == INVALID_FILE_ATTRIBUTES || !(kind & FILE_ATTRIBUTE_DIRECTORY) ||
      (kind & FILE_ATTRIBUTE_REPARSE_POINT)) {
    return false_v;
  }
  PSID owner = NULL;
  PACL dacl = NULL;
  PSECURITY_DESCRIPTOR existing = NULL;
  if (GetNamedSecurityInfoW(
          wide, SE_FILE_OBJECT,
          OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, NULL,
          &dacl, NULL, &existing) != ERROR_SUCCESS) {
    return false_v;
  }
  bool8_t ok = owner && EqualSid(owner, sid);
  /* A directory from an older build inherited its parent's DACL. */
  if (ok && !local_socket_owner_only(existing, dacl, sid)) {
    ok = SetNamedSecurityInfoW(wide, SE_FILE_OBJECT,
                               DACL_SECURITY_INFORMATION |
                                   PROTECTED_DACL_SECURITY_INFORMATION,
                               NULL, NULL, acl, NULL) == ERROR_SUCCESS;
  }
  LocalFree(existing);
  return ok;
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

uint32_t vkr_local_socket_user_id(void) {
  uint64_t sid_storage[32];
  const PSID sid = local_socket_user_sid(sid_storage, ArrayCount(sid_storage));
  if (!sid || !IsValidSid(sid) || !*GetSidSubAuthorityCount(sid)) {
    return 0u;
  }
  return (uint32_t)*GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1u);
}

void vkr_local_socket_error_text(char *out, uint64_t capacity) {
  const int error = WSAGetLastError();
  wchar_t message[256];
  const DWORD length =
      FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     NULL, (DWORD)error, 0, message, ArrayCount(message), NULL);
  char text[512] = {0};
  if (length) {
    (void)WideCharToMultiByte(CP_UTF8, 0, message, (int)length, text,
                              (int)sizeof(text) - 1, NULL, NULL);
  }
  /* System messages end with a line break and often a period. */
  uint64_t used = strlen(text);
  while (used && (text[used - 1u] == '\n' || text[used - 1u] == '\r' ||
                  text[used - 1u] == ' ' || text[used - 1u] == '.')) {
    text[--used] = '\0';
  }
  snprintf(out, capacity, "%s (WSA %d)", used ? text : "socket error", error);
}

#endif
