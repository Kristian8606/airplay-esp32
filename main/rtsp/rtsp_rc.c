#include "rtsp_rc.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "airplay_identity.h"
#include "bplist_writer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hap_internal.h"
#include "plist.h"
#include "socket_utils.h"
#include "sodium.h"
#include "settings.h"
#include "srp.h"
#include "wifi.h"

static const char *TAG = "airplay_rc";

#define RC_STACK_BYTES   8192U  /* PSRAM; crypto, protobuf, MDC handlers */
#define RC_FRAME_MAX     0x400U  /* HAP transport frame plaintext limit */
#define RC_HEADER_LEN    32U
#define RC_KEEP_MAX      8192U   /* payload bytes kept for parsing/logging */
#define RC_MAX_KEYS      4
#define RC_REPLY_MAX     1024U   /* MDC rply payload */
#define RC_STOP_WAIT_MS  5000U   /* an MDC "anch" may wait up to 3 s for PTP */

enum { RC_RUNNING = 0, RC_EXITED = 1, RC_ABANDONED = 2 };
enum { DS_RC = 0, DS_MDC = 1, DS_COUNT = 2 };
enum { CLOSE_MDC = 1, CLOSE_REMOTE = 2 };

/* One encrypted DataStream channel (type 130 remote control, or the
 * MediaDataControl stream connection of a type 103 audio stream). */
typedef struct {
  const char *label;
  int listen;
  int fd;
  /* Key candidates: seed in decimal as unsigned and (if it differs) signed,
   * each with the documented and the swapped direction. The first frame that
   * authenticates picks one. */
  uint8_t rx_key[RC_MAX_KEYS][32];
  uint8_t tx_key[RC_MAX_KEYS][32];
  int nkeys;
  int key; /* chosen candidate, -1 until the first frame authenticates */
  uint64_t rx_nonce, tx_nonce;

  uint8_t wire[2 + RC_FRAME_MAX + 16];
  size_t wire_used;
  uint8_t plain[RC_FRAME_MAX];

  uint8_t hdr[RC_HEADER_LEN];
  size_t hdr_used;
  uint32_t payload_len;
  uint32_t payload_used;
  uint8_t *keep; /* RC_KEEP_MAX, PSRAM */
  uint32_t msgs;
  uint64_t tx_seqno;
  bool device_info_sent;
} ds_chan_t;

struct rtsp_rc {
  int ev_listen;
  int ev_fd;
  _Atomic bool stop;
  _Atomic int close_req;  /* CLOSE_* bits: owner asks the task to close */
  /* Exit handshake: RC_RUNNING -> RC_EXITED (task) or RC_ABANDONED (owner
   * stopped waiting). Whoever moves second frees the struct. */
  _Atomic int state;

  rtsp_conn_t *conn;     /* valid until rtsp_rc_stop() returns */
  rtsp_rc_mdc_cb mdc_cb;

  ds_chan_t ds[DS_COUNT];

  /* UDP controlPort of the APAP stream (diagnostics: what arrives there). */
  int udp_fd;
  uint32_t udp_pkts;
  uint64_t udp_bytes;
  uint8_t udp_buf[1536];
  uint8_t *mrp;      /* RC_KEEP_MAX, PSRAM */
  uint8_t *reply;    /* RC_REPLY_MAX, PSRAM */

  /* transmit buffers (kept off the task stack) */
  uint8_t tx_frame[2 + RC_FRAME_MAX + 16];
  uint8_t tx_chunk[RC_FRAME_MAX];

  /* Remote-control event channel (HAP-encrypted, Events-* keys). */
  bool ev_keys;
  uint8_t ev_rx_key[32], ev_tx_key[32];
  uint64_t ev_rx_nonce, ev_tx_nonce;
  uint8_t ev_wire[2 + RC_FRAME_MAX + 16];
  size_t ev_wire_used;
  uint8_t ev_plain[RC_FRAME_MAX];
  char ev_msg[2048];
  size_t ev_msg_used;
};

typedef struct rtsp_rc rtsp_rc_t;

static void rc_free(rtsp_rc_t *rc) {
  if (!rc) return;
  for (int i = 0; i < DS_COUNT; i++) free(rc->ds[i].keep);
  free(rc->mrp);
  free(rc->reply);
  sodium_memzero(rc, sizeof(*rc));
  heap_caps_free(rc);
}

static void close_fd(int *fd) {
  if (*fd >= 0) {
    shutdown(*fd, SHUT_RDWR);
    close(*fd);
    *fd = -1;
  }
}

/* ---- DataStream ---- */

static int send_all(int fd, const uint8_t *p, size_t n) {
  while (n > 0) {
    ssize_t w = send(fd, p, n, 0);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static int ds_send(rtsp_rc_t *rc, ds_chan_t *ds, const uint8_t *data, size_t n) {
  if (ds->key < 0 || n > RC_FRAME_MAX) return -1;
  uint8_t *frame = rc->tx_frame;
  frame[0] = (uint8_t)(n & 0xFF);
  frame[1] = (uint8_t)(n >> 8);
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, &ds->tx_nonce, 8);
  unsigned long long written = 0;
  if (crypto_aead_chacha20poly1305_ietf_encrypt(frame + 2, &written, data, n,
                                                frame, 2, NULL, nonce,
                                                ds->tx_key[ds->key]) != 0)
    return -1;
  ds->tx_nonce++;
  return send_all(ds->fd, frame, 2 + (size_t)written);
}

static void put_be(uint8_t *p, uint64_t v, unsigned bytes) {
  for (unsigned i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8U * (bytes - 1U - i)));
}
static uint64_t get_be(const uint8_t *p, unsigned bytes) {
  uint64_t v = 0;
  for (unsigned i = 0; i < bytes; i++) v = (v << 8) | p[i];
  return v;
}

/* Send one DataStream message, split into transport frames. */
static int ds_send_message(rtsp_rc_t *rc, ds_chan_t *ds, const char *type4,
                           const char *cmd4, uint64_t seqno,
                           const uint8_t *payload, size_t n) {
  uint8_t h[RC_HEADER_LEN] = {0};
  put_be(h, RC_HEADER_LEN + n, 4);
  memcpy(h + 4, type4, 4);
  if (cmd4) memcpy(h + 16, cmd4, 4);
  put_be(h + 20, seqno, 8);
  uint8_t *chunk = rc->tx_chunk;
  size_t used = sizeof(h);
  memcpy(chunk, h, sizeof(h));
  size_t off = 0;
  while (off < n || used > 0) {
    size_t take = n - off;
    if (take > RC_FRAME_MAX - used) take = RC_FRAME_MAX - used;
    if (take) memcpy(chunk + used, payload + off, take);
    used += take;
    off += take;
    if (ds_send(rc, ds, chunk, used) != 0) return -1;
    used = 0;
  }
  return 0;
}

