#include "x86_64/hyperv.h"
#include "x86_64/pmm.h"
#include "x86_64/gdt.h"
#include "serial.h"
#include "string.h"

/*
 * Hyper-V hypervisor interface and VMBus.
 *
 * Hypervisor: identify through CPUID, register a guest OS id, enable the
 * hypercall page (the hypervisor writes the VMCALL/VMMCALL stub into it)
 * and the SynIC, whose message page receives VMBus control messages on
 * SINT 2.
 *
 * VMBus: the guest posts control messages to the host with the
 * HvPostMessage hypercall and gets answers in the SynIC message page.
 * After version negotiation the host offers one channel per virtual
 * device. A channel is opened by sharing ring buffer pages with the host
 * (a GPADL) and then carries packets in both directions; the guest
 * signals the host with HvSignalEvent. All of it is polled here.
 */

#define HV_CPUID_VENDOR         0x40000000
#define HV_CPUID_FEATURES       0x40000003

#define HV_MSR_GUEST_OS_ID      0x40000000
#define HV_MSR_HYPERCALL        0x40000001
#define HV_MSR_VP_INDEX         0x40000002
#define HV_MSR_TIME_REF_COUNT   0x40000020
#define HV_MSR_SCONTROL         0x40000080
#define HV_MSR_SIEFP            0x40000082
#define HV_MSR_SIMP             0x40000083
#define HV_MSR_EOM              0x40000084
#define HV_MSR_SINT0            0x40000090

// CPUID 0x40000003 EAX: partition privileges
#define HV_PRIV_TIME_REF        (1u << 1)
#define HV_PRIV_SYNIC           (1u << 2)
#define HV_PRIV_HYPERCALL       (1u << 5)
#define HV_PRIV_VP_INDEX        (1u << 6)

#define HVCALL_POST_MESSAGE     0x005C
#define HVCALL_SIGNAL_EVENT     0x005D
#define HV_HYPERCALL_FAST       (1ull << 16)
#define HV_STATUS_SUCCESS       0
#define HV_STATUS_INSUFFICIENT_BUFFERS 19

#define VMBUS_SINT              2
#define VMBUS_SINT_VECTOR       0xF3
#define HV_SINT_AUTO_EOI        (1ull << 17)

#define HVMSG_NONE              0
#define HV_MSG_FLAG_PENDING     1

// VMBus control messages
enum {
    CHANNELMSG_OFFERCHANNEL = 1,
    CHANNELMSG_RESCIND_CHANNELOFFER = 2,
    CHANNELMSG_REQUESTOFFERS = 3,
    CHANNELMSG_ALLOFFERS_DELIVERED = 4,
    CHANNELMSG_OPENCHANNEL = 5,
    CHANNELMSG_OPENCHANNEL_RESULT = 6,
    CHANNELMSG_GPADL_HEADER = 8,
    CHANNELMSG_GPADL_CREATED = 10,
    CHANNELMSG_INITIATE_CONTACT = 14,
    CHANNELMSG_VERSION_RESPONSE = 15,
};

#define VMBUS_MESSAGE_CONNECTION_ID   1
#define VMBUS_MESSAGE_CONNECTION_ID_4 4
#define VMBUS_VERSION_WIN10_V5  0x00050000u

#define VMBUS_MAX_CHANNELS      48

/* ------------------------------------------------------------- helpers */

static inline void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static inline void mb(void) { __asm__ volatile("mfence" ::: "memory"); }

static void log_hex(const char* label, uint64_t v) {
    serial_write_str(label);
    serial_write_str("0x");
    serial_write_hex(v);
    serial_write_str("\n");
}

/* ---------------------------------------------------------- hypervisor */

typedef struct __attribute__((packed)) {
    uint32_t message_type;
    uint8_t payload_size;
    uint8_t message_flags;
    uint8_t reserved[2];
    uint64_t sender;
    uint8_t payload[240];
} hv_message_t;

typedef struct __attribute__((packed)) {
    uint32_t connection_id;
    uint32_t reserved;
    uint32_t message_type;
    uint32_t payload_size;
    uint8_t payload[240];
} hv_post_message_input_t;

static bool g_hv_checked, g_hv_present, g_hv_ready;
static uint32_t g_hv_privileges;
static void* g_hypercall_page;
static volatile hv_message_t* g_simp;   // 16 slots, one per SINT
static void* g_siefp;
static hv_post_message_input_t* g_post_input;
static uint32_t g_vp_index;

