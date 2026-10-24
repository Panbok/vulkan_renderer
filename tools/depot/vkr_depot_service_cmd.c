#include "vkr_depot_service.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_repository.h"
#include "vkr_depot_sync.h"
#include "vkr_depot_zstd.h"
#include "vkr_net_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* `vkr_depot service`: the machine's one depot service (proposal
 * docs/proposals/depot-git-model.md, "Service"). Its folder, the user data
 * folder's `depot/`, holds:
 *
 *   service.conf   bind, name, discovery port, outbound only and the
 *                  served stores, one setting per line
 *   service.key    the service's secret key, one per machine; the first
 *                  store added gives its `server.key`, so remotes that
 *                  name a served store's key stay valid
 *   bin/           the installed copy of vkr_depot the service runs, so a
 *                  rebuild never stops the service or locks its binary
 *   service.log    what `service run` prints when it runs as the service
 *
 * `install` copies the binary and registers it to start at logon: a Task
 * Scheduler task that restarts on failure on Windows, which also asks once
 * (UAC) for a Defender rule bound to the installed binary, and a launchd
 * agent kept alive on macOS. The task and the agent run in the user's
 * session, so the store's process lock (`Local\` on Windows) excludes the
 * service and the user's own commits alike. */

#define SERVICE_TASK_NAME "VKR Depot"
#define SERVICE_LAUNCHD_LABEL "com.vkr.depot.service"
#define SERVICE_RULE_NAME "VKR Depot"
#define SERVICE_DEFAULT_BIND "0.0.0.0:7320"
#define SERVICE_LINE_MAX (VKR_DEPOT_PATH_CAPACITY + 256u)

typedef struct ServiceSettings {
  char bind[128];
  char name[VKR_DEPOT_DISCOVERY_TEXT];
  uint32_t discovery_port;
  bool8_t outbound_only;
  char store_names[VKR_DEPOT_SERVER_REPOSITORIES_MAX][VKR_DEPOT_BRANCH_NAME];
  char stores[VKR_DEPOT_SERVER_REPOSITORIES_MAX][VKR_DEPOT_PATH_CAPACITY];
  uint32_t store_count;
} ServiceSettings;

typedef struct ServicePaths {
  char folder[VKR_DEPOT_PATH_CAPACITY];
  char conf[VKR_DEPOT_PATH_CAPACITY];
  char key[VKR_DEPOT_PATH_CAPACITY];
  char bin_folder[VKR_DEPOT_PATH_CAPACITY];
  char binary[VKR_DEPOT_PATH_CAPACITY];
  char log[VKR_DEPOT_PATH_CAPACITY];
} ServicePaths;

/* Whether two absolute paths name one file: separators either way, and
   without case on Windows, as its file system compares them. */
static bool8_t service_same_path(const char *a, const char *b) {
  for (;; ++a, ++b) {
    char x = *a == '\\' ? '/' : *a;
    char y = *b == '\\' ? '/' : *b;
#if defined(_WIN32)
    x = x >= 'A' && x <= 'Z' ? (char)(x - 'A' + 'a') : x;
    y = y >= 'A' && y <= 'Z' ? (char)(y - 'A' + 'a') : y;
#endif
    if (x != y) {
      return false_v;
    }
    if (!x) {
      return true_v;
    }
  }
}

static VkrAllocator *service_cmd_allocator(void) {
  static VkrDMemory memory;
  static VkrAllocator allocator;
  static bool8_t ready = false_v;
  if (!ready) {
    if (!vkr_dmemory_create(MB(1), MB(64), &memory)) {
      return NULL;
    }
    allocator = (VkrAllocator){.ctx = &memory};
    vkr_dmemory_allocator_create(&allocator);
    ready = true_v;
  }
  return &allocator;
}

static bool8_t service_paths(ServicePaths *out) {
  char data[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_user_data_directory(data, sizeof(data)) ||
      !vkr_bakery_path_join(out->folder, sizeof(out->folder), data, "depot") ||
      !vkr_bakery_path_join(out->conf, sizeof(out->conf), out->folder,
                            "service.conf") ||
      !vkr_bakery_path_join(out->key, sizeof(out->key), out->folder,
                            "service.key") ||
      !vkr_bakery_path_join(out->bin_folder, sizeof(out->bin_folder),
                            out->folder, "bin") ||
      !vkr_bakery_path_join(out->log, sizeof(out->log), out->folder,
                            "service.log")) {
    return false_v;
  }
#if defined(_WIN32)
  return vkr_bakery_path_join(out->binary, sizeof(out->binary), out->bin_folder,
                              "vkr_depot.exe");
#else
  return vkr_bakery_path_join(out->binary, sizeof(out->binary), out->bin_folder,
                              "vkr_depot");
#endif
}

