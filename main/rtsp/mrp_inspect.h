#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * MediaRemote (MRP) over the AirPlay DataStream "comm" command.
 *
 * iPhone 27.2 opens a type-130 RemoteControl DataStream and sends
 *   sync/comm  { params = { data = <varint length><ProtocolMessage> ... } }
 * The first message is a DEVICE_INFO_MESSAGE (type 15). v4.1.81 showed that
 * iPhone tears the RemoteControl stream down ~7 s after our empty rply, and the
 * global session ends exactly 60 s after that (sender endpoint idle timer).
 *
 * This module is diagnostic first: it decodes every comm payload completely
 * (hex + generic recursive protobuf walk, no schema needed). The optional
 * DEVICE_INFO reply builder is an experiment behind
 * CONFIG_AIRPLAY_RCS_DEVICE_INFO_REPLY.
 */

#define MRP_TYPE_DEVICE_INFO 15U

/* Log params.data of one DataStream comm payload (a bplist). */
void mrp_inspect_comm(const char *label, const uint8_t *plist,
                      size_t plist_len);

/* Generic protobuf walk of one buffer (exposed for host tests). Returns the
 * number of lines logged, 0 if the buffer is not a valid protobuf message. */
unsigned mrp_inspect_protobuf(const char *label, const uint8_t *buf,
                              size_t len);

typedef struct {
  const char *unique_identifier;   /* DeviceInfo field 1 */
  const char *name;                /* field 2 */
  const char *localized_model;     /* field 3 */
  const char *system_build;        /* field 4 */
} mrp_device_identity_t;

/*
 * Build the payload of a receiver-initiated sync/comm carrying our own
 * DEVICE_INFO_MESSAGE in answer to the sender's DEVICE_INFO request in
 * `request_plist`. The answer reuses the request's message identifier (MRP
 * request/response correlation), the request's extension field number for the
 * DeviceInfo body, and copies the protocol-level DeviceInfo fields 5
 * (applicationBundleIdentifier), 6 (applicationBundleVersion) and
 * 7 (protocolVersion). Fields 1-4 come from `id`.
 *
 * Output is the bplist { params = { data = <varint len><ProtocolMessage> } }.
 * Returns its length, or 0 when the request is not a DEVICE_INFO message or
 * the output does not fit.
 */
size_t mrp_build_device_info_reply(const uint8_t *request_plist,
                                   size_t request_len,
                                   const mrp_device_identity_t *id,
                                   uint8_t *out, size_t out_cap);