bool hv_present(void) {
    if (g_hv_checked) return g_hv_present;
    g_hv_checked = true;
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(c & (1u << 31))) return false;            // no hypervisor at all
    cpuid(HV_CPUID_VENDOR, &a, &b, &c, &d);
    // "Microsoft Hv"
    g_hv_present = a >= HV_CPUID_FEATURES && b == 0x7263694D && c == 0x666F736F &&
                   d == 0x76482074;
    if (g_hv_present) {
        cpuid(HV_CPUID_FEATURES, &a, &b, &c, &d);
        g_hv_privileges = a;
    }
    return g_hv_present;
}

uint64_t hv_time_100ns(void) {
    if (!(g_hv_privileges & HV_PRIV_TIME_REF)) return 0;
    return rdmsr(HV_MSR_TIME_REF_COUNT);
}

uint32_t hv_vp_index(void) { return g_vp_index; }

// Monotonic milliseconds for timeouts. Falls back to a loop counter so a
// missing reference counter cannot hang a wait forever.
static uint64_t g_fallback_ticks;
static uint64_t hv_now_ms(void) {
    if (g_hv_privileges & HV_PRIV_TIME_REF) return rdmsr(HV_MSR_TIME_REF_COUNT) / 10000;
    for (volatile int i = 0; i < 20000; i++) __asm__ volatile("pause");
    return ++g_fallback_ticks / 10;
}

static uint64_t hv_hypercall(uint64_t control, uint64_t input, uint64_t output) {
    uint64_t status;
    register uint64_t r8 __asm__("r8") = output;
    __asm__ volatile("call *%[page]"
                     : "=a"(status), "+c"(control), "+d"(input), "+r"(r8)
                     : [page] "m"(g_hypercall_page)
                     : "cc", "memory", "r9", "r10", "r11");
    return status;
}

static uint16_t hv_post_message(uint32_t connection_id, const void* payload, uint32_t size) {
    if (size > sizeof(g_post_input->payload)) return 0xFFFF;
    uint16_t status = 0xFFFF;
    for (int attempt = 0; attempt < 100; attempt++) {
        memset(g_post_input, 0, sizeof(*g_post_input));
        g_post_input->connection_id = connection_id;
        g_post_input->message_type = 1;              // VMBus message
        g_post_input->payload_size = size;
        memcpy(g_post_input->payload, payload, size);
        status = (uint16_t)hv_hypercall(HVCALL_POST_MESSAGE,
                                        (uint64_t)(uintptr_t)g_post_input, 0);
        if (status != HV_STATUS_INSUFFICIENT_BUFFERS) break;
        uint64_t until = hv_now_ms() + 1;
        while (hv_now_ms() < until) __asm__ volatile("pause");
    }
    return status;
}

static uint16_t hv_signal_event(uint32_t connection_id) {
    return (uint16_t)hv_hypercall(HVCALL_SIGNAL_EVENT | HV_HYPERCALL_FAST, connection_id, 0);
}

extern void isr_spurious(void);   // a bare iretq
extern void idt_set_entry(uint8_t vector, uint64_t isr_addr, uint16_t selector,
                          uint8_t type_attr, uint8_t ist_index);

