#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Hyper-V guest support: hypercalls, the synthetic interrupt controller
 * (SynIC), VMBus channels and the virtual PCI bus that carries devices
 * passed through with Discrete Device Assignment (DDA).
 *
 * Reference: Microsoft's Hypervisor Top-Level Functional Specification
 * (TLFS) and Linux (arch/x86/hyperv, drivers/hv, drivers/pci/controller/
 * pci-hyperv.c), which document the VMBus and virtual PCI protocols.
 *
 * Everything here polls; no interrupt handling is needed. All functions
 * are safe to call on bare metal: they return false when Hyper-V is not
 * there.
 */

/* ------------------------------------------------------------ hypervisor */

bool hv_present(void);          // CPUID says "Microsoft Hv"
bool hv_init(void);             // guest ID, hypercall page, SynIC (once)
uint64_t hv_time_100ns(void);   // partition reference time, 100 ns units
uint32_t hv_vp_index(void);     // this CPU's virtual processor index

/* ----------------------------------------------------------------- VMBus */

typedef struct {
    uint8_t b[16];
} hv_guid_t;

typedef struct vmbus_channel {
    hv_guid_t type;             // device class
    hv_guid_t instance;         // this device
    uint32_t relid;             // child relation id
    uint32_t connection_id;     // for signaling the host
    uint16_t mmio_megabytes;
    uint8_t user_def[120];
    bool open;
    // Ring buffers (send ring first, then receive ring), one contiguous run.
    uint8_t* rings;
    uint32_t send_pages, recv_pages;
    uint32_t gpadl;
} vmbus_channel_t;

// Connects to the host and collects the device offers. Call after hv_init.
bool vmbus_init(void);
uint32_t vmbus_channel_count(void);
vmbus_channel_t* vmbus_channel_get(uint32_t index);
// Next channel of a device class after `after` (NULL: first one).
vmbus_channel_t* vmbus_find(const hv_guid_t* type, vmbus_channel_t* after);
// Short name for well-known device classes ("vpci", "storage", ...).
const char* vmbus_class_name(const hv_guid_t* type);

bool vmbus_channel_open(vmbus_channel_t* ch, uint32_t send_pages, uint32_t recv_pages);

#define VMBUS_PKT_INBAND        6
#define VMBUS_PKT_COMPLETION    11
#define VMBUS_PKT_FLAG_WANT_COMPLETION 1

// Queues one in-band packet. False if the ring is full.
bool vmbus_send(vmbus_channel_t* ch, const void* data, uint32_t len,
                uint64_t trans_id, bool want_completion);

typedef struct {
    uint16_t type;
    uint16_t flags;
    uint64_t trans_id;
    uint32_t len;               // payload bytes copied
} vmbus_packet_t;

// Takes the next packet off the receive ring; false if there is none.
// Payloads longer than `cap` are cut (the packet is still consumed).
bool vmbus_recv(vmbus_channel_t* ch, void* buf, uint32_t cap, vmbus_packet_t* pkt);

void hv_guid_format(const hv_guid_t* g, char out[37]);

/* ---------------------------------------------------------- virtual PCI */

// Finds DDA devices on every vPCI channel, assigns their BARs and adds
// them to pci_devices[] on virtual buses (bus number >= PCI_VBUS_FIRST),
// so the normal drivers find them. Returns the number of devices added.
uint32_t hv_pci_init(void);

/* ------------------------------------------------------- whole bring-up */

// hv_init + vmbus_init + hv_pci_init, logging to serial. Call once after
// pci_enumerate_all() and before drivers look for devices.
void hyperv_bringup(void);
// Prints Hyper-V / VMBus / vPCI state ('hyperv' command).
void hyperv_report(void (*print)(const char* text));