static bool read_varint(const uint8_t *p, size_t n, size_t *i, uint64_t *out) {
  uint64_t v = 0;
  for (unsigned shift = 0; *i < n && shift < 64; shift += 7) {
    uint8_t b = p[(*i)++];
    v |= (uint64_t)(b & 0x7F) << shift;
    if (!(b & 0x80)) {
      *out = v;
      return true;
    }
  }
  return false;
}

#define MRP_DEVICE_INFO_MESSAGE 15

/* Top-level fields of one MRP ProtocolMessage: type (1) and identifier (2). */
static void mrp_fields(const uint8_t *m, size_t n, uint64_t *type, char *ident,
                       size_t ident_cap) {
  size_t i = 0;
  *type = 0;
  if (ident_cap) ident[0] = '\0';
  while (i < n) {
    uint64_t key, v;
    if (!read_varint(m, n, &i, &key)) return;
    const unsigned field = (unsigned)(key >> 3), wire = (unsigned)(key & 7);
    if (wire == 0) {
      if (!read_varint(m, n, &i, &v)) return;
      if (field == 1) *type = v;
    } else if (wire == 2) {
      if (!read_varint(m, n, &i, &v) || v > n - i) return;
      if (field == 2 && ident_cap) {
        size_t c = v < ident_cap - 1 ? (size_t)v : ident_cap - 1;
        memcpy(ident, m + i, c);
        ident[c] = '\0';
      }
      i += (size_t)v;
    } else if (wire == 1) {
      i += 8;
    } else if (wire == 5) {
      i += 4;
    } else {
      return;
    }
  }
}

/* MRP messages in params.data: list their types for the log and remember the
 * identifier of a DEVICE_INFO request. */
static void parse_mrp(const uint8_t *d, size_t n, char *out, size_t cap,
                      bool *device_info, char *ident, size_t ident_cap) {
  size_t i = 0, o = 0;
  out[0] = '\0';
  for (int count = 0; i < n && count < 8; count++) {
    size_t len;
    if (d[i] == 0x08) { /* not length-prefixed (seen for some messages) */
      len = n - i;
    } else {
      uint64_t l;
      if (!read_varint(d, n, &i, &l) || l > n - i) break;
      len = (size_t)l;
    }
    uint64_t type = 0;
    char id[64];
    mrp_fields(d + i, len, &type, id, sizeof(id));
    if (type == MRP_DEVICE_INFO_MESSAGE && !*device_info) {
      *device_info = true;
      strlcpy(ident, id, ident_cap);
    }
    int w = snprintf(out + o, cap - o, "%s%" PRIu64, o ? "," : "", type);
    if (w < 0 || (size_t)w >= cap - o) break;
    o += (size_t)w;
    i += len;
  }
}

/* ---- MRP diagnostics: readable protobuf dump ---- */

static const char *const k_pm_names[] = {
    [1] = "type", [2] = "identifier", [3] = "authenticationToken",
    [4] = "errorCode", [5] = "timestamp"};
/* DeviceInfoMessage (pyatv DeviceInfoMessage.proto) */
static const char *const k_di_names[] = {
    [1] = "uniqueIdentifier", [2] = "name", [3] = "localizedModelName",
    [4] = "systemBuildVersion", [5] = "applicationBundleIdentifier",
    [6] = "applicationBundleVersion", [7] = "protocolVersion",
    [8] = "lastSupportedMessageType", [9] = "supportsSystemPairing",
    [10] = "allowsPairing", [11] = "connected", [12] = "systemMediaApplication",
    [13] = "supportsACL", [14] = "supportsSharedQueue",
    [15] = "supportsExtendedMotion", [16] = "bluetoothAddress",
    [17] = "sharedQueueVersion", [19] = "deviceUID",
    [20] = "managedConfigDeviceID", [21] = "deviceClass",
    [22] = "logicalDeviceCount", [23] = "tightlySyncedGroup",
    [24] = "isProxyGroupPlayer", [25] = "tightSyncUID", [26] = "groupUID",
    [27] = "groupName", [28] = "groupedDevices", [29] = "isGroupLeader",
    [30] = "isAirplayActive", [31] = "systemPodcastApplication",
    [32] = "senderDefaultGroupUID", [33] = "airplayReceivers",
    [34] = "linkAgent", [35] = "clusterID", [36] = "clusterLeaderID",
    [37] = "clusterType", [38] = "isClusterAware", [39] = "modelID",
    [40] = "supportsMultiplayer", [41] = "routingContextID",
    [42] = "airPlayGroupID", [43] = "systemBooksApplication",
    [44] = "clusteredDevices", [45] = "parentGroupContainsDiscoverableGroupLeader",
    [46] = "groupContainsDiscoverableGroupLeader", [47] = "lastKnownClusterType",
    [49] = "supportsOutputContextSync", [50] = "computerName",
    [51] = "configuredClusterSize", [52] = "preferredEncoding"};

enum { NAMES_NONE, NAMES_PM, NAMES_DI };

static const char *field_name(int table, unsigned f) {
  if (table == NAMES_PM) {
    if (f == 85) return "uniqueIdentifier";
    if (f == 78) return "errorDescription";
    if (f == 20) return "deviceInfoMessage";
    if (f < sizeof(k_pm_names) / sizeof(k_pm_names[0])) return k_pm_names[f];
  } else if (table == NAMES_DI) {
    if (f < sizeof(k_di_names) / sizeof(k_di_names[0])) return k_di_names[f];
  }
  return NULL;
}

/* True if the whole buffer parses as protobuf fields. */
static bool pb_valid(const uint8_t *m, size_t n) {
  size_t i = 0;
  if (n == 0) return false;
  while (i < n) {
    uint64_t key, v;
    if (!read_varint(m, n, &i, &key) || (key >> 3) == 0) return false;
    switch (key & 7) {
      case 0: if (!read_varint(m, n, &i, &v)) return false; break;
      case 1: if (n - i < 8) return false; i += 8; break;
      case 5: if (n - i < 4) return false; i += 4; break;
      case 2: if (!read_varint(m, n, &i, &v) || v > n - i) return false; i += (size_t)v; break;
      default: return false;
    }
  }
  return true;
}

