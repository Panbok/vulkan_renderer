#include "vkr_depot_service.h"

#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_repository.h"
#include "vkr_depot_sync.h"
#include "vkr_depot_zstd.h"

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

/* `vkr_depot service`: installs, registers and runs the machine's one
 * depot service (ADR-134). Its folder and settings are the library's
 * (vkr_depot_service.h, "The machine's service"); this file adds what only
 * the command does:
 *
 * `install` copies the binary into the folder's `bin/` (an app bundle on
 * macOS, which Local Network privacy needs) and registers it to
 * start at logon: a Task Scheduler task that restarts on failure on
 * Windows, which also asks once (UAC) for a Defender rule bound to the
 * installed binary, and a launchd agent kept alive on macOS. The task and
 * the agent run in the user's session, so the store's process lock
 * (`Local\` on Windows) excludes the service and the user's own commits
 * alike. `run` is what they start: it serves the stores `service.conf`
 * lists, follows changes to it, publishes `service.status`, and syncs each
 * served store with each of its remotes that has an interval
 * (vkr_depot_repository_sync, on a thread of its own). */

#define SERVICE_TASK_NAME "VKR Depot"
#define SERVICE_LAUNCHD_LABEL "com.vkr.depot.service"
#define SERVICE_RULE_NAME "VKR Depot"
/* How often `run` looks for a changed `service.conf`, and publishes what
   `status` reads. */
#define SERVICE_SETTINGS_POLL_MS 1000u
#define SERVICE_PUBLISH_MS 5000u

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

#if defined(__APPLE__)
/* Gives the bundle around the copied binary its Info.plist and an ad hoc
   signature whose designated requirement is its identifier. */