bool hv_init(void) {
    if (g_hv_ready) return true;
    if (!hv_present()) return false;
    uint32_t need = HV_PRIV_SYNIC | HV_PRIV_HYPERCALL;
    if ((g_hv_privileges & need) != need) {
        log_hex("hyperv: missing privileges, have ", g_hv_privileges);
        return false;
    }

    // Open-source guest id: bit 63, OS type 0x7F ("other"), version 1.
    if (!rdmsr(HV_MSR_GUEST_OS_ID)) wrmsr(HV_MSR_GUEST_OS_ID, (1ull << 63) | (0x7Full << 56) | 1);

    g_hypercall_page = alloc_page_zeroed();
    g_simp = (volatile hv_message_t*)alloc_page_zeroed();
    g_siefp = alloc_page_zeroed();
    g_post_input = (hv_post_message_input_t*)alloc_page_zeroed();
    if (!g_hypercall_page || !g_simp || !g_siefp || !g_post_input) return false;

    uint64_t hc = rdmsr(HV_MSR_HYPERCALL);
    wrmsr(HV_MSR_HYPERCALL, (hc & 0xFFE) | (uint64_t)(uintptr_t)g_hypercall_page | 1);
    if (!(rdmsr(HV_MSR_HYPERCALL) & 1)) {
        serial_write_str("hyperv: hypercall page not enabled\n");
        return false;
    }
    g_vp_index = (g_hv_privileges & HV_PRIV_VP_INDEX) ? (uint32_t)rdmsr(HV_MSR_VP_INDEX) : 0;

    // SynIC. Messages are polled; the SINT still needs a vector, and the
    // interrupt is auto-EOI so its handler can be a plain iretq.
    idt_set_entry(VMBUS_SINT_VECTOR, (uint64_t)(uintptr_t)isr_spurious,
                  GDT_SELECTOR_CS_KERNEL, 0x8E, 0);
    wrmsr(HV_MSR_SIMP, (rdmsr(HV_MSR_SIMP) & 0xFFE) | (uint64_t)(uintptr_t)g_simp | 1);
    wrmsr(HV_MSR_SIEFP, (rdmsr(HV_MSR_SIEFP) & 0xFFE) | (uint64_t)(uintptr_t)g_siefp | 1);
    wrmsr(HV_MSR_SINT0 + VMBUS_SINT, VMBUS_SINT_VECTOR | HV_SINT_AUTO_EOI);
    wrmsr(HV_MSR_SCONTROL, rdmsr(HV_MSR_SCONTROL) | 1);

    g_hv_ready = true;
    serial_write_str("hyperv: hypercalls and SynIC ready, VP ");
    serial_write_dec(g_vp_index);
    serial_write_str("\n");
    return true;
}

/* --------------------------------------------------------- VMBus core */

static vmbus_channel_t g_channels[VMBUS_MAX_CHANNELS];
static uint32_t g_channel_count;
static bool g_vmbus_ready;
static uint32_t g_vmbus_version;
static uint32_t g_msg_conn_id = VMBUS_MESSAGE_CONNECTION_ID;
static uint32_t g_next_gpadl = 0xE1E10;
static uint8_t* g_int_page;          // second half: guest -> host event bits
static void* g_monitor_pages;        // two pages

uint32_t vmbus_channel_count(void) { return g_channel_count; }
vmbus_channel_t* vmbus_channel_get(uint32_t i) { return i < g_channel_count ? &g_channels[i] : NULL; }

static void vmbus_store_offer(const uint8_t* m, uint32_t size) {
    // m: payload after the 8-byte message header
    if (size < 8 + 188 || g_channel_count >= VMBUS_MAX_CHANNELS) return;
    const uint8_t* o = m + 8;
    uint16_t sub_index;
    memcpy(&sub_index, o + 172, 2);
    if (sub_index != 0) return;                  // only primary channels
    uint32_t relid;
    memcpy(&relid, o + 176, 4);
    for (uint32_t i = 0; i < g_channel_count; i++) {
        if (g_channels[i].relid == relid) return;
    }
    vmbus_channel_t* ch = &g_channels[g_channel_count++];
    memset(ch, 0, sizeof(*ch));
    memcpy(ch->type.b, o, 16);
    memcpy(ch->instance.b, o + 16, 16);
    memcpy(&ch->mmio_megabytes, o + 50, 2);
    memcpy(ch->user_def, o + 52, 120);
    ch->relid = relid;
    memcpy(&ch->connection_id, o + 184, 4);
    if (g_vmbus_version < 0x00020004) ch->connection_id = 2;  // pre-Win8 hosts
}

// Takes one control message off SINT 2. Returns its VMBus message type
// (0: nothing waiting) and copies up to 240 bytes into `out`.
static uint32_t vmbus_poll_message(uint8_t* out, uint32_t* size) {
    volatile hv_message_t* slot = &g_simp[VMBUS_SINT];
    if (slot->message_type == HVMSG_NONE) return 0;
    mb();
    uint32_t n = slot->payload_size;
    if (n > 240) n = 240;
    for (uint32_t i = 0; i < n; i++) out[i] = slot->payload[i];
    *size = n;
    uint8_t flags = slot->message_flags;
    slot->message_type = HVMSG_NONE;
    mb();
    if (flags & HV_MSG_FLAG_PENDING) wrmsr(HV_MSR_EOM, 0);
    uint32_t type;
    memcpy(&type, out, 4);
    if (type == CHANNELMSG_OFFERCHANNEL) vmbus_store_offer(out, n);
    return type ? type : 0xFFFFFFFF;
}

