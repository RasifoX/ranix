/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef DRIVERS_USB_USBNET_PROTO_H
#define DRIVERS_USB_USBNET_PROTO_H
#if __has_include(<uapi/types.h>)
#include <uapi/types.h>
#else
#include <stdint.h>
#endif

/*
 * USB network adapters, the part that is only arithmetic: which configuration
 * and interfaces carry Ethernet frames (CDC-ECM or RNDIS), the adapter's MAC
 * address, and RNDIS's messages (the control ones and the header in front of
 * every frame). The roboRIO's USB port, phones sharing their connection and
 * many gadgets are one of the two. drivers/usb/usb_net.c moves the bytes;
 * tests/host_usbnet.c checks this file on the host.
 *
 * Sources, all public: USB CDC 1.2 and its ECM subclass 1.2 (descriptors,
 * SET_ETHERNET_PACKET_FILTER); Microsoft's Remote NDIS specification 1.0
 * (messages, OIDs).
 */

#define UN_NONE  0
#define UN_ECM   1
#define UN_RNDIS 2

typedef struct {
	uint8_t  kind;
	uint8_t  ctrl_iface, data_iface, data_alt;   /* ECM: frames flow on alternate 1 */
	uint8_t  ep_in, ep_out;
	uint16_t mps_in, mps_out;
	uint8_t  mac_index;                          /* ECM: iMACAddress string */
} un_desc_t;

/* One configuration's descriptors: what it offers, UN_NONE if no network function. */
int un_match(const uint8_t *cfg, uint32_t total, un_desc_t *out);

/* An ECM MAC string descriptor (12 hex digits, UTF-16): 0 and mac[6], or -1. */
int un_mac_from_string(const uint8_t *desc, uint32_t len, uint8_t mac[6]);

/* A bulk OUT transfer of len bytes that is a whole number of packets needs a
 * zero-length packet after it, or the device waits for more. */
static inline int un_needs_zlp(uint32_t len, uint16_t mps) { return mps && len && len % mps == 0; }

/* ---- RNDIS ---- */
#define RNDIS_PACKET_MSG      0x00000001u
#define RNDIS_INIT_MSG        0x00000002u
#define RNDIS_QUERY_MSG       0x00000004u
#define RNDIS_SET_MSG         0x00000005u
#define RNDIS_CMPLT           0x80000000u   /* or'd into a request's type in its reply */
#define RNDIS_HDR             44            /* in front of every frame */
#define OID_802_3_PERMANENT_ADDRESS 0x01010101u
#define OID_GEN_CURRENT_PACKET_FILTER 0x0001010Eu
#define RNDIS_FILTER (0x01u | 0x04u | 0x08u)  /* directed, all multicast (NDP), broadcast */

/* Control messages into buf; each returns its length. */
uint32_t un_rndis_init(uint8_t *buf, uint32_t reqid, uint32_t max_transfer);
/* A query carries a zeroed buffer of the size it expects back: the spec allows an
 * empty one, but devices (QEMU's among them) refuse a query without it. */
uint32_t un_rndis_query(uint8_t *buf, uint32_t reqid, uint32_t oid, uint32_t inlen);
uint32_t un_rndis_set(uint8_t *buf, uint32_t reqid, uint32_t oid, const uint8_t *val, uint32_t n);
/* A reply of type want for reqid: 0 with its status 0 and, for a query, its
 * information buffer (*info into buf, *n bytes); -1 otherwise. */
int un_rndis_reply(const uint8_t *buf, uint32_t len, uint32_t want, uint32_t reqid,
                   const uint8_t **info, uint32_t *n);
/* The 44-byte header for a frame of n bytes, written at buf. */
void un_rndis_wrap(uint8_t *buf, uint32_t n);
/* The frames of one received transfer, in order, to each(ctx, frame, n). Returns
 * how many; a malformed message ends the walk. */
typedef void (*un_frame_t)(void *ctx, const uint8_t *frame, uint32_t n);
int un_rndis_unwrap(const uint8_t *buf, uint32_t len, un_frame_t each, void *ctx);

#endif