// =============================================================================
// Settings
// =============================================================================

static void service_settings_default(ServiceSettings *settings) {
  MemZero(settings, sizeof(*settings));
  snprintf(settings->bind, sizeof(settings->bind), "%s", SERVICE_DEFAULT_BIND);
  settings->discovery_port = VKR_DEPOT_DISCOVERY_PORT;
}

/* Reads `service.conf`; a missing file gives the defaults. Unknown lines
   are kept out, so a newer setting does not stop an older binary. */
static bool8_t service_settings_load(const ServicePaths *paths,
                                     ServiceSettings *settings) {
  service_settings_default(settings);
  if (!vkr_bakery_is_file(paths->conf)) {
    return true_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_read_file(paths->conf, 1u << 20, &bytes, &size)) {
    return false_v;
  }
  uint64_t start = 0u;
  while (start < size) {
    uint64_t end = start;
    while (end < size && bytes[end] != '\n') {
      ++end;
    }
    char line[SERVICE_LINE_MAX];
    const uint64_t length = Min(end - start, (uint64_t)sizeof(line) - 1u);
    MemCopy(line, bytes + start, length);
    line[length] = '\0';
    while (line[0] && (line[strlen(line) - 1u] == '\r' ||
                       line[strlen(line) - 1u] == ' ')) {
      line[strlen(line) - 1u] = '\0';
    }
    start = end + 1u;
    char *value = strchr(line, ' ');
    if (!value || line[0] == '#') {
      continue;
    }
    *value++ = '\0';
    if (strcmp(line, "bind") == 0) {
      snprintf(settings->bind, sizeof(settings->bind), "%s", value);
    } else if (strcmp(line, "name") == 0) {
      snprintf(settings->name, sizeof(settings->name), "%s", value);
    } else if (strcmp(line, "discovery-port") == 0) {
      const unsigned long port = strtoul(value, NULL, 10);
      settings->discovery_port = port > 0u && port < 65536u
                                     ? (uint32_t)port
                                     : VKR_DEPOT_DISCOVERY_PORT;
    } else if (strcmp(line, "outbound-only") == 0) {
      settings->outbound_only = strcmp(value, "1") == 0;
    } else if (strcmp(line, "store") == 0 &&
               settings->store_count < VKR_DEPOT_SERVER_REPOSITORIES_MAX) {
      /* `store NAME PATH`: the path is the rest of the line. */
      char *path = strchr(value, ' ');
      if (!path) {
        continue;
      }
      *path++ = '\0';
      if (!vkr_depot_branch_valid(value) || !path[0]) {
        continue;
      }
      const uint32_t index = settings->store_count++;
      snprintf(settings->store_names[index],
               sizeof(settings->store_names[index]), "%s", value);
      snprintf(settings->stores[index], sizeof(settings->stores[index]), "%s",
               path);
    }
  }
  free(bytes);
  return true_v;
}

static bool8_t service_settings_save(const ServicePaths *paths,
                                     const ServiceSettings *settings) {
  const uint64_t capacity =
      1024u + (uint64_t)settings->store_count * SERVICE_LINE_MAX;
  char *text = malloc((size_t)capacity);
  if (!text) {
    return false_v;
  }
  int written = snprintf(text, (size_t)capacity,
                         "# vkr_depot service settings; `vkr_depot service` "
                         "writes this file.\n"
                         "bind %s\n"
                         "discovery-port %u\n"
                         "outbound-only %u\n",
                         settings->bind, settings->discovery_port,
                         settings->outbound_only ? 1u : 0u);
  if (settings->name[0]) {
    written += snprintf(text + written, (size_t)capacity - (size_t)written,
                        "name %s\n", settings->name);
  }
  for (uint32_t i = 0u; i < settings->store_count; ++i) {
    written += snprintf(text + written, (size_t)capacity - (size_t)written,
                        "store %s %s\n", settings->store_names[i],
                        settings->stores[i]);
  }
  const bool8_t ok =
      vkr_bakery_make_directories(paths->folder) &&
      vkr_bakery_write_file_atomic(paths->conf, text, (uint64_t)written);
  free(text);
  return ok;
}

