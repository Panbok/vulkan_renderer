/* vkr_net_bench: measurements for the network transport
 * (docs/proposals/network-protocol.md, phases 0 and 1).
 *
 *   vkr_net_bench crypto [--seconds S]
 *   vkr_net_bench raw-server --bind ADDR
 *   vkr_net_bench raw-client --connect ADDR [--seconds S] [--size B]
 *   vkr_net_bench server --bind ADDR [--secret HEX]
 *   vkr_net_bench client --connect ADDR --key HEX [--mode bulk|rtt]
 *                        [--seconds S] [--size B] [--background]
 *   vkr_net_bench sim [--latency-ms X] [--loss P] [--bandwidth-mb B]
 *                     [--megabytes N] [--mtu N]
 *
 * Every result is one `result` line of key=value pairs on stdout. */

#include "core/vkr_hash.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_entry.h"
#include "platform/vkr_platform.h"
#include "platform/vkr_udp_socket.h"
#include "vkr_net_core.h"
#include "vkr_net_crypto.h"
#include "vkr_net_host.h"
#include "vkr_net_sim.h"

#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_DEFAULT_PORT 7310u
#define BENCH_BULK_CHANNEL 1u
#define BENCH_RTT_CHANNEL 2u
#define BENCH_RTT_SAMPLES 65536u

/* Raw UDP datagram kinds of the baseline. */
#define RAW_SINK 1u
#define RAW_PING 2u
#define RAW_PONG 3u
#define RAW_REPORT 4u
#define RAW_COUNTS 5u

typedef struct BenchOptions {
  const char *bind;
  const char *connect;
  const char *key;
  const char *secret;
  const char *mode;
  float64_t seconds;
  uint32_t size;
  bool8_t background;
  float64_t latency_ms;
  float64_t loss;
  float64_t bandwidth_mb;
  uint32_t megabytes;
  uint32_t mtu;
} BenchOptions;

static float64_t bench_now(void) { return vkr_platform_get_absolute_time(); }