// Waits for a message of type `want`; offers arriving meanwhile are stored.
static bool vmbus_wait(uint32_t want, uint8_t* out, uint32_t timeout_ms) {
    uint64_t deadline = hv_now_ms() + timeout_ms;
    uint8_t buf[240];
    while (hv_now_ms() < deadline) {
        uint32_t size = 0;
        uint32_t type = vmbus_poll_message(buf, &size);
        if (!type) { __asm__ volatile("pause"); continue; }
        if (type == want) {
            memcpy(out, buf, size);
            if (size < 240) memset(out + size, 0, 240 - size);
            return true;
        }
    }
    return false;
}

static bool vmbus_connect(uint32_t version) {
    uint8_t msg[40];
    memset(msg, 0, sizeof(msg));
    uint32_t type = CHANNELMSG_INITIATE_CONTACT;
    memcpy(msg, &type, 4);
    memcpy(msg + 8, &version, 4);
    memcpy(msg + 12, &g_vp_index, 4);
    uint32_t conn = VMBUS_MESSAGE_CONNECTION_ID;
    if (version >= VMBUS_VERSION_WIN10_V5) {
        msg[16] = VMBUS_SINT;                        // msg_sint, msg_vtl 0
        conn = VMBUS_MESSAGE_CONNECTION_ID_4;
    } else {
        uint64_t ip = (uint64_t)(uintptr_t)g_int_page;
        memcpy(msg + 16, &ip, 8);
    }
    uint64_t m1 = (uint64_t)(uintptr_t)g_monitor_pages;
    uint64_t m2 = m1 + 4096;
    memcpy(msg + 24, &m1, 8);
    memcpy(msg + 32, &m2, 8);

    uint16_t st = hv_post_message(conn, msg, sizeof(msg));
    if (st != HV_STATUS_SUCCESS) {
        serial_write_str("vmbus: InitiateContact post failed, status ");
        serial_write_dec(st);
        serial_write_str("\n");
        return false;
    }
    uint8_t resp[240];
    if (!vmbus_wait(CHANNELMSG_VERSION_RESPONSE, resp, 2000)) {
        serial_write_str("vmbus: no version response\n");
        return false;
    }
    if (!resp[8]) return false;                      // version not supported
    g_vmbus_version = version;
    g_msg_conn_id = conn;
    if (version >= VMBUS_VERSION_WIN10_V5) {
        uint32_t id;
        memcpy(&id, resp + 12, 4);
        if (id) g_msg_conn_id = id;
    }
    return true;
}

bool vmbus_init(void) {
    if (g_vmbus_ready) return true;
    if (!hv_init()) return false;
    g_int_page = (uint8_t*)alloc_page_zeroed();
    g_monitor_pages = alloc_pages_zeroed(2);
    if (!g_int_page || !g_monitor_pages) return false;

    static const uint32_t versions[] = {
        0x00050000, 0x00040001, 0x00040000, 0x00030000, 0x00020004,
    };
    bool ok = false;
    for (uint32_t i = 0; i < sizeof(versions) / sizeof(versions[0]) && !ok; i++) {
        ok = vmbus_connect(versions[i]);
    }
    if (!ok) {
        serial_write_str("vmbus: could not connect\n");
        return false;
    }
    serial_write_str("vmbus: connected, protocol ");
    serial_write_dec(g_vmbus_version >> 16);
    serial_write_str(".");
    serial_write_dec(g_vmbus_version & 0xFFFF);
    serial_write_str("\n");

    uint8_t req[8] = { CHANNELMSG_REQUESTOFFERS };
    if (hv_post_message(g_msg_conn_id, req, sizeof(req)) != HV_STATUS_SUCCESS) {
        serial_write_str("vmbus: RequestOffers failed\n");
        return false;
    }
    uint8_t resp[240];
    if (!vmbus_wait(CHANNELMSG_ALLOFFERS_DELIVERED, resp, 5000)) {
        serial_write_str("vmbus: offers incomplete (timeout)\n");
    }
    g_vmbus_ready = true;

    for (uint32_t i = 0; i < g_channel_count; i++) {
        char guid[37];
        hv_guid_format(&g_channels[i].type, guid);
        serial_write_str("vmbus: offer relid ");
        serial_write_dec(g_channels[i].relid);
        serial_write_str(" ");
        serial_write_str(vmbus_class_name(&g_channels[i].type));
        serial_write_str(" {");
        serial_write_str(guid);
        serial_write_str("}\n");
    }
    return true;
}