static bool printable(const uint8_t *p, size_t n) {
  if (n == 0) return true;
  for (size_t i = 0; i < n; i++)
    if (p[i] < 0x20 && p[i] != '\t') return false; /* UTF-8 bytes >= 0x80 allowed */
  return true;
}

static void pb_dump(const char *tag, const uint8_t *m, size_t n, int depth,
                    int table) {
  size_t i = 0;
  char pad[12];
  int d = depth > 5 ? 5 : depth;
  memset(pad, ' ', (size_t)(2 * d));
  pad[2 * d] = '\0';
  while (i < n) {
    uint64_t key, v;
    if (!read_varint(m, n, &i, &key)) return;
    const unsigned f = (unsigned)(key >> 3);
    const char *nm = field_name(table, f);
    char label[48];
    if (nm) snprintf(label, sizeof(label), "%u %s", f, nm);
    else snprintf(label, sizeof(label), "%u", f);
    switch (key & 7) {
      case 0:
        if (!read_varint(m, n, &i, &v)) return;
        ESP_LOGI(TAG, "%s  %s%s = %" PRIu64, tag, pad, label, v);
        break;
      case 1: {
        if (n - i < 8) return;
        uint64_t q = 0;
        for (int b = 7; b >= 0; b--) q = (q << 8) | m[i + (size_t)b]; /* little-endian */
        ESP_LOGI(TAG, "%s  %s%s = %" PRIu64 " (fixed64)", tag, pad, label, q);
        i += 8;
        break;
      }
      case 5: {
        if (n - i < 4) return;
        uint32_t w = (uint32_t)m[i] | ((uint32_t)m[i + 1] << 8) |
                     ((uint32_t)m[i + 2] << 16) | ((uint32_t)m[i + 3] << 24);
        float fv;
        memcpy(&fv, &w, sizeof(fv));
        ESP_LOGI(TAG, "%s  %s%s = 0x%08" PRIx32 " (fixed32, as float %g)", tag, pad, label,
                 w, (double)fv);
        i += 4;
        break;
      }
      case 2: {
        if (!read_varint(m, n, &i, &v) || v > n - i) return;
        const uint8_t *p = m + i;
        const size_t len = (size_t)v;
        const int sub = (table == NAMES_PM && f == 20) ? NAMES_DI :
                        (table == NAMES_DI && (f == 28 || f == 44)) ? NAMES_DI : NAMES_NONE;
        if (printable(p, len) && !(sub != NAMES_NONE && pb_valid(p, len))) {
          ESP_LOGI(TAG, "%s  %s%s = \"%.*s\"", tag, pad, label, (int)(len > 120 ? 120 : len),
                   (const char *)p);
        } else if (depth < 5 && pb_valid(p, len)) {
          ESP_LOGI(TAG, "%s  %s%s = {", tag, pad, label);
          pb_dump(tag, p, len, depth + 1, sub);
          ESP_LOGI(TAG, "%s  %s}", tag, pad);
        } else {
          char hex[2 * 32 + 1];
          size_t k = len > 32 ? 32 : len;
          for (size_t j = 0; j < k; j++) snprintf(hex + 2 * j, 3, "%02x", p[j]);
          hex[2 * k] = '\0';
          ESP_LOGI(TAG, "%s  %s%s = <%u bytes> %s%s", tag, pad, label, (unsigned)len, hex,
                   len > 32 ? "..." : "");
        }
        i += len;
        break;
      }
      default:
        return;
    }
  }
}

/* All MRP ProtocolMessages of one params.data blob. */
static void mrp_dump(const char *tag, const uint8_t *d, size_t n) {
  size_t i = 0;
  for (int count = 0; i < n && count < 8; count++) {
    uint64_t l;
    size_t start = i;
    if (!read_varint(d, n, &i, &l) || l > n - i) {
      i = start;
      l = n - i; /* not length-prefixed */
    }
    ESP_LOGI(TAG, "%s message %d (%u bytes):", tag, count + 1, (unsigned)l);
    pb_dump(tag, d + i, (size_t)l, 0, NAMES_PM);
    i += (size_t)l;
  }
}

/* ---- minimal protobuf writer ---- */

typedef struct {
  uint8_t *p;
  size_t cap, len;
  bool err;
} pbw_t;

static void pb_byte(pbw_t *b, uint8_t v) {
  if (b->len >= b->cap) {
    b->err = true;
    return;
  }
  b->p[b->len++] = v;
}
static void pb_varint(pbw_t *b, uint64_t v) {
  do {
    uint8_t c = v & 0x7F;
    v >>= 7;
    pb_byte(b, (uint8_t)(c | (v ? 0x80 : 0)));
  } while (v);
}
static void pb_uint(pbw_t *b, unsigned field, uint64_t v) {
  pb_varint(b, ((uint64_t)field << 3) | 0);
  pb_varint(b, v);
}
static void pb_bytes(pbw_t *b, unsigned field, const void *d, size_t n) {
  pb_varint(b, ((uint64_t)field << 3) | 2);
  pb_varint(b, n);
  if (b->cap - b->len < n) {
    b->err = true;
    return;
  }
  memcpy(b->p + b->len, d, n);
  b->len += n;
}
static void pb_str(pbw_t *b, unsigned field, const char *s) {
  pb_bytes(b, field, s, strlen(s));
}

static void random_uuid(char *out, size_t cap) {
  uint8_t u[16];
  esp_fill_random(u, sizeof(u));
  u[6] = (uint8_t)((u[6] & 0x0F) | 0x40);
  u[8] = (uint8_t)((u[8] & 0x3F) | 0x80);
  snprintf(out, cap,
           "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
           u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11],
           u[12], u[13], u[14], u[15]);
}

/* Send one MRP ProtocolMessage as a DataStream "sync"/"comm" message:
 * {"params": {"data": <varint length><message>}}. */
static int ds_send_mrp(rtsp_rc_t *rc, ds_chan_t *ds, const uint8_t *pm, size_t len) {
  enum { DATA_CAP = 640, PAYLOAD_CAP = 1024 };
  int ret = -1;
  uint8_t *data_buf = malloc(DATA_CAP);
  uint8_t *payload = malloc(PAYLOAD_CAP);
  bpw_t *w = malloc(sizeof(*w));
  if (data_buf && payload && w) {
    pbw_t data = {.p = data_buf, .cap = DATA_CAP};
    pb_varint(&data, len);
    if (!data.err && data.cap - data.len >= len) {
      memcpy(data_buf + data.len, pm, len);
      data.len += len;
      mrp_dump("MRP out", data_buf, data.len);
      bpw_init(w);
      bpw_dict_begin(w);
      bpw_key(w, "params");
      bpw_dict_begin(w);
      bpw_kv_data(w, "data", data_buf, data.len);
      bpw_end(w);
      bpw_end(w);
      const size_t plen = bpw_finish(w, payload, PAYLOAD_CAP);
      if (plen) ret = ds_send_message(rc, ds, "sync", "comm", ds->tx_seqno++, payload, plen);
    }
  }
  free(w);
  free(payload);
  free(data_buf);
  return ret;
}