static void service_print_key(const uint8_t key[32]) {
  for (uint32_t i = 0u; i < 32u; ++i) {
    printf("%02x", key[i]);
  }
}

// =============================================================================
// Processes
// =============================================================================

/* Runs a program and waits; true for exit code zero. Its output goes to
   `output`, or nowhere when NULL. */
static bool8_t service_exec(const char *executable,
                            const char *const *arguments, uint32_t count,
                            const char *output) {
#if defined(_WIN32)
  const char *discard = "NUL";
#else
  const char *discard = "/dev/null";
#endif
  const VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = count,
      .stdout_path = output ? output : discard,
      .stderr_path = output ? output : discard,
      .timeout_ms = 60000u,
      .hidden = true_v,
  };
  int32_t exit_code = -1;
  bool8_t timed_out = false_v;
  return vkr_platform_process_run(&config, &exit_code, &timed_out) &&
         !timed_out && exit_code == 0;
}

/* Copies this executable into the service's `bin/`. */
static bool8_t service_copy_binary(const ServicePaths *paths) {
  char self[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_executable_path(self, sizeof(self)) ||
      !vkr_bakery_make_directories(paths->bin_folder)) {
    return false_v;
  }
  if (service_same_path(self, paths->binary)) {
    return true_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_read_file(self, 1ull << 30, &bytes, &size)) {
    return false_v;
  }
  const bool8_t ok = vkr_bakery_write_file_atomic(paths->binary, bytes, size);
  free(bytes);
#if !defined(_WIN32)
  return ok && chmod(paths->binary, 0755) == 0;
#else
  return ok;
#endif
}

// =============================================================================
// Registration
// =============================================================================

#if defined(_WIN32)

/* Text with `&`, `<`, `>` and `"` escaped, for the task XML. */
static void service_xml_escape(const char *text, char *out, uint32_t capacity) {
  uint32_t used = 0u;
  for (const char *c = text; *c && used + 7u < capacity; ++c) {
    const char *entity = *c == '&'   ? "&amp;"
                         : *c == '<' ? "&lt;"
                         : *c == '>' ? "&gt;"
                         : *c == '"' ? "&quot;"
                                     : NULL;
    if (entity) {
      const uint32_t length = (uint32_t)strlen(entity);
      MemCopy(out + used, entity, length);
      used += length;
    } else {
      out[used++] = *c;
    }
  }
  out[used] = '\0';
}

/* Writes UTF-8 `text` as UTF-16 with a byte order mark, as Task Scheduler
   reads its XML. */
static bool8_t service_write_utf16(const char *path, const char *text) {
  const int units = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
  if (units <= 0) {
    return false_v;
  }
  wchar_t *wide = malloc(((size_t)units + 1u) * sizeof(wchar_t));
  if (!wide) {
    return false_v;
  }
  wide[0] = 0xfeffu;
  MultiByteToWideChar(CP_UTF8, 0, text, -1, wide + 1, units);
  const bool8_t ok = vkr_bakery_write_file_atomic(
      path, wide, (uint64_t)units * sizeof(wchar_t));
  free(wide);
  return ok;
}