/* ------------------------------------------------------------ channels */

static bool vmbus_create_gpadl(vmbus_channel_t* ch, void* base, uint32_t pages, uint32_t* handle) {
    // Everything has to fit into the header message: 26 page numbers.
    if (pages == 0 || pages > 26) return false;
    uint8_t msg[240];
    memset(msg, 0, sizeof(msg));
    uint32_t type = CHANNELMSG_GPADL_HEADER;
    uint32_t gpadl = g_next_gpadl++;
    uint16_t range_len = (uint16_t)(8 + pages * 8);
    uint16_t range_count = 1;
    uint32_t byte_count = pages * 4096, byte_offset = 0;
    memcpy(msg, &type, 4);
    memcpy(msg + 8, &ch->relid, 4);
    memcpy(msg + 12, &gpadl, 4);
    memcpy(msg + 16, &range_len, 2);
    memcpy(msg + 18, &range_count, 2);
    memcpy(msg + 20, &byte_count, 4);
    memcpy(msg + 24, &byte_offset, 4);
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t pfn = ((uint64_t)(uintptr_t)base >> 12) + i;
        memcpy(msg + 28 + i * 8, &pfn, 8);
    }
    if (hv_post_message(g_msg_conn_id, msg, 28 + pages * 8) != HV_STATUS_SUCCESS) return false;

    uint64_t deadline = hv_now_ms() + 3000;
    while (hv_now_ms() < deadline) {
        uint8_t resp[240];
        if (!vmbus_wait(CHANNELMSG_GPADL_CREATED, resp, 3000)) break;
        uint32_t relid, id, status;
        memcpy(&relid, resp + 8, 4);
        memcpy(&id, resp + 12, 4);
        memcpy(&status, resp + 16, 4);
        if (relid != ch->relid || id != gpadl) continue;
        if (status != 0) {
            log_hex("vmbus: GPADL refused, status ", status);
            return false;
        }
        *handle = gpadl;
        return true;
    }
    serial_write_str("vmbus: no GPADL answer\n");
    return false;
}

typedef struct __attribute__((packed)) {
    uint32_t write_index;
    uint32_t read_index;
    uint32_t interrupt_mask;
    uint32_t pending_send_size;
    uint32_t reserved[12];
    uint32_t feature_bits;
} vmbus_ring_header_t;     // padded to a page; data follows at +4096

bool vmbus_channel_open(vmbus_channel_t* ch, uint32_t send_pages, uint32_t recv_pages) {
    if (!ch || ch->open) return ch && ch->open;
    if (send_pages < 2 || recv_pages < 2) return false;
    uint32_t total = send_pages + recv_pages;
    ch->rings = (uint8_t*)pmm_try_alloc_pages_zeroed(total);
    if (!ch->rings) return false;
    ch->send_pages = send_pages;
    ch->recv_pages = recv_pages;
    if (!vmbus_create_gpadl(ch, ch->rings, total, &ch->gpadl)) return false;

    uint8_t msg[148];
    memset(msg, 0, sizeof(msg));
    uint32_t type = CHANNELMSG_OPENCHANNEL;
    memcpy(msg, &type, 4);
    memcpy(msg + 8, &ch->relid, 4);
    memcpy(msg + 12, &ch->relid, 4);             // open id
    memcpy(msg + 16, &ch->gpadl, 4);
    memcpy(msg + 20, &g_vp_index, 4);
    memcpy(msg + 24, &send_pages, 4);            // receive ring's page offset
    if (hv_post_message(g_msg_conn_id, msg, sizeof(msg)) != HV_STATUS_SUCCESS) return false;

    uint8_t resp[240];
    uint64_t deadline = hv_now_ms() + 3000;
    while (hv_now_ms() < deadline) {
        if (!vmbus_wait(CHANNELMSG_OPENCHANNEL_RESULT, resp, 3000)) break;
        uint32_t relid, status;
        memcpy(&relid, resp + 8, 4);
        memcpy(&status, resp + 16, 4);
        if (relid != ch->relid) continue;
        if (status != 0) {
            log_hex("vmbus: open refused, status ", status);
            return false;
        }
        ch->open = true;
        return true;
    }
    serial_write_str("vmbus: no open answer\n");
    return false;
}