/* Our DEVICE_INFO_MESSAGE answer: who we are (HomePod-like), echoing the
 * request identifier so the sender can match it. */
static int ds_send_device_info(rtsp_rc_t *rc, ds_chan_t *ds, const char *request_id) {
  char name[65], deviceid[18], psi[AIRPLAY_PAIRING_ID_LEN];
  char uid[40];
  settings_get_device_name(name, sizeof(name));
  wifi_get_mac_str(deviceid, sizeof(deviceid));
  airplay_get_psi(psi, sizeof(psi));
  random_uuid(uid, sizeof(uid));

  enum { INFO_CAP = 512, MSG_CAP = 600 };
  uint8_t *bufs = malloc(INFO_CAP + MSG_CAP);
  if (!bufs) return -1;
  uint8_t *info_buf = bufs, *msg_buf = bufs + INFO_CAP;
  int ret = -1;
  pbw_t info = {.p = info_buf, .cap = INFO_CAP};
  /* Only what we can stand behind. Claiming group/cluster leadership
   * (groupUID, isGroupLeader, airPlayGroupID, ...) coincided with the iPhone
   * no longer sending PTP to us and restarting sessions -- keep them out. */
  pb_str(&info, 1, psi);                       /* uniqueIdentifier */
  pb_str(&info, 2, name);                      /* name */
  pb_str(&info, 3, "HomePod");                 /* localizedModelName */
  pb_str(&info, 4, AIRPLAY_OS_BUILD);          /* systemBuildVersion */
  pb_str(&info, 5, "com.apple.mediaremoted");  /* applicationBundleIdentifier */
  pb_uint(&info, 7, 1);                        /* protocolVersion */
  pb_uint(&info, 8, 108);                      /* lastSupportedMessageType */
  pb_uint(&info, 9, 1);                        /* supportsSystemPairing */
  pb_uint(&info, 10, 1);                       /* allowsPairing */
  pb_uint(&info, 13, 1);                       /* supportsACL */
  pb_uint(&info, 14, 1);                       /* supportsSharedQueue */
  pb_uint(&info, 15, 1);                       /* supportsExtendedMotion */
  pb_uint(&info, 17, 2);                       /* sharedQueueVersion */
  pb_str(&info, 19, deviceid);                 /* deviceUID */
  pb_uint(&info, 21, 7);                       /* deviceClass = Accessory */
  pb_uint(&info, 22, 1);                       /* logicalDeviceCount */
  pb_uint(&info, 30, 1);                       /* isAirplayActive */
  pb_str(&info, 39, AIRPLAY_MODEL);            /* modelID */
  if (info.err) goto out;

  pbw_t msg = {.p = msg_buf, .cap = MSG_CAP};
  pb_uint(&msg, 1, MRP_DEVICE_INFO_MESSAGE);   /* type */
  if (request_id && request_id[0]) pb_str(&msg, 2, request_id); /* identifier */
  pb_uint(&msg, 4, 0);                         /* errorCode = NoError */
  pb_bytes(&msg, 20, info_buf, info.len);      /* deviceInfoMessage */
  pb_str(&msg, 85, uid);                       /* uniqueIdentifier */
  if (msg.err) goto out;
  ret = ds_send_mrp(rc, ds, msg_buf, msg.len);
out:
  free(bufs);
  return ret;
}

#if CONFIG_AIRPLAY_HP_MRP_CLIENT
/* After the DEVICE_INFO exchange an MRP client continues with
 * SET_CONNECTION_STATE(Connected) and CLIENT_UPDATES_CONFIG (pyatv's order).
 * The iPhone sends nothing after DEVICE_INFO, waits ~5 s, drops the type 130
 * stream and ends the whole session 60 s later, so (lab) we take that role. */
static void ds_send_mrp_client_handshake(rtsp_rc_t *rc, ds_chan_t *ds) {
  uint8_t m[160];
  char uid[40], id[40];

  random_uuid(uid, sizeof(uid));
  pbw_t a = {.p = m, .cap = sizeof(m)};
  pb_uint(&a, 1, 38);                          /* SET_CONNECTION_STATE_MESSAGE */
  pb_uint(&a, 4, 0);
  pb_str(&a, 85, uid);
  uint8_t st[2] = {0x08, 2};                   /* state = Connected */
  pb_bytes(&a, 42, st, sizeof(st));
  if (!a.err && ds_send_mrp(rc, ds, m, a.len) == 0)
    ESP_LOGI(TAG, "DataStream: sent SET_CONNECTION_STATE(Connected)");

  random_uuid(uid, sizeof(uid));
  random_uuid(id, sizeof(id));
  pbw_t b = {.p = m, .cap = sizeof(m)};
  pb_uint(&b, 1, 16);                          /* CLIENT_UPDATES_CONFIG_MESSAGE */
  pb_str(&b, 2, id);
  pb_uint(&b, 4, 0);
  pb_str(&b, 85, uid);
  uint8_t cfg[10] = {0x08, 1, 0x10, 1, 0x18, 1, 0x20, 1, 0x28, 1}; /* all updates */
  pb_bytes(&b, 21, cfg, sizeof(cfg));
  if (!b.err && ds_send_mrp(rc, ds, m, b.len) == 0)
    ESP_LOGI(TAG, "DataStream: sent CLIENT_UPDATES_CONFIG (identifier %s)", id);
}
#endif