static bool8_t service_register(const ServicePaths *paths) {
  char user[256];
  char domain[256];
  user[0] = '\0';
  domain[0] = '\0';
  (void)vkr_platform_environment_variable("USERNAME", user, sizeof(user));
  (void)vkr_platform_environment_variable("USERDOMAIN", domain, sizeof(domain));
  char account[600];
  snprintf(account, sizeof(account), "%s%s%s", domain, domain[0] ? "\\" : "",
           user);
  char binary[VKR_DEPOT_PATH_CAPACITY * 2u];
  char folder[VKR_DEPOT_PATH_CAPACITY * 2u];
  char log[VKR_DEPOT_PATH_CAPACITY * 2u];
  char escaped_account[1200];
  service_xml_escape(paths->binary, binary, sizeof(binary));
  service_xml_escape(paths->folder, folder, sizeof(folder));
  service_xml_escape(paths->log, log, sizeof(log));
  service_xml_escape(account, escaped_account, sizeof(escaped_account));
  const uint64_t capacity =
      8192u + sizeof(binary) + sizeof(folder) + sizeof(log);
  char *xml = malloc((size_t)capacity);
  if (!xml) {
    return false_v;
  }
  /* At this user's logon, in this user's session, restarted a minute after
     any exit, without a time limit, on battery too. */
  snprintf(
      xml, (size_t)capacity,
      "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
      "<Task version=\"1.2\" "
      "xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
      "  <RegistrationInfo><Description>VKR depot service: vkr_depot "
      "service run</Description></RegistrationInfo>\r\n"
      "  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>%s"
      "</UserId></LogonTrigger></Triggers>\r\n"
      "  <Principals><Principal id=\"Author\"><UserId>%s</UserId>"
      "<LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege"
      "</RunLevel></Principal></Principals>\r\n"
      "  <Settings>\r\n"
      "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
      "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
      "\r\n"
      "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
      "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
      "    <RestartOnFailure><Interval>PT1M</Interval><Count>999</Count>"
      "</RestartOnFailure>\r\n"
      "    <StartWhenAvailable>true</StartWhenAvailable>\r\n"
      "    <Hidden>true</Hidden>\r\n"
      "    <Enabled>true</Enabled>\r\n"
      "  </Settings>\r\n"
      "  <Actions Context=\"Author\"><Exec><Command>\"%s\"</Command>"
      "<Arguments>service run --log \"%s\"</Arguments><WorkingDirectory>"
      "%s</WorkingDirectory></Exec></Actions>\r\n"
      "</Task>\r\n",
      escaped_account, escaped_account, binary, log, folder);
  char xml_path[VKR_DEPOT_PATH_CAPACITY];
  const bool8_t written = vkr_bakery_path_join(xml_path, sizeof(xml_path),
                                               paths->folder, "task.xml") &&
                          service_write_utf16(xml_path, xml);
  free(xml);
  if (!written) {
    return false_v;
  }
  const char *create[] = {"/Create", "/TN",    SERVICE_TASK_NAME,
                          "/XML",    xml_path, "/F"};
  const char *run[] = {"/Run", "/TN", SERVICE_TASK_NAME};
  const bool8_t ok =
      service_exec("schtasks.exe", create, ArrayCount(create), NULL) &&
      service_exec("schtasks.exe", run, ArrayCount(run), NULL);
  (void)vkr_bakery_remove_file(xml_path);
  return ok;
}

static void service_stop_registered(void) {
  const char *end[] = {"/End", "/TN", SERVICE_TASK_NAME};
  (void)service_exec("schtasks.exe", end, ArrayCount(end), NULL);
}

static bool8_t service_unregister(void) {
  service_stop_registered();
  const char *remove[] = {"/Delete", "/TN", SERVICE_TASK_NAME, "/F"};
  return service_exec("schtasks.exe", remove, ArrayCount(remove), NULL);
}

static bool8_t service_registered(void) {
  const char *query[] = {"/Query", "/TN", SERVICE_TASK_NAME};
  return service_exec("schtasks.exe", query, ArrayCount(query), NULL);
}

/* The Defender rule for the installed binary: one UAC prompt, since only an
   administrator may add one. False, with the command printed, when the
   user declines or the rule could not be added. */
static bool8_t service_firewall(const ServicePaths *paths) {
  char rule[64];
  snprintf(rule, sizeof(rule), "name=%s", SERVICE_RULE_NAME);
  const char *show[] = {"advfirewall", "firewall", "show", "rule", rule};
  if (service_exec("netsh.exe", show, ArrayCount(show), NULL)) {
    return true_v;
  }
  char parameters[VKR_DEPOT_PATH_CAPACITY + 256u];
  snprintf(parameters, sizeof(parameters),
           "advfirewall firewall add rule name=\"%s\" dir=in action=allow "
           "protocol=UDP program=\"%s\" enable=yes profile=any",
           SERVICE_RULE_NAME, paths->binary);
  wchar_t wide_parameters[VKR_DEPOT_PATH_CAPACITY + 256u];
  if (!MultiByteToWideChar(CP_UTF8, 0, parameters, -1, wide_parameters,
                           (int)ArrayCount(wide_parameters))) {
    return false_v;
  }
  SHELLEXECUTEINFOW info = {
      .cbSize = sizeof(info),
      .fMask = SEE_MASK_NOCLOSEPROCESS,
      .lpVerb = L"runas",
      .lpFile = L"netsh.exe",
      .lpParameters = wide_parameters,
      .nShow = SW_HIDE,
  };
  DWORD exit_code = 1u;
  if (ShellExecuteExW(&info) && info.hProcess) {
    WaitForSingleObject(info.hProcess, 60000u);
    GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);
  }
  if (exit_code != 0u) {
    printf("the firewall rule was not added; as an administrator, run:\n"
           "  netsh %s\n",
           parameters);
    return false_v;
  }
  return true_v;
}