static void vmbus_signal(vmbus_channel_t* ch) {
    // Event bit for hosts that look at the interrupt page, then the
    // signal itself.
    uint8_t* send_bits = g_int_page + 2048;
    __atomic_fetch_or(&send_bits[ch->relid / 8], (uint8_t)(1u << (ch->relid % 8)),
                      __ATOMIC_SEQ_CST);
    hv_signal_event(ch->connection_id);
}

static void ring_copy_in(uint8_t* data, uint32_t size, uint32_t pos, const void* src, uint32_t len) {
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < len; i++) data[(pos + i) % size] = s[i];
}

static void ring_copy_out(const uint8_t* data, uint32_t size, uint32_t pos, void* dst, uint32_t len) {
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t i = 0; i < len; i++) d[i] = data[(pos + i) % size];
}

bool vmbus_send(vmbus_channel_t* ch, const void* payload, uint32_t len,
                uint64_t trans_id, bool want_completion) {
    if (!ch || !ch->open) return false;
    volatile vmbus_ring_header_t* hdr = (volatile vmbus_ring_header_t*)ch->rings;
    uint8_t* data = ch->rings + 4096;
    uint32_t size = (ch->send_pages - 1) * 4096;

    uint32_t packet_len = 16 + len;
    uint32_t aligned = (packet_len + 7) & ~7u;
    uint32_t total = aligned + 8;
    uint32_t w = hdr->write_index, r = hdr->read_index;
    uint32_t space = (w >= r) ? size - (w - r) : r - w;
    if (total >= space) return false;

    uint8_t desc[16];
    uint16_t type = VMBUS_PKT_INBAND, offset8 = 2, len8 = (uint16_t)(aligned / 8);
    uint16_t flags = want_completion ? VMBUS_PKT_FLAG_WANT_COMPLETION : 0;
    memcpy(desc, &type, 2);
    memcpy(desc + 2, &offset8, 2);
    memcpy(desc + 4, &len8, 2);
    memcpy(desc + 6, &flags, 2);
    memcpy(desc + 8, &trans_id, 8);

    uint32_t pos = w;
    ring_copy_in(data, size, pos, desc, 16);
    ring_copy_in(data, size, pos + 16, payload, len);
    static const uint8_t zeros[8];
    ring_copy_in(data, size, pos + packet_len, zeros, aligned - packet_len);
    uint64_t prev = (uint64_t)w << 32;
    ring_copy_in(data, size, pos + aligned, &prev, 8);
    mb();
    hdr->write_index = (w + total) % size;
    mb();
    if (!hdr->interrupt_mask) vmbus_signal(ch);
    return true;
}

bool vmbus_recv(vmbus_channel_t* ch, void* buf, uint32_t cap, vmbus_packet_t* pkt) {
    if (!ch || !ch->open) return false;
    uint8_t* ring = ch->rings + ch->send_pages * 4096;
    volatile vmbus_ring_header_t* hdr = (volatile vmbus_ring_header_t*)ring;
    uint8_t* data = ring + 4096;
    uint32_t size = (ch->recv_pages - 1) * 4096;

    uint32_t r = hdr->read_index, w = hdr->write_index;
    if (r == w) return false;
    mb();
    uint8_t desc[16];
    ring_copy_out(data, size, r, desc, 16);
    uint16_t type, offset8, len8, flags;
    memcpy(&type, desc, 2);
    memcpy(&offset8, desc + 2, 2);
    memcpy(&len8, desc + 4, 2);
    memcpy(&flags, desc + 6, 2);
    uint32_t total = (uint32_t)len8 * 8 + 8;
    uint32_t start = (uint32_t)offset8 * 8;
    uint32_t len = len8 * 8u > start ? len8 * 8u - start : 0;
    uint32_t n = len < cap ? len : cap;
    ring_copy_out(data, size, r + start, buf, n);
    if (pkt) {
        pkt->type = type;
        pkt->flags = flags;
        memcpy(&pkt->trans_id, desc + 8, 8);
        pkt->len = n;
    }
    mb();
    hdr->read_index = (r + total) % size;
    mb();
    // The host may be waiting for room to send.
    if (hdr->pending_send_size) vmbus_signal(ch);
    return true;
}