static void ds_message_done(rtsp_rc_t *rc, ds_chan_t *ds) {
  const uint8_t *h = ds->hdr;
  const uint64_t seqno = get_be(h + 20, 8);
  char type[13] = {0}, cmd[5] = {0};
  memcpy(type, h + 4, 12);
  memcpy(cmd, h + 16, 4);
  for (int k = 0; k < 4; k++)
    if (cmd[k] && (cmd[k] < 0x20 || cmd[k] > 0x7E)) cmd[k] = '.';
  const size_t kept = ds->payload_used < RC_KEEP_MAX ? ds->payload_used : RC_KEEP_MAX;
  const bool complete = kept == ds->payload_len && ds->keep;
  ds->msgs++;
  const bool sync = strncmp(type, "sync", 4) == 0;

  if (ds == &rc->ds[DS_MDC]) {
    if (!sync) {
      ESP_LOGI(TAG, "MDC %.4s seq=%" PRIu64 " len=%" PRIu32, type, seqno, ds->payload_len);
      return;
    }
    size_t reply_len = 0;
    if (rc->mdc_cb && rc->conn && complete && rc->reply)
      reply_len = rc->mdc_cb(rc->conn, cmd, ds->keep, kept, rc->reply, RC_REPLY_MAX);
    if (ds_send_message(rc, ds, "rply", NULL, seqno, rc->reply, reply_len) != 0)
      ESP_LOGW(TAG, "MDC rply %s failed", cmd);
    return;
  }

  char mrp[96] = "";
  bool device_info = false;
  char ident[64] = "";
  if (complete && kept > 0 && rc->mrp) {
    size_t mrp_len = 0;
    if (bplist_find_data_deep(ds->keep, kept, "data", rc->mrp, RC_KEEP_MAX, &mrp_len))
    {
      parse_mrp(rc->mrp, mrp_len, mrp, sizeof(mrp), &device_info, ident, sizeof(ident));
      mrp_dump("MRP in", rc->mrp, mrp_len);
    }
  }

  if (sync) {
    ESP_LOGI(TAG, "DataStream sync #%" PRIu32 " seq=%" PRIu64 " cmd=%s len=%" PRIu32
             "%s%s", ds->msgs, seqno, cmd, ds->payload_len, mrp[0] ? " mrp=" : "", mrp);
    if (ds_send_message(rc, ds, "rply", NULL, seqno, NULL, 0) != 0)
      ESP_LOGW(TAG, "DataStream reply failed");
    if (device_info && !ds->device_info_sent) {
      ds->device_info_sent = true;
      if (ds_send_device_info(rc, ds, ident) == 0) {
        ESP_LOGI(TAG, "DataStream: answered DEVICE_INFO (identifier %s)",
                 ident[0] ? ident : "-");
#if CONFIG_AIRPLAY_HP_MRP_CLIENT
        ds_send_mrp_client_handshake(rc, ds);
#endif
      } else {
        ESP_LOGW(TAG, "DataStream: DEVICE_INFO answer failed");
      }
    }
  } else {
    ESP_LOGI(TAG, "DataStream %.4s seq=%" PRIu64 " len=%" PRIu32, type, seqno,
             ds->payload_len);
  }
}

/* Feed decrypted bytes into the message parser. */
static int ds_consume(rtsp_rc_t *rc, ds_chan_t *ds, const uint8_t *p, size_t n) {
  while (n > 0) {
    if (ds->hdr_used < RC_HEADER_LEN) {
      size_t take = RC_HEADER_LEN - ds->hdr_used;
      if (take > n) take = n;
      memcpy(ds->hdr + ds->hdr_used, p, take);
      ds->hdr_used += take;
      p += take;
      n -= take;
      if (ds->hdr_used < RC_HEADER_LEN) return 0;
      const uint32_t size = (uint32_t)get_be(ds->hdr, 4);
      if (size < RC_HEADER_LEN || size > 16U * 1024U * 1024U) {
        ESP_LOGE(TAG, "%s: bad message size %" PRIu32, ds->label, size);
        return -1;
      }
      ds->payload_len = size - RC_HEADER_LEN;
      ds->payload_used = 0;
      if (ds->payload_len == 0) {
        ds_message_done(rc, ds);
        ds->hdr_used = 0;
        continue;
      }
    }
    const uint32_t want = ds->payload_len - ds->payload_used;
    size_t take = want < n ? want : n;
    if (ds->keep && ds->payload_used < RC_KEEP_MAX) {
      size_t room = RC_KEEP_MAX - ds->payload_used;
      memcpy(ds->keep + ds->payload_used, p, take < room ? take : room);
    }
    ds->payload_used += (uint32_t)take;
    p += take;
    n -= take;
    if (ds->payload_used == ds->payload_len) {
      ds_message_done(rc, ds);
      ds->hdr_used = 0;
    }
  }
  return 0;
}

static bool ds_decrypt(ds_chan_t *ds, int k, size_t ct_len, unsigned long long *out) {
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, &ds->rx_nonce, 8);
  return crypto_aead_chacha20poly1305_ietf_decrypt(ds->plain, out, NULL, ds->wire + 2,
                                                   ct_len, ds->wire, 2, nonce,
                                                   ds->rx_key[k]) == 0;
}

/* Read what is available; returns -1 when the connection must close. */
static int ds_read(rtsp_rc_t *rc, ds_chan_t *ds) {
  size_t target = 2;
  if (ds->wire_used >= 2) {
    const size_t length = (size_t)ds->wire[0] | ((size_t)ds->wire[1] << 8);
    if (!length || length > RC_FRAME_MAX) return -1;
    target = length + 18;
  }
  ssize_t r = recv(ds->fd, ds->wire + ds->wire_used, target - ds->wire_used, 0);
  if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  if (r <= 0) return -1;
  ds->wire_used += (size_t)r;
  if (target == 2 || ds->wire_used < target) return 0;

  unsigned long long plain_len = 0;
  bool ok = false;
  if (ds->key >= 0) {
    ok = ds_decrypt(ds, ds->key, target - 2, &plain_len);
  } else {
    for (int k = 0; k < ds->nkeys && !ok; k++) {
      if (ds_decrypt(ds, k, target - 2, &plain_len)) {
        ds->key = k;
        ok = true;
        ESP_LOGI(TAG, "%s keys verified (%s seed, %s direction)", ds->label,
                 (k / 2) == 0 ? "unsigned" : "signed",
                 (k % 2) == 0 ? "documented" : "swapped");
      }
    }
  }
  if (!ok) {
    ESP_LOGE(TAG, "%s authentication failed", ds->label);
    return -1;
  }
  ds->rx_nonce++;
  ds->wire_used = 0;
  return ds_consume(rc, ds, ds->plain, (size_t)plain_len);
}

/* ---- task ---- */

/* ---- remote-control event channel ---- */