#else

static bool8_t service_launchd_plist(char *out, uint32_t capacity) {
  const char *home = getenv("HOME");
  return home && home[0] &&
         (uint32_t)snprintf(out, capacity, "%s/Library/LaunchAgents/%s.plist",
                            home, SERVICE_LAUNCHD_LABEL) < capacity;
}

static void service_domain(char *out, uint32_t capacity) {
  snprintf(out, capacity, "gui/%u", (unsigned)getuid());
}

static bool8_t service_register(const ServicePaths *paths) {
  char plist[VKR_DEPOT_PATH_CAPACITY];
  char folder[VKR_DEPOT_PATH_CAPACITY];
  if (!service_launchd_plist(plist, sizeof(plist))) {
    return false_v;
  }
  vkr_bakery_path_parent(folder, sizeof(folder), plist);
  char *text = malloc(16384u);
  if (!text) {
    return false_v;
  }
  /* Started at login and kept alive, in the user's session. */
  const int length = snprintf(
      text, 16384u,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\"><dict>\n"
      "  <key>Label</key><string>%s</string>\n"
      "  <key>ProgramArguments</key><array><string>%s</string>"
      "<string>service</string><string>run</string><string>--log</string>"
      "<string>%s</string></array>\n"
      "  <key>RunAtLoad</key><true/>\n"
      "  <key>KeepAlive</key><true/>\n"
      "  <key>WorkingDirectory</key><string>%s</string>\n"
      "</dict></plist>\n",
      SERVICE_LAUNCHD_LABEL, paths->binary, paths->log, paths->folder);
  const bool8_t written =
      length > 0 && vkr_bakery_make_directories(folder) &&
      vkr_bakery_write_file_atomic(plist, text, (uint64_t)length);
  free(text);
  if (!written) {
    return false_v;
  }
  char domain[64];
  service_domain(domain, sizeof(domain));
  const char *bootout[] = {"bootout", domain, plist};
  (void)service_exec("/bin/launchctl", bootout, ArrayCount(bootout), NULL);
  const char *bootstrap[] = {"bootstrap", domain, plist};
  return service_exec("/bin/launchctl", bootstrap, ArrayCount(bootstrap), NULL);
}

static void service_stop_registered(void) {
  char domain[64];
  char target[128];
  service_domain(domain, sizeof(domain));
  snprintf(target, sizeof(target), "%s/%s", domain, SERVICE_LAUNCHD_LABEL);
  const char *stop[] = {"kill", "SIGTERM", target};
  (void)service_exec("/bin/launchctl", stop, ArrayCount(stop), NULL);
}

static bool8_t service_unregister(void) {
  char plist[VKR_DEPOT_PATH_CAPACITY];
  char domain[64];
  if (!service_launchd_plist(plist, sizeof(plist))) {
    return false_v;
  }
  service_domain(domain, sizeof(domain));
  const char *bootout[] = {"bootout", domain, plist};
  (void)service_exec("/bin/launchctl", bootout, ArrayCount(bootout), NULL);
  return vkr_bakery_remove_file(plist) || !vkr_bakery_is_file(plist);
}

static bool8_t service_registered(void) {
  char plist[VKR_DEPOT_PATH_CAPACITY];
  return service_launchd_plist(plist, sizeof(plist)) &&
         vkr_bakery_is_file(plist);
}

/* macOS asks the user about incoming connections itself; a managed Mac
   drops them, which `--outbound-only` records. */
static bool8_t service_firewall(const ServicePaths *paths) {
  (void)paths;
  return true_v;
}

#endif

// =============================================================================
// Commands
// =============================================================================

static void service_usage(void) {
  fprintf(stderr, "usage:\n"
                  "  vkr_depot service add STORE [--name NAME]\n"
                  "  vkr_depot service remove NAME|STORE\n"
                  "  vkr_depot service list\n"
                  "  vkr_depot service install [--bind ADDR] [--name NAME] "
                  "[--outbound-only]\n"
                  "  vkr_depot service uninstall\n"
                  "  vkr_depot service status\n"
                  "  vkr_depot service run [--log FILE]\n");
}