static bool8_t bench_hex(const char *hex, uint8_t *out, uint32_t size) {
  if (!hex || strlen(hex) != (uint64_t)size * 2u) {
    return false_v;
  }
  for (uint32_t i = 0u; i < size * 2u; ++i) {
    const char c = hex[i];
    uint32_t digit = 0u;
    if (c >= '0' && c <= '9') {
      digit = (uint32_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = (uint32_t)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = (uint32_t)(c - 'A' + 10);
    } else {
      return false_v;
    }
    out[i / 2u] = (uint8_t)((i % 2u) ? (out[i / 2u] | digit)
                                       : (digit << 4));
  }
  return true_v;
}

static void bench_print_hex(const uint8_t *bytes, uint32_t size) {
  for (uint32_t i = 0u; i < size; ++i) {
    printf("%02x", bytes[i]);
  }
}

static bool8_t bench_address(const char *text, VkrNetAddress *out) {
  if (vkr_net_address_parse(text, BENCH_DEFAULT_PORT, out)) {
    return true_v;
  }
  /* "host:port" with a name. */
  char host[256];
  const char *colon = strrchr(text, ':');
  uint32_t port = BENCH_DEFAULT_PORT;
  uint64_t length = strlen(text);
  if (colon && !strchr(colon + 1, ']')) {
    port = (uint32_t)strtoul(colon + 1, NULL, 10);
    length = (uint64_t)(colon - text);
  }
  if (length == 0u || length >= sizeof(host) || port > 65535u) {
    return false_v;
  }
  memcpy(host, text, length);
  host[length] = '\0';
  return vkr_net_address_resolve(host, (uint16_t)port, true_v, out);
}

static int bench_compare_u64(const void *a, const void *b) {
  const uint64_t x = *(const uint64_t *)a;
  const uint64_t y = *(const uint64_t *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

// =============================================================================
// crypto: per-core cost of the bytes every transfer pays
// =============================================================================

static int bench_crypto(const BenchOptions *options) {
  static uint8_t buffer[1u << 20];
  uint8_t key[32];
  uint8_t nonce[12] = {0};
  uint8_t tag[16];
  unsigned long long tag_size = 0u;
  randombytes_buf(key, sizeof(key));
  randombytes_buf(buffer, sizeof(buffer));
  crypto_aead_aes256gcm_state state;
  crypto_aead_aes256gcm_beforenm(&state, key);
  const float64_t budget = options->seconds > 0.0 ? options->seconds : 2.0;

  /* AES-256-GCM over transport-sized packets, as the transport seals them. */
  uint64_t bytes = 0u;
  float64_t start = bench_now();
  while (bench_now() - start < budget) {
    for (uint32_t i = 0u; i < 1024u; ++i) {
      nonce[0] = (uint8_t)i;
      crypto_aead_aes256gcm_encrypt_detached_afternm(
          buffer, tag, &tag_size, buffer, 1200u, buffer + 1200u, 7u, NULL,
          nonce, &state);
    }
    bytes += 1024u * 1200u;
  }
  float64_t elapsed = bench_now() - start;
  printf("result test=aes256gcm_1200 mb_s=%.0f packets_s=%.0f\n",
         (float64_t)bytes / elapsed / 1e6, (float64_t)bytes / 1200.0 / elapsed);

  bytes = 0u;
  start = bench_now();
  while (bench_now() - start < budget) {
    crypto_aead_aes256gcm_encrypt_detached_afternm(buffer, tag, &tag_size,
                                                   buffer, sizeof(buffer), NULL,
                                                   0u, NULL, nonce, &state);
    bytes += sizeof(buffer);
  }
  elapsed = bench_now() - start;
  printf("result test=aes256gcm_1mib mb_s=%.0f\n",
         (float64_t)bytes / elapsed / 1e6);

  bytes = 0u;
  start = bench_now();
  while (bench_now() - start < budget) {
    uint8_t digest[VKR_SHA256_DIGEST_SIZE];
    vkr_sha256(buffer, sizeof(buffer), digest);
    bytes += sizeof(buffer);
  }
  elapsed = bench_now() - start;
  printf("result test=sha256_1mib mb_s=%.0f\n",
         (float64_t)bytes / elapsed / 1e6);

  bytes = 0u;
  start = bench_now();
  while (bench_now() - start < budget) {
    crypto_aead_chacha20poly1305_ietf_encrypt_detached(
        buffer, tag, &tag_size, buffer, sizeof(buffer), NULL, 0u, NULL, nonce,
        key);
    bytes += sizeof(buffer);
  }
  elapsed = bench_now() - start;
  printf("result test=chacha20poly1305_1mib mb_s=%.0f\n",
         (float64_t)bytes / elapsed / 1e6);
  return 0;
}

// =============================================================================
// Raw UDP baseline: what the host does without the transport
// =============================================================================

static int bench_raw_server(const BenchOptions *options) {
  VkrNetAddress bind;
  if (!bench_address(options->bind ? options->bind : "[::]", &bind)) {
    fprintf(stderr, "invalid --bind\n");
    return 1;
  }
  VkrUdpSocket socket;
  const VkrUdpSocketConfig config = {.receive_buffer = 8u << 20,
                                     .send_buffer = 8u << 20};
  if (!vkr_udp_socket_open(&bind, &config, &socket)) {
    char text[256];
    vkr_udp_socket_error_text(text, sizeof(text));
    fprintf(stderr, "bind failed: %s\n", text);
    return 1;
  }
  char text[VKR_NET_ADDRESS_TEXT];
  VkrNetAddress local;
  (void)vkr_udp_socket_local_address(socket, &local);
  vkr_net_address_format(&local, text, sizeof(text));
  printf("raw server on %s\n", text);
  fflush(stdout);

  static uint8_t storage[32][VKR_UDP_DATAGRAM_MAX + 1u];
  VkrUdpDatagram batch[32];
  for (uint32_t i = 0u; i < 32u; ++i) {
    batch[i] =
        (VkrUdpDatagram){.data = storage[i], .capacity = sizeof(storage[i])};
  }
  uint64_t sink_datagrams = 0u;
  uint64_t sink_bytes = 0u;
  for (;;) {
    if (vkr_udp_socket_wait(socket, 1000) < 0) {
      break;
    }
    uint32_t received = 0u;
    if (vkr_udp_socket_receive(socket, batch, 32u, &received, NULL) ==
        VKR_UDP_ERROR) {
      break;
    }
    for (uint32_t i = 0u; i < received; ++i) {
      VkrUdpDatagram *datagram = &batch[i];
      if (datagram->size < 1u) {
        continue;
      }
      const uint8_t kind = datagram->data[0];
      if (kind == RAW_SINK) {
        sink_datagrams += 1u;
        sink_bytes += datagram->size;
      } else if (kind == RAW_PING) {
        datagram->data[0] = RAW_PONG;
        (void)vkr_udp_socket_send(socket, datagram, 1u, NULL);
      } else if (kind == RAW_REPORT) {
        uint8_t reply[17];
        reply[0] = RAW_COUNTS;
        memcpy(reply + 1, &sink_datagrams, 8u);
        memcpy(reply + 9, &sink_bytes, 8u);
        const VkrUdpDatagram answer = {.address = datagram->address,
                                       .data = reply,
                                       .capacity = sizeof(reply),
                                       .size = sizeof(reply)};
        (void)vkr_udp_socket_send(socket, &answer, 1u, NULL);
        sink_datagrams = 0u;
        sink_bytes = 0u;
      }
    }
  }
  vkr_udp_socket_close(&socket);
  return 0;
}

static int bench_raw_client(const BenchOptions *options) {
  VkrNetAddress server;
  if (!options->connect || !bench_address(options->connect, &server)) {
    fprintf(stderr, "invalid --connect\n");
    return 1;
  }
  VkrUdpSocket socket;
  const VkrNetAddress bind =
      vkr_net_address_any((VkrNetAddressFamily)server.family, 0u);
  const VkrUdpSocketConfig config = {.receive_buffer = 8u << 20,
                                     .send_buffer = 8u << 20};
  if (!vkr_udp_socket_open(&bind, &config, &socket)) {
    fprintf(stderr, "socket failed\n");
    return 1;
  }
  const float64_t seconds = options->seconds > 0.0 ? options->seconds : 5.0;
  const uint32_t size = options->size ? options->size : 1200u;
  static uint8_t payload[VKR_UDP_DATAGRAM_MAX];
  memset(payload, 0x5a, sizeof(payload));

  /* RTT: one ping in flight at a time. */
  static uint64_t samples[BENCH_RTT_SAMPLES];
  uint32_t sample_count = 0u;
  float64_t start = bench_now();
  while (bench_now() - start < 1.0 && sample_count < 2000u) {
    payload[0] = RAW_PING;
    const float64_t sent = bench_now();
    memcpy(payload + 1, &sent, sizeof(sent));
    VkrUdpDatagram ping = {
        .address = server, .data = payload, .capacity = 64u, .size = 64u};
    (void)vkr_udp_socket_send(socket, &ping, 1u, NULL);
    if (vkr_udp_socket_wait(socket, 200) <= 0) {
      continue;
    }
    uint8_t reply[128];
    VkrUdpDatagram answer = {.data = reply, .capacity = sizeof(reply)};
    uint32_t received = 0u;
    (void)vkr_udp_socket_receive(socket, &answer, 1u, &received, NULL);
    if (received == 1u && reply[0] == RAW_PONG) {
      float64_t echoed = 0.0;
      memcpy(&echoed, reply + 1, sizeof(echoed));
      samples[sample_count++] = (uint64_t)((bench_now() - echoed) * 1e6);
    }
  }
  if (sample_count > 0u) {
    qsort(samples, sample_count, sizeof(uint64_t), bench_compare_u64);
    printf(
        "result test=raw_rtt samples=%u min_us=%llu p50_us=%llu p99_us=%llu\n",
        sample_count, (unsigned long long)samples[0],
        (unsigned long long)samples[sample_count / 2u],
        (unsigned long long)samples[sample_count * 99u / 100u]);
  }

  /* Throughput: send as fast as the socket accepts. */
  payload[0] = RAW_SINK;
  VkrUdpDatagram batch[32];
  for (uint32_t i = 0u; i < 32u; ++i) {
    batch[i] = (VkrUdpDatagram){
        .address = server, .data = payload, .capacity = size, .size = size};
  }
  uint64_t sent_datagrams = 0u;
  start = bench_now();
  while (bench_now() - start < seconds) {
    uint32_t sent = 0u;
    (void)vkr_udp_socket_send(socket, batch, 32u, &sent);
    sent_datagrams += sent;
  }
  const float64_t elapsed = bench_now() - start;
  vkr_platform_sleep(200u);

  uint8_t request = RAW_REPORT;
  VkrUdpDatagram report = {
      .address = server, .data = &request, .capacity = 1u, .size = 1u};
  uint64_t delivered = 0u;
  uint64_t delivered_bytes = 0u;
  for (uint32_t attempt = 0u; attempt < 5u && delivered == 0u; ++attempt) {
    (void)vkr_udp_socket_send(socket, &report, 1u, NULL);
    if (vkr_udp_socket_wait(socket, 500) <= 0) {
      continue;
    }
    uint8_t reply[64];
    VkrUdpDatagram answer = {.data = reply, .capacity = sizeof(reply)};
    uint32_t received = 0u;
    (void)vkr_udp_socket_receive(socket, &answer, 1u, &received, NULL);
    if (received == 1u && reply[0] == RAW_COUNTS) {
      memcpy(&delivered, reply + 1, 8u);
      memcpy(&delivered_bytes, reply + 9, 8u);
    }
  }
  printf("result test=raw_throughput size=%u seconds=%.2f sent_pps=%.0f "
         "delivered_pps=%.0f delivered_mb_s=%.1f delivered_ratio=%.3f\n",
         size, elapsed, (float64_t)sent_datagrams / elapsed,
         (float64_t)delivered / elapsed,
         (float64_t)delivered_bytes / elapsed / 1e6,
         sent_datagrams ? (float64_t)delivered / (float64_t)sent_datagrams
                        : 0.0);
  vkr_udp_socket_close(&socket);
  return 0;
}

// =============================================================================
// Transport over real sockets
// =============================================================================

static const VkrNetChannelConfig bench_bulk_channel = {
    .delivery = VKR_NET_RELIABLE_UNORDERED,
    .priority = 7u,
    .max_message_size = 1u << 20,
    .receive_window = 32u << 20,
    .send_queue = 64u,
};

static const VkrNetChannelConfig bench_rtt_channel = {
    .delivery = VKR_NET_UNRELIABLE,
    .priority = 1u,
};

static void bench_accept(void *context, const VkrNetAddress *address,
                         const uint8_t peer_key[VKR_NET_KEY_SIZE],
                         const uint8_t *credential, uint32_t credential_size,
                         VkrNetAcceptResult *out_result) {
  (void)context;
  (void)credential;
  (void)credential_size;
  (void)peer_key;
  char text[VKR_NET_ADDRESS_TEXT];
  vkr_net_address_format(address, text, sizeof(text));
  printf("accept %s\n", text);
  fflush(stdout);
  out_result->code = 0u;
}

static bool8_t bench_keys(const BenchOptions *options, VkrNetKeyPair *keys) {
  if (options->secret) {
    uint8_t secret[32];
    if (!bench_hex(options->secret, secret, sizeof(secret))) {
      return false_v;
    }
    vkr_net_keypair_from_secret(secret, keys);
  } else {
    vkr_net_keypair_generate(keys);
  }
  return true_v;
}

static int bench_server(VkrAllocator *allocator, const BenchOptions *options) {
  VkrNetHostConfig config = {
      .socket = {.receive_buffer = 8u << 20, .send_buffer = 8u << 20}};
  if (!bench_address(options->bind ? options->bind : "[::]", &config.bind) ||
      !bench_keys(options, &config.core.static_keys)) {
    fprintf(stderr, "invalid --bind or --secret\n");
    return 1;
  }
  config.core.accept_incoming = true_v;
  config.core.accept = bench_accept;
  config.core.max_connections = 64u;
  config.core.max_datagram = options->mtu ? options->mtu : 0u;
  VkrNetHost *host = vkr_net_host_create(allocator, &config);
  if (!host) {
    fprintf(stderr, "server start failed\n");
    return 1;
  }
  VkrNetAddress local;
  (void)vkr_net_host_local_address(host, &local);
  char text[VKR_NET_ADDRESS_TEXT];
  vkr_net_address_format(&local, text, sizeof(text));
  printf("server on %s key ", text);
  bench_print_hex(config.core.static_keys.public_key, VKR_NET_KEY_SIZE);
  printf("\n");
  fflush(stdout);

  VkrNetCore *core = vkr_net_host_core(host);
  uint64_t bulk_bytes = 0u;
  float64_t report = bench_now();
  for (;;) {
    if (!vkr_net_host_pump(host, 100)) {
      fprintf(stderr, "socket failed\n");
      break;
    }
    VkrNetEvent event;
    while (vkr_net_core_poll(core, &event)) {
      if (event.type == VKR_NET_EVENT_CONNECTED) {
        (void)vkr_net_core_open_channel(
            core, event.connection, BENCH_BULK_CHANNEL, &bench_bulk_channel);
        (void)vkr_net_core_open_channel(core, event.connection,
                                        BENCH_RTT_CHANNEL, &bench_rtt_channel);
      } else if (event.type == VKR_NET_EVENT_MESSAGE) {
        if (event.channel == BENCH_RTT_CHANNEL) {
          const VkrNetSendOptions echo = {.flags = VKR_NET_SEND_IMMEDIATE};
          (void)vkr_net_core_send(core, event.connection, BENCH_RTT_CHANNEL,
                                  event.data, event.size, &echo,
                                  vkr_net_host_now(host));
        } else {
          bulk_bytes += event.size;
        }
      } else if (event.type == VKR_NET_EVENT_CLOSED) {
        printf("closed code=%u\n", event.code);
        fflush(stdout);
      }
    }
    if (bench_now() - report >= 1.0 && bulk_bytes > 0u) {
      printf("received mb_s=%.1f\n",
             (float64_t)bulk_bytes / (bench_now() - report) / 1e6);
      fflush(stdout);
      bulk_bytes = 0u;
      report = bench_now();
    }
  }
  vkr_net_host_destroy(host);
  return 0;
}

static int bench_client(VkrAllocator *allocator, const BenchOptions *options) {
  VkrNetAddress server;
  uint8_t server_key[VKR_NET_KEY_SIZE];
  if (!options->connect || !bench_address(options->connect, &server) ||
      !bench_hex(options->key, server_key, sizeof(server_key))) {
    fprintf(stderr, "client needs --connect and --key\n");
    return 1;
  }
  VkrNetHostConfig config = {
      .socket = {.receive_buffer = 8u << 20, .send_buffer = 8u << 20}};
  config.bind = vkr_net_address_any((VkrNetAddressFamily)server.family, 0u);
  config.core.background = options->background;
  config.core.max_datagram = options->mtu ? options->mtu : 0u;
  config.core.sent_packet_capacity = 16384u;
  vkr_net_keypair_generate(&config.core.static_keys);
  VkrNetHost *host = vkr_net_host_create(allocator, &config);
  if (!host) {
    fprintf(stderr, "client start failed\n");
    return 1;
  }
  VkrNetCore *core = vkr_net_host_core(host);
  VkrNetConnectionId connection = VKR_NET_CONNECTION_NONE;
  const float64_t connect_start = bench_now();
  if (!vkr_net_core_connect(core, &server, server_key, NULL, 0u,
                            vkr_net_host_now(host), &connection)) {
    fprintf(stderr, "connect failed\n");
    vkr_net_host_destroy(host);
    return 1;
  }
  bool8_t connected = false_v;
  bool8_t closed = false_v;
  while (!connected && !closed) {
    if (!vkr_net_host_pump(host, 50)) {
      break;
    }
    VkrNetEvent event;
    while (vkr_net_core_poll(core, &event)) {
      connected |= event.type == VKR_NET_EVENT_CONNECTED;
      if (event.type == VKR_NET_EVENT_CLOSED) {
        closed = true_v;
        fprintf(stderr, "handshake failed code=%u\n", event.code);
      }
    }
  }
  if (!connected) {
    vkr_net_host_destroy(host);
    return 1;
  }
  printf("result test=handshake ms=%.2f\n",
         (bench_now() - connect_start) * 1000.0);
  (void)vkr_net_core_open_channel(core, connection, BENCH_BULK_CHANNEL,
                                  &bench_bulk_channel);
  (void)vkr_net_core_open_channel(core, connection, BENCH_RTT_CHANNEL,
                                  &bench_rtt_channel);

  const float64_t seconds = options->seconds > 0.0 ? options->seconds : 10.0;
  const bool8_t rtt = options->mode && strcmp(options->mode, "rtt") == 0;
  static uint64_t samples[BENCH_RTT_SAMPLES];
  uint32_t sample_count = 0u;
  const uint32_t size = options->size ? Min(options->size, 1u << 20) : 1u << 20;
  uint8_t *payload = malloc(size);
  if (!payload) {
    vkr_net_host_destroy(host);
    return 1;
  }
  memset(payload, 0xa5, size);
  uint64_t queued_total = 0u;
  float64_t next_ping = 0.0;
  const float64_t start = bench_now();
  while (bench_now() - start < seconds && !closed) {
    if (rtt) {
      if (bench_now() >= next_ping) {
        uint8_t ping[64] = {0};
        const uint64_t sent = vkr_net_host_now(host);
        memcpy(ping, &sent, sizeof(sent));
        const VkrNetSendOptions now_flag = {.flags = VKR_NET_SEND_IMMEDIATE};
        (void)vkr_net_core_send(core, connection, BENCH_RTT_CHANNEL, ping,
                                sizeof(ping), &now_flag,
                                vkr_net_host_now(host));
        next_ping = bench_now() + 0.01;
      }
    } else {
      while (vkr_net_core_send(core, connection, BENCH_BULK_CHANNEL, payload,
                               size, NULL,
                               vkr_net_host_now(host)) == VKR_NET_SEND_OK) {
        queued_total += size;
      }
      vkr_net_core_flush(core);
    }
    if (!vkr_net_host_pump(host, rtt ? 5 : 1)) {
      break;
    }
    VkrNetEvent event;
    while (vkr_net_core_poll(core, &event)) {
      if (event.type == VKR_NET_EVENT_MESSAGE &&
          event.channel == BENCH_RTT_CHANNEL && event.size >= 8u &&
          sample_count < BENCH_RTT_SAMPLES) {
        uint64_t sent = 0u;
        memcpy(&sent, event.data, sizeof(sent));
        samples[sample_count++] = vkr_net_host_now(host) - sent;
      } else if (event.type == VKR_NET_EVENT_CLOSED) {
        closed = true_v;
      }
    }
  }
  const float64_t elapsed = bench_now() - start;
  VkrNetPathStats stats = {0};
  (void)vkr_net_core_path(core, connection, &stats);
  const VkrNetHostStats host_stats = vkr_net_host_stats(host);
  if (rtt) {
    if (sample_count > 0u) {
      qsort(samples, sample_count, sizeof(uint64_t), bench_compare_u64);
      printf("result test=transport_rtt samples=%u min_us=%llu p50_us=%llu "
             "p99_us=%llu\n",
             sample_count, (unsigned long long)samples[0],
             (unsigned long long)samples[sample_count / 2u],
             (unsigned long long)samples[sample_count * 99u / 100u]);
    }
  } else {
    const uint64_t acked =
        queued_total -
        vkr_net_core_queued_bytes(core, connection, BENCH_BULK_CHANNEL);
    printf("result test=transport_bulk seconds=%.2f mb_s=%.1f srtt_ms=%.2f "
           "bandwidth_mb_s=%.1f mtu=%u lost=%llu sent=%llu send_dropped=%llu\n",
           elapsed, (float64_t)acked / elapsed / 1e6,
           (float64_t)stats.srtt_us / 1000.0, (float64_t)stats.bandwidth / 1e6,
           stats.max_datagram, (unsigned long long)stats.packets_lost,
           (unsigned long long)stats.packets_sent,
           (unsigned long long)host_stats.send_dropped);
  }
  vkr_net_core_close(core, connection, VKR_NET_CLOSE_APPLICATION,
                     vkr_net_host_now(host));
  for (uint32_t i = 0u; i < 20u; ++i) {
    (void)vkr_net_host_pump(host, 10);
  }
  free(payload);
  vkr_net_host_destroy(host);
  return 0;
}

// =============================================================================
// Transport over the simulated link: CPU cost and behavior without sockets
// =============================================================================

static int bench_sim(VkrAllocator *allocator, const BenchOptions *options) {
  VkrNetCoreConfig server_config = {.accept_incoming = true_v,
                                    .accept = bench_accept,
                                    .sent_packet_capacity = 16384u};
  VkrNetCoreConfig client_config = {.sent_packet_capacity = 16384u};
  if (options->mtu) {
    server_config.max_datagram = options->mtu;
    client_config.max_datagram = options->mtu;
  }
  vkr_net_keypair_generate(&server_config.static_keys);
  vkr_net_keypair_generate(&client_config.static_keys);
  VkrNetCore *server = vkr_net_core_create(allocator, &server_config);
  VkrNetCore *client = vkr_net_core_create(allocator, &client_config);
  VkrNetSim *sim = vkr_net_sim_create(allocator, 1u);
  if (!server || !client || !sim) {
    fprintf(stderr, "sim setup failed\n");
    return 1;
  }
  VkrNetAddress server_address, client_address;
  (void)vkr_net_address_parse("10.0.0.1:1", 0u, &server_address);
  (void)vkr_net_address_parse("10.0.0.2:2", 0u, &client_address);
  const uint32_t s = vkr_net_sim_add(sim, server, &server_address);
  const uint32_t c = vkr_net_sim_add(sim, client, &client_address);
  const VkrNetSimLink link = {
      .latency_us = (uint64_t)(options->latency_ms * 1000.0),
      .loss = (float32_t)options->loss,
      .bandwidth = (uint64_t)(options->bandwidth_mb * 1e6),
      .mtu = options->mtu ? options->mtu : 0u,
  };
  vkr_net_sim_set_link(sim, s, c, &link);
  vkr_net_sim_set_link(sim, c, s, &link);

  VkrNetConnectionId connection = VKR_NET_CONNECTION_NONE;
  VkrNetConnectionId accepted = VKR_NET_CONNECTION_NONE;
  (void)vkr_net_core_connect(client, &server_address,
                             server_config.static_keys.public_key, NULL, 0u, 0u,
                             &connection);
  uint64_t now = 0u;
  bool8_t ready = false_v;
  const uint32_t megabytes = options->megabytes ? options->megabytes : 64u;
  const uint64_t total = (uint64_t)megabytes << 20;
  static uint8_t payload[1u << 20];
  uint64_t queued = 0u;
  uint64_t received = 0u;
  uint64_t start_now = 0u;
  const float64_t wall_start = bench_now();
  while (received < total && now < 3600ull * 1000000ull) {
    uint64_t next = vkr_net_sim_next_time(sim);
    now = Max(now, next == UINT64_MAX ? now + 1000u : next);
    vkr_net_sim_step(sim, now);
    VkrNetEvent event;
    while (vkr_net_core_poll(server, &event)) {
      if (event.type == VKR_NET_EVENT_CONNECTED) {
        accepted = event.connection;
        (void)vkr_net_core_open_channel(server, accepted, BENCH_BULK_CHANNEL,
                                        &bench_bulk_channel);
      } else if (event.type == VKR_NET_EVENT_MESSAGE) {
        received += event.size;
      }
    }
    while (vkr_net_core_poll(client, &event)) {
      if (event.type == VKR_NET_EVENT_CONNECTED) {
        (void)vkr_net_core_open_channel(client, connection, BENCH_BULK_CHANNEL,
                                        &bench_bulk_channel);
        ready = true_v;
        start_now = now;
      }
    }
    while (ready && queued < total &&
           vkr_net_core_send(client, connection, BENCH_BULK_CHANNEL, payload,
                             sizeof(payload), NULL, now) == VKR_NET_SEND_OK) {
      queued += sizeof(payload);
    }
    vkr_net_core_flush(client);
  }
  const float64_t wall = bench_now() - wall_start;
  VkrNetPathStats client_stats = {0}, server_stats = {0};
  (void)vkr_net_core_path(client, connection, &client_stats);
  (void)vkr_net_core_path(server, accepted, &server_stats);
  const float64_t simulated = (float64_t)(now - start_now) / 1e6;
  const uint64_t packets =
      client_stats.packets_sent + client_stats.packets_received +
      server_stats.packets_sent + server_stats.packets_received;
  printf("result test=sim_bulk megabytes=%u simulated_s=%.2f mb_s=%.1f "
         "wall_s=%.2f packets=%llu us_per_packet=%.3f lost=%llu sent=%llu "
         "mtu=%u\n",
         megabytes, simulated,
         simulated > 0.0 ? (float64_t)received / simulated / 1e6 : 0.0, wall,
         (unsigned long long)packets,
         packets ? wall * 1e6 / (float64_t)packets : 0.0,
         (unsigned long long)client_stats.packets_lost,
         (unsigned long long)client_stats.packets_sent,
         client_stats.max_datagram);
  vkr_net_sim_destroy(sim);
  vkr_net_core_destroy(client);
  vkr_net_core_destroy(server);
  return 0;
}

static void bench_usage(void) {
  fprintf(
      stderr,
      "usage: vkr_net_bench crypto|raw-server|raw-client|server|client|sim "
      "[options]\n"
      "  --bind ADDR --connect ADDR --key HEX --secret HEX --mode bulk|rtt\n"
      "  --seconds S --size B --background --mtu N\n"
      "  --latency-ms X --loss P --bandwidth-mb B --megabytes N\n");
}

VKR_MAIN(argc, argv) {
  if (argc < 2) {
    bench_usage();
    return 1;
  }
  BenchOptions options = {0};
  for (int i = 2; i < argc; ++i) {
    const char *arg = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (strcmp(arg, "--background") == 0) {
      options.background = true_v;
      continue;
    }
    if (!value) {
      bench_usage();
      return 1;
    }
    ++i;
    if (strcmp(arg, "--bind") == 0) {
      options.bind = value;
    } else if (strcmp(arg, "--connect") == 0) {
      options.connect = value;
    } else if (strcmp(arg, "--key") == 0) {
      options.key = value;
    } else if (strcmp(arg, "--secret") == 0) {
      options.secret = value;
    } else if (strcmp(arg, "--mode") == 0) {
      options.mode = value;
    } else if (strcmp(arg, "--seconds") == 0) {
      options.seconds = atof(value);
    } else if (strcmp(arg, "--size") == 0) {
      options.size = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(arg, "--mtu") == 0) {
      options.mtu = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(arg, "--latency-ms") == 0) {
      options.latency_ms = atof(value);
    } else if (strcmp(arg, "--loss") == 0) {
      options.loss = atof(value);
    } else if (strcmp(arg, "--bandwidth-mb") == 0) {
      options.bandwidth_mb = atof(value);
    } else if (strcmp(arg, "--megabytes") == 0) {
      options.megabytes = (uint32_t)strtoul(value, NULL, 10);
    } else {
      bench_usage();
      return 1;
    }
  }

  vkr_platform_init();
  if (!vkr_net_crypto_init()) {
    fprintf(stderr, "libsodium or AES-GCM hardware unavailable\n");
    return 1;
  }
  VkrDMemory memory;
  if (!vkr_dmemory_create(MB(64), GB(8), &memory)) {
    fprintf(stderr, "memory reserve failed\n");
    return 1;
  }
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  int result = 1;
  const char *command = argv[1];
  if (strcmp(command, "crypto") == 0) {
    result = bench_crypto(&options);
  } else if (strcmp(command, "raw-server") == 0) {
    result = bench_raw_server(&options);
  } else if (strcmp(command, "raw-client") == 0) {
    result = bench_raw_client(&options);
  } else if (strcmp(command, "server") == 0) {
    result = bench_server(&allocator, &options);
  } else if (strcmp(command, "client") == 0) {
    result = bench_client(&allocator, &options);
  } else if (strcmp(command, "sim") == 0) {
    result = bench_sim(&allocator, &options);
  } else {
    bench_usage();
  }
  vkr_dmemory_allocator_destroy(&allocator);
  vkr_platform_shutdown();
  return result;
}
