// pulsar_link.h — the Pulsar's vendor GATT service on the BLE link Bluepad32 already holds.
// Story 023: find HostService 7EDF0001-… after connect, subscribe to storage-up, ask STATUS.
// Everything here runs in BTstack's context (core0 async context); never call from core1 or
// from the main loop. The heartbeat string is the one thing read from the main loop.
#ifndef P2M_PULSAR_LINK_H
#define P2M_PULSAR_LINK_H

#include "uni.h"

// Called from the platform's on_device_ready for a BLE device. Discovery decides whether it is
// a Pulsar: a device without the service is left alone (state "not-pulsar").
void pulsar_link_attach(uni_hid_device_t *d);
// Called from the platform's on_device_disconnected; drops the listener and any queued query.
void pulsar_link_detach(uni_hid_device_t *d);

// One-line state for the [hb] heartbeat, e.g. "ready mtu=247 st=f:0b q:16 ep:3 card:1a2b3c4d".
const char *pulsar_link_heartbeat(void);

#endif