static int ev_send(rtsp_rc_t *rc, const uint8_t *data, size_t n) {
  while (n > 0) {
    size_t c = n > RC_FRAME_MAX ? RC_FRAME_MAX : n;
    uint8_t *frame = rc->tx_frame;
    frame[0] = (uint8_t)(c & 0xFF);
    frame[1] = (uint8_t)(c >> 8);
    uint8_t nonce[12] = {0};
    memcpy(nonce + 4, &rc->ev_tx_nonce, 8);
    unsigned long long written = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(frame + 2, &written, data, c, frame, 2,
                                                  NULL, nonce, rc->ev_tx_key) != 0)
      return -1;
    rc->ev_tx_nonce++;
    if (send_all(rc->ev_fd, frame, 2 + (size_t)written) != 0) return -1;
    data += c;
    n -= c;
  }
  return 0;
}

/* Log every complete message; answer requests with 200 OK (same CSeq). */
static void ev_process(rtsp_rc_t *rc) {
  for (;;) {
    rc->ev_msg[rc->ev_msg_used] = '\0';
    char *end = strstr(rc->ev_msg, "\r\n\r\n");
    if (!end) return;
    size_t header_len = (size_t)(end - rc->ev_msg) + 4;
    size_t body_len = 0;
    const char *cl = strcasestr(rc->ev_msg, "Content-Length:");
    if (cl && cl < end) body_len = (size_t)strtoul(cl + 15, NULL, 10);
    if (header_len + body_len > rc->ev_msg_used) return;

    char first[96];
    size_t fl = strcspn(rc->ev_msg, "\r\n");
    if (fl >= sizeof(first)) fl = sizeof(first) - 1;
    memcpy(first, rc->ev_msg, fl);
    first[fl] = '\0';
    int cseq = -1;
    const char *cs = strcasestr(rc->ev_msg, "CSeq:");
    if (cs && cs < end) cseq = atoi(cs + 5);
    char dump[256] = "";
    if (body_len && body_len <= sizeof(rc->ev_msg) - header_len)
      bplist_dump((const uint8_t *)rc->ev_msg + header_len, body_len, dump, sizeof(dump), false);
    ESP_LOGI(TAG, "RC event in: '%s' cseq=%d body=%u %s", first, cseq, (unsigned)body_len,
             dump);
    if (strncmp(first, "RTSP/", 5) != 0 && strncmp(first, "HTTP/", 5) != 0) {
      char reply[96];
      int n = snprintf(reply, sizeof(reply), "RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n",
                       cseq < 0 ? 0 : cseq);
      if (ev_send(rc, (const uint8_t *)reply, (size_t)n) != 0)
        ESP_LOGW(TAG, "RC event reply failed");
    }
    const size_t used = header_len + body_len;
    memmove(rc->ev_msg, rc->ev_msg + used, rc->ev_msg_used - used);
    rc->ev_msg_used -= used;
  }
}

static int ev_read(rtsp_rc_t *rc) {
  size_t target = 2;
  if (rc->ev_wire_used >= 2) {
    const size_t length = (size_t)rc->ev_wire[0] | ((size_t)rc->ev_wire[1] << 8);
    if (!length || length > RC_FRAME_MAX) return -1;
    target = length + 18;
  }
  ssize_t r = recv(rc->ev_fd, rc->ev_wire + rc->ev_wire_used, target - rc->ev_wire_used, 0);
  if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  if (r <= 0) return -1;
  rc->ev_wire_used += (size_t)r;
  if (target == 2 || rc->ev_wire_used < target) return 0;
  rc->ev_wire_used = 0;
  if (!rc->ev_keys) {
    ESP_LOGI(TAG, "RC event in: %u encrypted bytes (no keys)", (unsigned)(target - 2));
    return 0;
  }
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, &rc->ev_rx_nonce, 8);
  unsigned long long plain_len = 0;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(rc->ev_plain, &plain_len, NULL,
                                                rc->ev_wire + 2, target - 2, rc->ev_wire,
                                                2, nonce, rc->ev_rx_key) != 0) {
    ESP_LOGW(TAG, "RC event: authentication failed (%u bytes)", (unsigned)(target - 2));
    rc->ev_keys = false; /* log sizes only from now on */
    return 0;
  }
  rc->ev_rx_nonce++;
  if (rc->ev_msg_used + plain_len >= sizeof(rc->ev_msg)) rc->ev_msg_used = 0;
  memcpy(rc->ev_msg + rc->ev_msg_used, rc->ev_plain, (size_t)plain_len);
  rc->ev_msg_used += (size_t)plain_len;
  ev_process(rc);
  return 0;
}

static int accept_one(int *listen_fd, const char *what) {
  int c = accept(*listen_fd, NULL, NULL);
  if (c < 0) {
    /* e.g. ENFILE when all lwIP sockets are in use: the connection stays
     * pending and select() reports it again, so do not spin. */
    ESP_LOGW(TAG, "%s accept failed (errno %d)", what, errno);
    vTaskDelay(pdMS_TO_TICKS(50));
    return -1;
  }
  close_fd(listen_fd); /* one client per channel */
  ESP_LOGI(TAG, "%s channel connected", what);
  return c;
}