static int service_add(const ServicePaths *paths, ServiceSettings *settings,
                       const char *store, const char *name) {
  char absolute[VKR_DEPOT_PATH_CAPACITY];
  char objects[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_absolute(store, absolute, sizeof(absolute)) ||
      !vkr_bakery_path_join(objects, sizeof(objects), absolute, "objects") ||
      !vkr_bakery_is_directory(objects)) {
    fprintf(stderr, "%s is not a depot store\n", store);
    return 1;
  }
  const char *label = name ? name : vkr_bakery_path_name(absolute);
  if (!vkr_depot_branch_valid(label)) {
    fprintf(stderr, "invalid store name %s; give one with --name\n", label);
    return 1;
  }
  for (uint32_t i = 0u; i < settings->store_count; ++i) {
    if (service_same_path(settings->stores[i], absolute) ||
        strcmp(settings->store_names[i], label) == 0) {
      fprintf(stderr, "the service already serves %s as %s\n",
              settings->stores[i], settings->store_names[i]);
      return 1;
    }
  }
  if (settings->store_count == VKR_DEPOT_SERVER_REPOSITORIES_MAX) {
    fprintf(stderr, "the service serves at most %u stores\n",
            VKR_DEPOT_SERVER_REPOSITORIES_MAX);
    return 1;
  }
  /* The first store's key becomes the machine's, so remotes that name a
     store this machine served already keep working. */
  char store_key[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_is_file(paths->key) &&
      vkr_bakery_path_join(store_key, sizeof(store_key), absolute,
                           "server.key") &&
      vkr_bakery_is_file(store_key)) {
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    const bool8_t adopted =
        vkr_bakery_read_file(store_key, 64u, &bytes, &size) &&
        size == VKR_NET_KEY_SIZE &&
        vkr_bakery_make_directories(paths->folder) &&
        vkr_bakery_write_private_file_atomic(paths->key, bytes, size);
    if (bytes) {
      vkr_net_random(bytes, size);
    }
    free(bytes);
    if (!adopted) {
      fprintf(stderr, "could not adopt %s's server key\n", absolute);
      return 1;
    }
    printf("the service adopts %s's server key\n", absolute);
  }
  /* Discovery and HELLO name a repository by its ID. */
  uint8_t id[VKR_DEPOT_REPOSITORY_ID_SIZE];
  if (!vkr_depot_repository_id(absolute, id, true_v)) {
    fprintf(stderr, "could not give %s a repository ID\n", absolute);
    return 1;
  }
  const uint32_t index = settings->store_count++;
  snprintf(settings->stores[index], sizeof(settings->stores[index]), "%s",
           absolute);
  snprintf(settings->store_names[index], sizeof(settings->store_names[index]),
           "%s", label);
  if (!service_settings_save(paths, settings)) {
    fprintf(stderr, "could not write %s\n", paths->conf);
    return 1;
  }
  printf("serving %s as %s; a running service serves it after it restarts\n",
         absolute, label);
  return 0;
}

static int service_remove(const ServicePaths *paths, ServiceSettings *settings,
                          const char *which) {
  char absolute[VKR_DEPOT_PATH_CAPACITY];
  const bool8_t path =
      vkr_bakery_path_absolute(which, absolute, sizeof(absolute));
  for (uint32_t i = 0u; i < settings->store_count; ++i) {
    if (strcmp(settings->store_names[i], which) == 0 ||
        (path && service_same_path(settings->stores[i], absolute))) {
      printf("no longer serving %s\n", settings->stores[i]);
      for (uint32_t k = i; k + 1u < settings->store_count; ++k) {
        MemCopy(settings->stores[k], settings->stores[k + 1u],
                sizeof(settings->stores[k]));
        MemCopy(settings->store_names[k], settings->store_names[k + 1u],
                sizeof(settings->store_names[k]));
      }
      settings->store_count -= 1u;
      return service_settings_save(paths, settings) ? 0 : 1;
    }
  }
  fprintf(stderr, "the service serves no %s\n", which);
  return 1;
}