static bool8_t service_bundle_seal(const VkrDepotServicePaths *paths) {
  char bundle[VKR_DEPOT_PATH_CAPACITY];
  char info[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(bundle, sizeof(bundle), paths->folder,
                            VKR_DEPOT_SERVICE_BUNDLE) ||
      !vkr_bakery_path_join(info, sizeof(info), bundle,
                            "Contents/Info.plist")) {
    return false_v;
  }
  static const char text[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\"><dict>\n"
      "  <key>CFBundleIdentifier</key><string>" SERVICE_LAUNCHD_LABEL
      "</string>\n"
      "  <key>CFBundleName</key><string>VKR Depot</string>\n"
      "  <key>CFBundleExecutable</key><string>vkr_depot</string>\n"
      "  <key>CFBundlePackageType</key><string>APPL</string>\n"
      "  <key>CFBundleVersion</key><string>1</string>\n"
      "  <key>LSUIElement</key><true/>\n"
      "  <key>NSLocalNetworkUsageDescription</key><string>Serves this "
      "machine's VKR depot repositories and syncs them with the depot "
      "services of other machines in this network.</string>\n"
      "</dict></plist>\n";
  if (!vkr_bakery_write_file_atomic(info, text, sizeof(text) - 1u)) {
    return false_v;
  }
  /* An ad hoc signature names its code by hash, so each build would be a
     new program to Local Network; a requirement on the identifier alone
     lets a reinstalled build keep the user's answer. */
  const char *sign[] = {"-s",
                        "-",
                        "--force",
                        "--identifier",
                        SERVICE_LAUNCHD_LABEL,
                        "-r=designated => identifier \"" SERVICE_LAUNCHD_LABEL
                        "\"",
                        bundle};
  return service_exec("/usr/bin/codesign", sign, ArrayCount(sign), NULL);
}
#endif

/* Copies this executable into the service's `bin/` (inside its app bundle
   on macOS). */
static bool8_t service_copy_binary(const VkrDepotServicePaths *paths) {
  char self[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_executable_path(self, sizeof(self)) ||
      !vkr_bakery_make_directories(paths->bin_folder)) {
    return false_v;
  }
  /* `install` run from the installed copy. */
  VkrBakeryStat self_stat = {0};
  VkrBakeryStat installed_stat = {0};
  if (vkr_bakery_stat(self, &self_stat) &&
      vkr_bakery_stat(paths->binary, &installed_stat) &&
      installed_stat.exists && self_stat.device == installed_stat.device &&
      self_stat.file_id == installed_stat.file_id) {
    return true_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_read_file(self, 1ull << 30, &bytes, &size)) {
    return false_v;
  }
  const bool8_t ok = vkr_bakery_write_file_atomic(paths->binary, bytes, size);
  free(bytes);
#if defined(_WIN32)
  return ok;
#elif defined(__APPLE__)
  return ok && chmod(paths->binary, 0755) == 0 && service_bundle_seal(paths);
#else
  return ok && chmod(paths->binary, 0755) == 0;
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

static bool8_t service_register(const VkrDepotServicePaths *paths) {
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
   administrator may add one. The elevated step first removes every inbound
   rule for the binary: a Block rule, which Defender adds when its "allow
   access" dialog is dismissed, wins over any Allow rule. False, with the
   commands printed, when the user declines or the rule could not be
   added. */
static bool8_t service_firewall(const VkrDepotServicePaths *paths) {
  char rule[64];
  snprintf(rule, sizeof(rule), "name=%s", SERVICE_RULE_NAME);
  const char *show[] = {"advfirewall", "firewall", "show", "rule", rule};
  if (service_exec("netsh.exe", show, ArrayCount(show), NULL)) {
    return true_v;
  }
  /* Defender matches the program by its path in Windows form. */
  char program[VKR_DEPOT_PATH_CAPACITY];
  snprintf(program, sizeof(program), "%s", paths->binary);
  for (char *c = program; *c; ++c) {
    *c = *c == '/' ? '\\' : *c;
  }
  char remove[VKR_DEPOT_PATH_CAPACITY + 128u];
  char add[VKR_DEPOT_PATH_CAPACITY + 256u];
  snprintf(remove, sizeof(remove),
           "advfirewall firewall delete rule name=all dir=in program=\"%s\"",
           program);
  snprintf(add, sizeof(add),
           "advfirewall firewall add rule name=\"%s\" dir=in action=allow "
           "protocol=UDP program=\"%s\" enable=yes profile=any",
           SERVICE_RULE_NAME, program);
  /* The delete fails when there is nothing to delete; the add decides. */
  char parameters[2u * VKR_DEPOT_PATH_CAPACITY + 512u];
  snprintf(parameters, sizeof(parameters), "/c netsh %s & netsh %s", remove,
           add);
  wchar_t wide_parameters[ArrayCount(parameters)];
  if (!MultiByteToWideChar(CP_UTF8, 0, parameters, -1, wide_parameters,
                           (int)ArrayCount(wide_parameters))) {
    return false_v;
  }
  SHELLEXECUTEINFOW info = {
      .cbSize = sizeof(info),
      .fMask = SEE_MASK_NOCLOSEPROCESS,
      .lpVerb = L"runas",
      .lpFile = L"cmd.exe",
      .lpParameters = wide_parameters,
      .nShow = SW_HIDE,
  };
  DWORD exit_code = 1u;
  const bool8_t started = ShellExecuteExW(&info) && info.hProcess;
  const DWORD start_error = started ? 0u : GetLastError();
  if (started) {
    WaitForSingleObject(info.hProcess, 60000u);
    GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);
  }
  if (exit_code != 0u) {
    printf("the firewall rule was not added (%s); as an administrator, "
           "run:\n  netsh %s\n  netsh %s\n",
           start_error == ERROR_CANCELLED ? "the UAC prompt was declined"
           : started                      ? "netsh failed"
                                          : "the UAC prompt did not start",
           remove, add);
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

static bool8_t service_register(const VkrDepotServicePaths *paths) {
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
static bool8_t service_firewall(const VkrDepotServicePaths *paths) {
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

static void service_print_settings(const VkrDepotServicePaths *paths,
                                   const VkrDepotServiceSettings *settings,
                                   bool8_t has_key, const uint8_t key[32]) {
  printf("settings %s\n", paths->conf);
  printf("bind %s, discovery UDP %u%s\n", settings->bind,
         settings->discovery_port,
         settings->outbound_only ? ", outbound only" : "");
  if (has_key) {
    printf("key ");
    service_print_key(key);
    printf("\n");
  } else {
    printf("key: none yet (made when the service first runs)\n");
  }
  for (uint32_t i = 0u; i < settings->store_count; ++i) {
    printf("store %s %s\n", settings->stores[i].name, settings->stores[i].root);
  }
  if (settings->store_count == 0u) {
    printf("no stores: add one with `vkr_depot service add STORE`\n");
  }
}

// =============================================================================
// Interval sync
// =============================================================================

/* One remote of a served store that has an interval, and when it synced. */
typedef struct ServiceSyncEntry {
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  char remote[VKR_DEPOT_REMOTE_NAME];
  uint64_t last_s;
  /* The last round failed: the next failure is not logged again. */
  bool8_t failing;
  bool8_t seen;
} ServiceSyncEntry;

typedef struct ServiceSync {
  const VkrDepotServicePaths *paths;
  VkrAtomicBool stopping;
  VkrThread thread;
  VkrDMemory memory;
  VkrAllocator allocator;
  VkrDepotServiceSettings settings;
  ServiceSyncEntry
      entries[VKR_DEPOT_SERVER_REPOSITORIES_MAX * VKR_DEPOT_REMOTES_MAX];
  uint32_t entry_count;
} ServiceSync;

typedef struct ServiceSyncVisit {
  const char *store_name;
  const char *remote;
} ServiceSyncVisit;

static void service_sync_print(void *context, const char *branch,
                               VkrDepotSyncAction action,
                               const VkrDepotId *commit, const char *detail) {
  static const char *const actions[] = {"same", "pulled", "pushed", "diverged",
                                        "failed"};
  const ServiceSyncVisit *visit = context;
  /* Only what changed or needs a person. */
  if (action == VKR_DEPOT_SYNC_SAME) {
    return;
  }
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(commit, hex);
  printf("sync %s with %s: %s %s %.12s%s%s\n", visit->store_name, visit->remote,
         actions[action], branch, hex, detail[0] ? ": " : "", detail);
  fflush(stdout);
}

static ServiceSyncEntry *service_sync_entry(ServiceSync *sync,
                                            const char *store_root,
                                            const char *remote) {
  for (uint32_t i = 0u; i < sync->entry_count; ++i) {
    ServiceSyncEntry *entry = &sync->entries[i];
    if (strcmp(entry->store_root, store_root) == 0 &&
        strcmp(entry->remote, remote) == 0) {
      return entry;
    }
  }
  if (sync->entry_count == ArrayCount(sync->entries)) {
    return NULL;
  }
  ServiceSyncEntry *entry = &sync->entries[sync->entry_count++];
  MemZero(entry, sizeof(*entry));
  snprintf(entry->store_root, sizeof(entry->store_root), "%s", store_root);
  snprintf(entry->remote, sizeof(entry->remote), "%s", remote);
  return entry;
}

/* Syncs each served store with each of its remotes whose interval has
   passed. The thread reads the settings and the remotes files itself each
   second, so it shares nothing with the service but the stores, whose
   branches move under the store's lock. A sync blocks only this thread. */
static void *service_sync_thread(void *argument) {
  ServiceSync *sync = argument;
  while (!vkr_atomic_bool_load(&sync->stopping, VKR_MEMORY_ORDER_ACQUIRE)) {
    vkr_platform_sleep(SERVICE_SETTINGS_POLL_MS);
    if (!vkr_depot_service_settings_load(sync->paths, &sync->settings)) {
      continue;
    }
    const uint64_t now_s = (uint64_t)vkr_bakery_unix_seconds();
    for (uint32_t i = 0u; i < sync->entry_count; ++i) {
      sync->entries[i].seen = false_v;
    }
    for (uint32_t s = 0u; s < sync->settings.store_count; ++s) {
      const VkrDepotServiceStore *store = &sync->settings.stores[s];
      VkrDepotRemoteEntry remotes[VKR_DEPOT_REMOTES_MAX];
      uint32_t remote_count = 0u;
      if (!vkr_depot_remotes_load(store->root, remotes, ArrayCount(remotes),
                                  &remote_count)) {
        continue;
      }
      for (uint32_t r = 0u; r < remote_count; ++r) {
        if (remotes[r].interval_seconds == 0u ||
            vkr_atomic_bool_load(&sync->stopping, VKR_MEMORY_ORDER_ACQUIRE)) {
          continue;
        }
        ServiceSyncEntry *entry =
            service_sync_entry(sync, store->root, remotes[r].name);
        if (!entry) {
          continue;
        }
        entry->seen = true_v;
        /* The first round after a start syncs at once. */
        if (entry->last_s != 0u &&
            now_s < entry->last_s + remotes[r].interval_seconds) {
          continue;
        }
        entry->last_s = now_s;
        const ServiceSyncVisit visit = {.store_name = store->name,
                                        .remote = remotes[r].name};
        char error[512];
        const bool8_t ok = vkr_depot_repository_sync(
            &sync->allocator, store->root, vkr_depot_zstd_codec(),
            remotes[r].name, service_sync_print, (void *)&visit, NULL, error,
            sizeof(error));
        if (!ok && !entry->failing) {
          printf("sync %s with %s failed: %s\n", store->name, remotes[r].name,
                 error);
          fflush(stdout);
        } else if (ok && entry->failing) {
          printf("sync %s with %s works again\n", store->name, remotes[r].name);
          fflush(stdout);
        }
        entry->failing = !ok;
      }
    }
    /* A store or remote that went away, or lost its interval, leaves. */
    uint32_t kept = 0u;
    for (uint32_t i = 0u; i < sync->entry_count; ++i) {
      if (sync->entries[i].seen) {
        sync->entries[kept++] = sync->entries[i];
      }
    }
    sync->entry_count = kept;
  }
  return NULL;
}

static ServiceSync *service_sync_start(const VkrDepotServicePaths *paths) {
  ServiceSync *sync = calloc(1u, sizeof(ServiceSync));
  if (!sync) {
    return NULL;
  }
  sync->paths = paths;
  vkr_atomic_bool_store(&sync->stopping, false_v, VKR_MEMORY_ORDER_RELEASE);
  /* Its own memory: the sync allocates on this thread only. */
  if (!vkr_dmemory_create(MB(8), GB(1), &sync->memory)) {
    free(sync);
    return NULL;
  }
  sync->allocator = (VkrAllocator){.ctx = &sync->memory};
  vkr_dmemory_allocator_create(&sync->allocator);
  /* A fetch walks trees on the stack; the default thread stack is small. */
  if (!vkr_thread_create_with_stack(service_cmd_allocator(), &sync->thread,
                                    service_sync_thread, sync, MB(8))) {
    vkr_dmemory_allocator_destroy(&sync->allocator);
    free(sync);
    return NULL;
  }
  return sync;
}

/* Waits for a sync in progress to finish. */
static void service_sync_stop(ServiceSync *sync) {
  if (!sync) {
    return;
  }
  vkr_atomic_bool_store(&sync->stopping, true_v, VKR_MEMORY_ORDER_RELEASE);
  (void)vkr_thread_join(sync->thread);
  (void)vkr_thread_destroy(service_cmd_allocator(), &sync->thread);
  vkr_dmemory_allocator_destroy(&sync->allocator);
  free(sync);
}

static int service_run(const VkrDepotServicePaths *paths,
                       VkrDepotServiceSettings *settings, const char *log) {
  /* The log path is UTF-8, as under a non-ASCII profile. */
  if (log && !file_reopen_standard_output(log)) {
    return 1;
  }
  const char *roots[VKR_DEPOT_SERVER_REPOSITORIES_MAX];
  const char *names[VKR_DEPOT_SERVER_REPOSITORIES_MAX];
  for (uint32_t i = 0u; i < settings->store_count; ++i) {
    roots[i] = settings->stores[i].root;
    names[i] = settings->stores[i].name;
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
  if (info->repository_count == 0u) {
    printf("no store yet: `vkr_depot service add STORE` serves one\n");
  }
  if (info->discoverable) {
    printf("discoverable as %s on UDP %u\n", info->name, info->discovery_port);
  } else {
    printf("not discoverable: UDP %u is taken\n", info->discovery_port);
  }
  fflush(stdout);
  /* Without the thread the service still serves; remotes only miss their
     intervals. */
  ServiceSync *sync = service_sync_start(paths);
  if (!sync) {
    fprintf(stderr, "could not start interval sync\n");
  }
  VkrBakeryStat known = {0};
  (void)vkr_bakery_stat(paths->conf, &known);
  (void)vkr_depot_service_publish(service, paths->status);
  uint64_t since_publish_ms = 0u;
  for (;;) {
    vkr_platform_sleep(SERVICE_SETTINGS_POLL_MS);
    vkr_depot_service_info(service, info);
    if (info->failed) {
      fprintf(stderr, "the socket failed\n");
      break;
    }
    /* `service add` or `remove`, from the command or the editor. */
    VkrBakeryStat stat = {0};
    (void)vkr_bakery_stat(paths->conf, &stat);
    if (stat.exists != known.exists || stat.mtime_ns != known.mtime_ns ||
        stat.size != known.size) {
      known = stat;
      if (vkr_depot_service_settings_load(paths, settings)) {
        if (!vkr_depot_service_apply(service, settings, error, sizeof(error))) {
          fprintf(stderr, "%s\n", error);
        }
        vkr_depot_service_info(service, info);
        printf("settings changed: serving %u store%s\n", info->repository_count,
               info->repository_count == 1u ? "" : "s");
        for (uint32_t i = 0u; i < info->repository_count; ++i) {
          printf("serving %s as %s\n", info->repositories[i].store_root,
                 info->repositories[i].name);
        }
        fflush(stdout);
        since_publish_ms = SERVICE_PUBLISH_MS;
      }
    }
    since_publish_ms += SERVICE_SETTINGS_POLL_MS;
    if (since_publish_ms >= SERVICE_PUBLISH_MS) {
      since_publish_ms = 0u;
      (void)vkr_depot_service_publish(service, paths->status);
    }
  }
  free(info);
  service_sync_stop(sync);
  vkr_depot_service_stop(service);
  /* A nonzero exit: Task Scheduler and launchd start it again. */
  return 1;
}

static int service_install(const VkrDepotServicePaths *paths,
                           VkrDepotServiceSettings *settings) {
  if (settings->store_count == 0u) {
    fprintf(stderr, "add a store first: `vkr_depot service add STORE`\n");
    return 1;
  }
  if (!vkr_depot_service_settings_save(paths, settings)) {
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

static int service_status(void) {
  VkrDepotServiceStatus *status = malloc(sizeof(VkrDepotServiceStatus));
  if (!status || !vkr_depot_service_status(status, 1500u)) {
    fprintf(stderr, "could not read the service settings\n");
    free(status);
    return 1;
  }
  service_print_settings(&status->paths, &status->settings, status->has_key,
                         status->key);
  printf("registered to start at logon: %s\n",
         service_registered() ? "yes" : "no");
  if (!status->running) {
    printf("running: no answer on UDP %u\n", status->settings.discovery_port);
    free(status);
    return 1;
  }
  const VkrDepotDiscovered *answer = &status->answer;
  printf("running: %s on port %u%s\n", answer->name, answer->address.port,
         answer->outbound_only ? " (outbound only)" : "");
  for (uint32_t k = 0u; k < answer->repository_count; ++k) {
    printf("  repository %s, default branch %s\n", answer->repositories[k].name,
           answer->repositories[k].default_branch[0]
               ? answer->repositories[k].default_branch
               : "-");
  }
  const uint64_t now_s = (uint64_t)vkr_bakery_unix_seconds();
  for (uint32_t i = 0u; i < status->known_peer_count; ++i) {
    const VkrDepotServerPeer *peer = &status->known_peers[i];
    const uint64_t seen_s = peer->last_seen_us / 1000000u;
    char from[VKR_NET_ADDRESS_TEXT];
    vkr_net_address_format(&peer->address, from, sizeof(from));
    printf("  peer ");
    service_print_key(peer->key);
    printf(" %s from %s, %u connection%s, seen %llu s ago%s\n",
           peer->name[0] ? peer->name : "-", from, peer->connections,
           peer->connections == 1u ? "" : "s",
           (unsigned long long)(now_s > seen_s ? now_s - seen_s : 0u),
           peer->outbound_only ? ", outbound only" : "");
  }
  free(status);
  return 0;
}

int vkr_depot_service_command(int argc, char **argv) {
  if (argc < 1) {
    service_usage();
    return 1;
  }
  VkrDepotServicePaths paths;
  VkrDepotServiceSettings *settings = malloc(sizeof(VkrDepotServiceSettings));
  if (!settings || !vkr_depot_service_paths(&paths) ||
      !vkr_depot_service_settings_load(&paths, settings)) {
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
  char message[VKR_DEPOT_PATH_CAPACITY + 256u];
  if ((strcmp(verb, "add") == 0 || strcmp(verb, "remove") == 0) && positional) {
    const bool8_t ok =
        verb[0] == 'a' ? vkr_depot_service_add(&paths, settings, positional,
                                               name, message, sizeof(message))
                       : vkr_depot_service_remove(&paths, settings, positional,
                                                  message, sizeof(message));
    fprintf(ok ? stdout : stderr, "%s\n", message);
    result = ok ? 0 : 1;
  } else if (strcmp(verb, "list") == 0) {
    VkrDepotServiceStatus *status = malloc(sizeof(VkrDepotServiceStatus));
    if (status && vkr_depot_service_status(status, 0u)) {
      service_print_settings(&paths, settings, status->has_key, status->key);
      result = 0;
    }
    free(status);
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
    result = service_status();
  } else if (strcmp(verb, "run") == 0) {
    result = service_run(&paths, settings, log);
  } else {
    service_usage();
  }
  free(settings);
  return result;
}