static void rc_task(void *arg) {
  rtsp_rc_t *rc = arg;
  while (!atomic_load(&rc->stop)) {
    const int req = atomic_load(&rc->close_req);
    if (req) {
      for (int i = 0; i < DS_COUNT; i++) {
        if (!(req & (i == DS_MDC ? CLOSE_MDC : CLOSE_REMOTE))) continue;
        ds_chan_t *m = &rc->ds[i];
        if (m->fd >= 0)
          ESP_LOGI(TAG, "%s closed after %" PRIu32 " messages", m->label, m->msgs);
        close_fd(&m->listen);
        close_fd(&m->fd);
      }
      if ((req & CLOSE_MDC) && rc->udp_fd >= 0) {
        ESP_LOGI(TAG, "controlPort closed after %" PRIu32 " packets", rc->udp_pkts);
        close_fd(&rc->udp_fd);
      }
      atomic_fetch_and(&rc->close_req, ~req);
    }
    fd_set rd;
    FD_ZERO(&rd);
    int maxfd = -1;
    int fds[3 + 2 * DS_COUNT] = {rc->ev_listen, rc->ev_fd, rc->udp_fd};
    for (int i = 0; i < DS_COUNT; i++) {
      fds[3 + 2 * i] = rc->ds[i].listen;
      fds[4 + 2 * i] = rc->ds[i].fd;
    }
    for (int i = 0; i < 3 + 2 * DS_COUNT; i++) {
      if (fds[i] >= 0) {
        FD_SET(fds[i], &rd);
        if (fds[i] > maxfd) maxfd = fds[i];
      }
    }
    if (maxfd < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 250000};
    int n = select(maxfd + 1, &rd, NULL, NULL, &tv);
    if (n <= 0) continue;

    if (rc->ev_listen >= 0 && FD_ISSET(rc->ev_listen, &rd)) {
      int c = accept_one(&rc->ev_listen, "Remote control event");
      if (c >= 0) rc->ev_fd = c;
    }
    if (rc->ev_fd >= 0 && FD_ISSET(rc->ev_fd, &rd)) {
      if (ev_read(rc) < 0) {
        ESP_LOGI(TAG, "Remote control event channel closed");
        close_fd(&rc->ev_fd);
      }
    }

    if (rc->udp_fd >= 0 && FD_ISSET(rc->udp_fd, &rd)) {
      struct sockaddr_storage from;
      socklen_t flen = sizeof(from);
      ssize_t r = recvfrom(rc->udp_fd, rc->udp_buf, sizeof(rc->udp_buf), 0,
                           (struct sockaddr *)&from, &flen);
      if (r > 0) {
        rc->udp_pkts++;
        rc->udp_bytes += (uint64_t)r;
        if (rc->udp_pkts <= 8 || (rc->udp_pkts % 200U) == 0) {
          char hex[2 * 48 + 1];
          size_t k = (size_t)r > 48 ? 48 : (size_t)r;
          for (size_t j = 0; j < k; j++) snprintf(hex + 2 * j, 3, "%02x", rc->udp_buf[j]);
          hex[2 * k] = '\0';
          ESP_LOGI(TAG, "controlPort packet #%" PRIu32 " %d bytes: %s%s", rc->udp_pkts,
                   (int)r, hex, (size_t)r > 48 ? "..." : "");
        }
      }
    }

    for (int i = 0; i < DS_COUNT; i++) {
      ds_chan_t *ds = &rc->ds[i];
      if (ds->listen >= 0 && FD_ISSET(ds->listen, &rd)) {
        int c = accept_one(&ds->listen, ds->label);
        if (c >= 0) ds->fd = c;
      }
      if (ds->fd >= 0 && FD_ISSET(ds->fd, &rd)) {
        if (ds_read(rc, ds) < 0) {
          ESP_LOGI(TAG, "%s closed after %" PRIu32 " messages", ds->label, ds->msgs);
          close_fd(&ds->fd);
        }
      }
    }
  }
  close_fd(&rc->ev_listen);
  close_fd(&rc->ev_fd);
  close_fd(&rc->udp_fd);
  for (int i = 0; i < DS_COUNT; i++) {
    close_fd(&rc->ds[i].listen);
    close_fd(&rc->ds[i].fd);
  }
  if (atomic_exchange(&rc->state, RC_EXITED) == RC_ABANDONED) rc_free(rc);
  vTaskDeleteWithCaps(NULL);
}

/* ---- public ---- */