static void service_print_settings(const ServicePaths *paths,
                                   const ServiceSettings *settings) {
  printf("settings %s\n", paths->conf);
  printf("bind %s, discovery UDP %u%s\n", settings->bind,
         settings->discovery_port,
         settings->outbound_only ? ", outbound only" : "");
  VkrNetKeyPair keys;
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (vkr_bakery_is_file(paths->key) &&
      vkr_bakery_read_file(paths->key, 64u, &bytes, &size) &&
      size == VKR_NET_KEY_SIZE) {
    vkr_net_keypair_from_secret(bytes, &keys);
    printf("key ");
    service_print_key(keys.public_key);
    printf("\n");
    vkr_net_random(&keys, sizeof(keys));
  } else {
    printf("key: none yet (made when the service first runs)\n");
  }
  if (bytes) {
    vkr_net_random(bytes, size);
  }
  free(bytes);
  for (uint32_t i = 0u; i < settings->store_count; ++i) {
    printf("store %s %s\n", settings->store_names[i], settings->stores[i]);
  }
  if (settings->store_count == 0u) {
    printf("no stores: add one with `vkr_depot service add STORE`\n");
  }
}

static int service_run(const ServicePaths *paths,
                       const ServiceSettings *settings, const char *log) {
  if (log && (!freopen(log, "a", stdout) || !freopen(log, "a", stderr))) {
    return 1;
  }
  if (settings->store_count == 0u) {
    fprintf(stderr, "the service serves no store; add one with `vkr_depot "
                    "service add STORE`\n");
    return 1;
  }
  const char *roots[VKR_DEPOT_SERVER_REPOSITORIES_MAX];
  const char *names[VKR_DEPOT_SERVER_REPOSITORIES_MAX];
  for (uint32_t i = 0u; i < settings->store_count; ++i) {
    roots[i] = settings->stores[i];
    names[i] = settings->store_names[i];
  }
  /* The machine's service answers discovery with the machine's name. */
  char host[VKR_DEPOT_DISCOVERY_TEXT];
  vkr_depot_host_name(host, sizeof(host));
  const VkrDepotServiceConfig config = {
      .store_roots = roots,
      .store_names = names,
      .store_count = settings->store_count,
      .key_path = paths->key,
      .outbound_only = settings->outbound_only,
      .bind = settings->bind,
      .port_attempts = 1u,
      .name = settings->name[0] ? settings->name : host,
      .discovery_port = (uint16_t)settings->discovery_port,
      .codec = vkr_depot_zstd_codec(),
  };
  char error[512];
  VkrDepotService *service = vkr_depot_service_start(
      service_cmd_allocator(), &config, error, sizeof(error));
  if (!service) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  VkrDepotServiceInfo *info = malloc(sizeof(VkrDepotServiceInfo));
  if (!info) {
    vkr_depot_service_stop(service);
    return 1;
  }
  vkr_depot_service_info(service, info);
  char text[VKR_NET_ADDRESS_TEXT];
  vkr_net_address_format(&info->address, text, sizeof(text));
  printf("service on %s key ", text);
  service_print_key(info->server_key);
  printf("\n");
  for (uint32_t i = 0u; i < info->repository_count; ++i) {
    printf("serving %s as %s\n", info->repositories[i].store_root,
           info->repositories[i].name);
  }
  if (info->discoverable) {
    printf("discoverable as %s on UDP %u\n", info->name, info->discovery_port);
  } else {
    printf("not discoverable: UDP %u is taken\n", info->discovery_port);
  }
  fflush(stdout);
  for (;;) {
    vkr_platform_sleep(500u);
    vkr_depot_service_info(service, info);
    if (info->failed) {
      fprintf(stderr, "the socket failed\n");
      break;
    }
  }
  free(info);
  vkr_depot_service_stop(service);
  /* A nonzero exit: Task Scheduler and launchd start it again. */
  return 1;
}