/* ---------------------------------------------------------------- GUIDs */

void hv_guid_format(const hv_guid_t* g, char out[37]) {
    static const char hex[] = "0123456789abcdef";
    // Data1..Data3 are little-endian, the last 8 bytes in order.
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        uint8_t v = g->b[order[i]];
        out[o++] = hex[v >> 4];
        out[o++] = hex[v & 15];
    }
    out[o] = 0;
}

static const struct {
    const char* guid;
    const char* name;
} g_classes[] = {
    { "44c4f61d-4444-4400-9d52-802e27ede19f", "vpci" },
    { "ba6163d9-04a1-4d29-b605-72e2ffb1dc7f", "storage" },
    { "32412632-86cb-44a2-9b5c-50d1417354f5", "ide" },
    { "f8615163-df3e-46c5-913f-f2d2f965ed0e", "network" },
    { "da0a7802-e377-4aac-8e77-0558eb1073f8", "video" },
    { "f912ad6d-2b17-48ea-bd65-f927a61c7684", "keyboard" },
    { "cfa8b69e-5b4a-4cc0-b98b-8ba1a1f3f95a", "mouse" },
    { "57164f39-9115-4e78-ab55-382f3bd5422d", "heartbeat" },
    { "0e0b6031-5213-4934-818b-38d90ced39db", "shutdown" },
    { "9527e630-d0ae-497b-adce-e80ab0175caf", "timesync" },
    { "a9a0f4e7-5a45-4d96-b827-8a841e8c03e6", "kvp" },
    { "35fa2e29-ea23-4236-96ae-3a6ebacba440", "vss" },
    { "525074dc-8985-46e2-8057-a307dc18a502", "dynmem" },
    { "34d14be3-dee4-41c8-9ae7-6b174977c192", "fcopy" },
};

const char* vmbus_class_name(const hv_guid_t* type) {
    char s[37];
    hv_guid_format(type, s);
    for (uint32_t i = 0; i < sizeof(g_classes) / sizeof(g_classes[0]); i++) {
        if (strcmp(s, g_classes[i].guid) == 0) return g_classes[i].name;
    }
    return "other";
}

vmbus_channel_t* vmbus_find(const hv_guid_t* type, vmbus_channel_t* after) {
    uint32_t start = after ? (uint32_t)(after - g_channels) + 1 : 0;
    for (uint32_t i = start; i < g_channel_count; i++) {
        if (memcmp(g_channels[i].type.b, type->b, 16) == 0) return &g_channels[i];
    }
    return NULL;
}

/* ------------------------------------------------------------- bring-up */

static uint32_t g_vpci_devices;

void hyperv_bringup(void) {
    if (!hv_present()) return;
    serial_write_str("hyperv: running under Hyper-V, privileges ");
    serial_write_hex(g_hv_privileges);
    serial_write_str("\n");
    if (!hv_init() || !vmbus_init()) return;
    g_vpci_devices = hv_pci_init();
}

void hyperv_report(void (*print)(const char* text)) {
    char line[160];
    if (!hv_present()) {
        print("Not running under Hyper-V.\n");
        return;
    }
    snprintf(line, sizeof(line), "Hyper-V guest, privileges 0x%x, VP %u\n",
             (unsigned)g_hv_privileges, (unsigned)g_vp_index);
    print(line);
    if (!g_vmbus_ready) {
        print("VMBus: not connected\n");
        return;
    }
    snprintf(line, sizeof(line), "VMBus %u.%u, %u channels, %u passed-through PCI devices\n",
             (unsigned)(g_vmbus_version >> 16), (unsigned)(g_vmbus_version & 0xFFFF),
             (unsigned)g_channel_count, (unsigned)g_vpci_devices);
    print(line);
    for (uint32_t i = 0; i < g_channel_count; i++) {
        char guid[37];
        hv_guid_format(&g_channels[i].type, guid);
        snprintf(line, sizeof(line), "  relid %2u  %s {%s}%s\n", (unsigned)g_channels[i].relid,
                 vmbus_class_name(&g_channels[i].type), guid,
                 g_channels[i].open ? " open" : "");
        print(line);
    }
}