static rtsp_rc_t *rc_get(rtsp_conn_t *conn) {
  if (conn->rc) return conn->rc;
  rtsp_rc_t *rc = heap_caps_calloc(1, sizeof(*rc), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rc) return NULL;
  rc->ev_listen = rc->ev_fd = rc->udp_fd = -1;
  for (int i = 0; i < DS_COUNT; i++) {
    rc->ds[i].listen = rc->ds[i].fd = -1;
    rc->ds[i].key = -1;
  }
  rc->ds[DS_RC].label = "DataStream";
  rc->ds[DS_MDC].label = "MediaDataControl";
  rc->conn = conn;
  atomic_store(&rc->state, RC_RUNNING);
  if (xTaskCreatePinnedToCoreWithCaps(rc_task, "airplay_rc", RC_STACK_BYTES, rc, 5,
                                      NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
    ESP_LOGE(TAG, "Remote control task start failed");
    heap_caps_free(rc);
    return NULL;
  }
  conn->rc = rc;
  return rc;
}

static int open_listener(uint16_t *port) {
  uint16_t bound = 0;
  int s = socket_utils_bind_tcp_listener(0, 1, false, &bound);
  if (s >= 0 && port) *port = bound;
  return s;
}

esp_err_t rtsp_rc_open_events(rtsp_conn_t *conn, uint16_t *port) {
  if (!conn) return ESP_ERR_INVALID_ARG;
  int s = open_listener(port);
  if (s < 0) return ESP_FAIL;
  rtsp_rc_t *rc = rc_get(conn);
  if (!rc) {
    close(s);
    return ESP_ERR_NO_MEM;
  }
  if (rc->ev_listen >= 0 || rc->ev_fd >= 0) {
    close(s); /* already open: keep the first one */
    return ESP_ERR_INVALID_STATE;
  }
  /* Same keys as the audio event channel (rtsp_remote.c). */
  const hap_session_t *hap = conn->hap_session;
  if (conn->encrypted_mode && hap && hap->session_established) {
    const uint8_t *secret = hap->shared_secret;
    size_t secret_len = sizeof(hap->shared_secret);
    if (hap->pair_setup_transient) secret = srp_get_session_key(hap->srp, &secret_len);
    if (secret && secret_len) {
      static const char salt[] = "Events-Salt";
      static const char rx_info[] = "Events-Read-Encryption-Key";
      static const char tx_info[] = "Events-Write-Encryption-Key";
      hap_hkdf_sha512((const uint8_t *)salt, strlen(salt), secret, secret_len,
                      (const uint8_t *)rx_info, strlen(rx_info), rc->ev_rx_key, 32);
      hap_hkdf_sha512((const uint8_t *)salt, strlen(salt), secret, secret_len,
                      (const uint8_t *)tx_info, strlen(tx_info), rc->ev_tx_key, 32);
      rc->ev_keys = true;
    }
  }
  rc->ev_rx_nonce = rc->ev_tx_nonce = 0;
  rc->ev_wire_used = rc->ev_msg_used = 0;
  atomic_thread_fence(memory_order_release);
  rc->ev_listen = s;
  return ESP_OK;
}

/* Keys, parser state and listener for one DataStream channel. */
static esp_err_t ds_open(rtsp_conn_t *conn, rtsp_rc_t *rc, ds_chan_t *ds,
                         uint64_t seed, uint16_t *port) {
  const hap_session_t *hap = conn->hap_session;
  if (!conn->encrypted_mode || !hap || !hap->session_established) {
    ESP_LOGE(TAG, "%s needs a verified pairing", ds->label);
    return ESP_ERR_INVALID_STATE;
  }
  const uint8_t *secret = hap->shared_secret;
  size_t secret_len = sizeof(hap->shared_secret);
  if (hap->pair_setup_transient) secret = srp_get_session_key(hap->srp, &secret_len);
  if (!secret || !secret_len) return ESP_ERR_INVALID_STATE;
  if (ds->listen >= 0 || ds->fd >= 0) {
    ESP_LOGW(TAG, "%s already open", ds->label);
    return ESP_ERR_INVALID_STATE;
  }
  if (!ds->keep) ds->keep = heap_caps_malloc(RC_KEEP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rc->mrp) rc->mrp = heap_caps_malloc(RC_KEEP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rc->reply) rc->reply = heap_caps_malloc(RC_REPLY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ds->keep) return ESP_ERR_NO_MEM;

  /* Salt suffix: the seed in decimal. Try the unsigned form first and the
   * signed form when it differs; the first frame decides. */
  char salts[2][48];
  int nsalts = 0;
  snprintf(salts[nsalts++], sizeof(salts[0]), "DataStream-Salt%" PRIu64, seed);
  if ((int64_t)seed < 0)
    snprintf(salts[nsalts++], sizeof(salts[0]), "DataStream-Salt%" PRId64, (int64_t)seed);
  static const char out_info[] = "DataStream-Output-Encryption-Key";
  static const char in_info[] = "DataStream-Input-Encryption-Key";
  int nkeys = 0;
  for (int s = 0; s < nsalts; s++) {
    uint8_t k_out[32], k_in[32];
    hap_hkdf_sha512((const uint8_t *)salts[s], strlen(salts[s]), secret, secret_len,
                    (const uint8_t *)out_info, strlen(out_info), k_out, 32);
    hap_hkdf_sha512((const uint8_t *)salts[s], strlen(salts[s]), secret, secret_len,
                    (const uint8_t *)in_info, strlen(in_info), k_in, 32);
    /* documented (pyatv): sender encrypts with Output, receiver with Input */
    memcpy(ds->rx_key[nkeys], k_out, 32);
    memcpy(ds->tx_key[nkeys], k_in, 32);
    nkeys++;
    memcpy(ds->rx_key[nkeys], k_in, 32);
    memcpy(ds->tx_key[nkeys], k_out, 32);
    nkeys++;
    sodium_memzero(k_out, sizeof(k_out));
    sodium_memzero(k_in, sizeof(k_in));
  }
  ds->nkeys = nkeys;
  ds->key = -1;
  ds->rx_nonce = ds->tx_nonce = 0;
  ds->tx_seqno = 0x100000000ULL + (esp_random() & 0x7FFFFFFFU);
  ds->device_info_sent = false;
  ds->wire_used = ds->hdr_used = 0;
  ds->msgs = 0;

  int s = open_listener(port);
  if (s < 0) return ESP_FAIL;
  /* Keys and parser state first, then publish the listener to the task. */
  atomic_thread_fence(memory_order_release);
  ds->listen = s;
  return ESP_OK;
}

esp_err_t rtsp_rc_open_data(rtsp_conn_t *conn, uint64_t seed, uint16_t *port) {
  if (!conn) return ESP_ERR_INVALID_ARG;
  rtsp_rc_t *rc = rc_get(conn);
  if (!rc) return ESP_ERR_NO_MEM;
  return ds_open(conn, rc, &rc->ds[DS_RC], seed, port);
}

static void rc_close(rtsp_conn_t *conn, int what) {
  if (!conn || !conn->rc) return;
  rtsp_rc_t *rc = conn->rc;
  /* Nothing open on the requested channels: no need to involve the task. */
  bool open = false;
  if (what & CLOSE_MDC)
    open |= rc->ds[DS_MDC].listen >= 0 || rc->ds[DS_MDC].fd >= 0 || rc->udp_fd >= 0;
  if (what & CLOSE_REMOTE)
    open |= rc->ds[DS_RC].listen >= 0 || rc->ds[DS_RC].fd >= 0;
  if (!open) return;
  atomic_fetch_or(&rc->close_req, what);
  TickType_t start = xTaskGetTickCount();
  /* Sleep at least one tick: the RC task may run on this core at a lower
   * priority, and IDLE must run too (task watchdog). */
  while ((atomic_load(&rc->close_req) & what) &&
         (TickType_t)(xTaskGetTickCount() - start) < pdMS_TO_TICKS(RC_STOP_WAIT_MS))
    vTaskDelay(1);
}

void rtsp_rc_close_mdc(rtsp_conn_t *conn) { rc_close(conn, CLOSE_MDC); }

void rtsp_rc_close_remote(rtsp_conn_t *conn) { rc_close(conn, CLOSE_REMOTE); }

esp_err_t rtsp_rc_open_mdc(rtsp_conn_t *conn, uint64_t seed, rtsp_rc_mdc_cb cb,
                           int udp_fd, uint16_t *port) {
  if (!conn || !cb) {
    if (udp_fd >= 0) close(udp_fd);
    return ESP_ERR_INVALID_ARG;
  }
  rtsp_rc_close_mdc(conn); /* a previous audio stream's channel */
  rtsp_rc_t *rc = rc_get(conn);
  if (!rc || (atomic_load(&rc->close_req) & CLOSE_MDC)) {
    if (udp_fd >= 0) close(udp_fd);
    return rc ? ESP_ERR_TIMEOUT : ESP_ERR_NO_MEM;
  }
  rc->mdc_cb = cb;
  esp_err_t err = ds_open(conn, rc, &rc->ds[DS_MDC], seed, port);
  if (err != ESP_OK) {
    if (udp_fd >= 0) close(udp_fd);
    return err;
  }
  rc->udp_pkts = 0;
  rc->udp_bytes = 0;
  atomic_thread_fence(memory_order_release);
  rc->udp_fd = udp_fd; /* the task owns it from here */
  return ESP_OK;
}

void rtsp_rc_stop(rtsp_conn_t *conn) {
  if (!conn || !conn->rc) return;
  rtsp_rc_t *rc = conn->rc;
  conn->rc = NULL;
  atomic_store(&rc->stop, true);
  TickType_t start = xTaskGetTickCount();
  while (atomic_load(&rc->state) == RC_RUNNING &&
         (TickType_t)(xTaskGetTickCount() - start) < pdMS_TO_TICKS(RC_STOP_WAIT_MS))
    vTaskDelay(1);
  /* If the task is still running it frees the struct when it exits. */
  if (atomic_exchange(&rc->state, RC_ABANDONED) == RC_EXITED) rc_free(rc);
}
