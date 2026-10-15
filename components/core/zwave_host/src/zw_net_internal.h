/*
 * zw_net_internal.h - between zw_net.c (the link, the routing of frames, the operations' API) and
 * zw_net_nodes.c (the node cache and its persistence, the check at every link up, the interview,
 * sleeping devices, their own check-ins and supervision through zw_health, removals). Private to
 * zwave_host.
 */
#ifndef ZW_NET_INTERNAL_H
#define ZW_NET_INTERNAL_H

#include "zwave_host/zw_net.h"
#include "zwave_proto/zw_api.h"

/* The network operation running (network.md §1). */
enum { OP_NONE = 0, OP_ADD, OP_REMOVE, OP_REGION, OP_RESET, OP_SMART, OP_ROUTES, OP_POWER, OP_OTW };

/* zw_net's own tags: 0xFF3 | what | a stamp or an index. */
#define NT_BASE  0xFF300000u
#define NT_MASK  0xFFF00000u
#define NT_QUEUE 1u /* a wake-up queue command out: | its qout index */
#define NT_PING  2u /* a supervision ping: | stamp | its index */
#define NT_NMI   3u /* No More Information */
#define NT_CHECK 4u /* GetSerialApiInitData at a link up */
#define NT_RESET 5u /* HardReset */
#define NT_NIF   6u /* RequestNodeInfo for a listening device */
#define NT_CHECK_IN 7u /* a device's own check-in, in its queue: | its stamp */
#define NT_SMART 8u    /* SmartStart's listen mode on or off (AddNode without a callback) */
#define NT_JAM   9u    /* GetBackgroundRSSI (§9) */
#define NT_POWER 10u   /* §12 power: | 1 classic read, 2 Long Range read, 3 a Set */

/* zw_health's keys for what zw_net queues (the owner's: 0x01 | CC | command | parameter). */
#define KIND_IV       0x49560000u /* the interview's one question to a device */
#define KIND_REREAD   0x52520000u /* a late wake-up's Interval Get */
#define KIND_CHECK_IN 0x43490000u /* a device's own check-in: Wake Up Interval Set */
#define KIND_REPLY    0x53520000u /* a reply the owner owes a device (a Supervision Report): |
                                   * its stamp, so none replaces another */
#define KIND_OWNER    0x01000000u
/* ZW13: the owner's Multi Channel command to one end point: | end point << 24 (bits 24-30) | the
 * inner CC | command | parameter - so end points never replace each other's commands */
#define KIND_OWNER_EP 0x80000000u
/* ...an encapsulation queue_kind does not look into (one to several end points at once, Multi
 * Instance v1, CRC-16, Supervision Get, ...), or end point 127's FF FF FF (the ping's tag): | a
 * stamp, so it replaces nothing */
#define KIND_OWNER_EPS 0x02000000u

/* A queued RequestNodeInfo: this in the class byte. */
#define QCLS_NIF 0xFEu

uint32_t zw_net_tag(unsigned what, uint32_t low);
bool zw_net_is_own(uint32_t tag, unsigned what);

/* zw_net_nodes.c */
void zw_net_nodes_init(zw_net_t *net);
void zw_net_check(zw_net_t *net);
void zw_net_link_lost(zw_net_t *net);
void zw_net_nodes_poll(zw_net_t *net);
uint64_t zw_net_nodes_deadline(const zw_net_t *net);
void zw_net_received(zw_net_t *net, const zw_s2_rx_t *rx);
void zw_net_heard(zw_net_t *net, uint16_t node);
/* zw_net.c: a node left or joined: its signal readings go. */
void zw_net_signal_forget(zw_net_t *net, uint16_t node);
/* A sleeping device listens now (it is owed a reply): as a wake-up, its queue goes. */
void zw_net_woke(zw_net_t *net, uint16_t node);
bool zw_net_app_update(zw_net_t *net, const zw_frame_t *frame);
bool zw_net_own_send_done(zw_net_t *net, uint32_t tag, zw_s2_result_t result);
void zw_net_frame_sent(zw_net_t *net, uint32_t s2tag, uint16_t node);
void zw_net_frame_done(zw_net_t *net, uint32_t s2tag, bool delivered);
void zw_net_incl_event(zw_net_t *net, const zw_incl_event_t *ev);
void zw_net_flush(zw_net_t *net);
void zw_net_emit(zw_net_t *net, zw_net_event_t *ev);
bool zw_net_sleeping(const zw_net_t *net, uint16_t node);
/* Health counts the device awake now (a sleeper during its wake-up, or a listening device). */
bool zw_net_counts_awake(const zw_net_t *net, uint16_t node);
zw_s2_result_t zw_net_queue_send(zw_net_t *net, uint16_t node, zw_sec_class_t cls,
                                 const uint8_t *cmd, size_t len, uint32_t tag, uint32_t kind,
                                 bool interview, bool *held);
bool zw_net_fresh_keys(zw_net_t *net);
void zw_net_bind_keys(zw_net_t *net, uint32_t home_id);
void zw_net_forget_all(zw_net_t *net);
void zw_net_run_gone(zw_net_t *net);
zw_nid_width_t zw_net_nid(const zw_net_t *net);
/* §4: the other nodes' interviews wait while a device joins. */
void zw_net_quiet(zw_net_t *net);
/* A node to remove as failed when nothing else runs, without NODE_GONE (a SmartStart join that
 * failed its key exchange). */
void zw_net_gone_quietly(zw_net_t *net, uint16_t node);

/* zw_net_routes.c */
void zw_net_routes_init(zw_net_t *net);
void zw_net_routes_woke(zw_net_t *net, uint16_t node);

/* zw_net_power.c */
void zw_net_power_poll(zw_net_t *net);
void zw_net_power_lost(zw_net_t *net);
/* The start-up again, from step 0 (zw_net.c). */
void zw_net_restart_link(zw_net_t *net);
/* Refusals every network operation shares (zw_net.c): 0, or 1 not allowed, 2 link, 3 busy. */
int zw_net_op_refusal(const zw_net_t *net, bool allowed);
zw_net_result_t zw_net_op_result(int refused);

/* zw_net_smart.c */
void zw_net_smart_poll(zw_net_t *net);
void zw_net_smart_pause(zw_net_t *net);
void zw_net_smart_lost(zw_net_t *net);
void zw_net_smart_request(zw_net_t *net, const zw_api_app_update_t *u);
void zw_net_smart_done(zw_net_t *net, const zw_incl_event_t *ev, bool *interview);

#endif /* ZW_NET_INTERNAL_H */