static int service_install(const ServicePaths *paths,
                           ServiceSettings *settings) {
  if (settings->store_count == 0u) {
    fprintf(stderr, "add a store first: `vkr_depot service add STORE`\n");
    return 1;
  }
  if (!service_settings_save(paths, settings)) {
    fprintf(stderr, "could not write %s\n", paths->conf);
    return 1;
  }
  /* The running service holds the installed binary. */
  service_stop_registered();
  vkr_platform_sleep(1000u);
  if (!service_copy_binary(paths)) {
    fprintf(stderr, "could not copy vkr_depot to %s\n", paths->binary);
    return 1;
  }
  if (settings->outbound_only) {
    /* Clients on this machine say so in their HELLO too. */
    char identity[VKR_DEPOT_PATH_CAPACITY];
    char marker[VKR_DEPOT_PATH_CAPACITY];
    char folder[VKR_DEPOT_PATH_CAPACITY];
    if (vkr_depot_identity_path(identity, sizeof(identity))) {
      vkr_bakery_path_parent(folder, sizeof(folder), identity);
      if (vkr_bakery_path_join(marker, sizeof(marker), folder,
                               "outbound-only")) {
        (void)vkr_bakery_write_file_atomic(marker, "1\n", 2u);
      }
    }
  }
  if (!service_register(paths)) {
    fprintf(stderr, "could not register the service to start at logon\n");
    return 1;
  }
  printf("installed %s; it starts at logon and now\n", paths->binary);
  const bool8_t firewall = settings->outbound_only || service_firewall(paths);
  if (!firewall) {
    printf("other machines cannot reach the service until the rule exists\n");
  }
  return 0;
}

static int service_status(const ServicePaths *paths,
                          const ServiceSettings *settings) {
  service_print_settings(paths, settings);
  printf("registered to start at logon: %s\n",
         service_registered() ? "yes" : "no");
  VkrNetAddress loopback;
  VkrDepotDiscovered found[4];
  const uint32_t count =
      vkr_depot_address("127.0.0.1:0", &loopback)
          ? vkr_depot_discover(&loopback, (uint16_t)settings->discovery_port,
                               1500u, found, ArrayCount(found))
          : 0u;
  if (count == 0u) {
    printf("running: no answer on UDP %u\n", settings->discovery_port);
    return 1;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    printf("running: %s on port %u key ", found[i].name, found[i].address.port);
    service_print_key(found[i].server_key);
    printf("%s\n", found[i].outbound_only ? " (outbound only)" : "");
    for (uint32_t k = 0u; k < found[i].repository_count; ++k) {
      printf("  repository %s, default branch %s\n",
             found[i].repositories[k].name,
             found[i].repositories[k].default_branch[0]
                 ? found[i].repositories[k].default_branch
                 : "-");
    }
  }
  return 0;
}

int vkr_depot_service_command(int argc, char **argv) {
  if (argc < 1) {
    service_usage();
    return 1;
  }
  ServicePaths paths;
  ServiceSettings *settings = malloc(sizeof(ServiceSettings));
  if (!settings || !service_paths(&paths) ||
      !service_settings_load(&paths, settings)) {
    fprintf(stderr, "could not read the service settings\n");
    free(settings);
    return 1;
  }
  const char *verb = argv[0];
  const char *positional = NULL;
  const char *name = NULL;
  const char *log = NULL;
  for (int i = 1; i < argc; ++i) {
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (value && strcmp(argv[i], "--name") == 0) {
      name = value;
      ++i;
    } else if (value && strcmp(argv[i], "--bind") == 0) {
      snprintf(settings->bind, sizeof(settings->bind), "%s", value);
      ++i;
    } else if (value && strcmp(argv[i], "--log") == 0) {
      log = value;
      ++i;
    } else if (strcmp(argv[i], "--outbound-only") == 0) {
      settings->outbound_only = true_v;
    } else if (argv[i][0] != '-' && !positional) {
      positional = argv[i];
    } else {
      service_usage();
      free(settings);
      return 1;
    }
  }
  int result = 1;
  if (strcmp(verb, "add") == 0 && positional) {
    result = service_add(&paths, settings, positional, name);
  } else if (strcmp(verb, "remove") == 0 && positional) {
    result = service_remove(&paths, settings, positional);
  } else if (strcmp(verb, "list") == 0) {
    service_print_settings(&paths, settings);
    result = 0;
  } else if (strcmp(verb, "install") == 0) {
    if (name) {
      snprintf(settings->name, sizeof(settings->name), "%s", name);
    }
    result = service_install(&paths, settings);
  } else if (strcmp(verb, "uninstall") == 0) {
    result = service_unregister() ? 0 : 1;
    printf("%s\n", result == 0 ? "the service no longer starts at logon"
                               : "the service was not registered");
  } else if (strcmp(verb, "status") == 0) {
    result = service_status(&paths, settings);
  } else if (strcmp(verb, "run") == 0) {
    result = service_run(&paths, settings, log);
  } else {
    service_usage();
  }
  free(settings);
  return result;
}
