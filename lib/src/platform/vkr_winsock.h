#pragma once

#include "defines.h"

/* Windows only: starts Winsock 2 once per process for the local and UDP
 * socket modules (vkr_local_socket_windows.c). False, with
 * WSANOTINITIALISED as the last error, when startup failed. Winsock stays
 * started for the life of the process. */
bool8_t vkr_winsock_ready(void);
