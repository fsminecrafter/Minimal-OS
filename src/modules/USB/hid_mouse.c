#include "mouse/hid_mouse.h"

/* Short items (HID 1.11, 6.2.2.2): prefix = tag(4) type(2) size(2). */
#define TYPE_MAIN 0
#define TYPE_GLOBAL 1
#define TYPE_LOCAL 2

#define MAIN_INPUT 0x8
#define MAIN_COLLECTION 0xA
#define MAIN_END_COLLECTION 0xC
#define GLOBAL_USAGE_PAGE 0x0
#define GLOBAL_LOGICAL_MIN 0x1
#define GLOBAL_REPORT_SIZE 0x7
#define GLOBAL_REPORT_ID 0x8
#define GLOBAL_REPORT_COUNT 0x9
#define GLOBAL_PUSH 0xA
#define GLOBAL_POP 0xB
#define LOCAL_USAGE 0x0
#define LOCAL_USAGE_MIN 0x1
#define LOCAL_USAGE_MAX 0x2

#define PAGE_DESKTOP 0x01
#define PAGE_BUTTON 0x09
#define USAGE_MOUSE 0x02
#define USAGE_POINTER 0x01
#define USAGE_X 0x30
#define USAGE_Y 0x31
#define USAGE_WHEEL 0x38

#define MAX_USAGES 16
#define MAX_IDS 16

typedef struct {
    uint32_t page;
    int32_t logical_min;
    uint32_t size, count, id;
} globals_t;

bool hid_mouse_parse(const uint8_t* desc, uint32_t length, hid_mouse_layout_t* out) {
    hid_mouse_layout_t found = {0};
    globals_t g = {0}, stack[4];
    uint32_t depth = 0;
    uint32_t usages[MAX_USAGES], usage_count = 0, usage_min = 0, usage_max = 0;
    bool have_range = false;
    // Input bits seen so far, per report ID.
    uint8_t ids[MAX_IDS];
    uint32_t bits[MAX_IDS], id_count = 0;
    // Inside a Mouse (or Pointer) application collection.
    uint32_t collection_depth = 0, mouse_depth = 0;
    bool x_found = false;

    for (uint32_t at = 0; at < length;) {
        uint8_t prefix = desc[at];
        if (prefix == 0xFE) {                       // long item: skip
            if (at + 2 >= length) break;
            at += 3u + desc[at + 1];
            continue;
        }
        uint32_t size = prefix & 3u;
        if (size == 3) size = 4;
        if (at + 1 + size > length) break;
        uint32_t value = 0;
        for (uint32_t i = 0; i < size; i++) value |= (uint32_t)desc[at + 1 + i] << (8 * i);
        int32_t svalue = size == 1 ? (int8_t)value : size == 2 ? (int16_t)value : (int32_t)value;
        uint32_t type = (prefix >> 2) & 3u, tag = prefix >> 4;
        at += 1 + size;

        if (type == TYPE_GLOBAL) {
            switch (tag) {
                case GLOBAL_USAGE_PAGE: g.page = value; break;
                case GLOBAL_LOGICAL_MIN: g.logical_min = svalue; break;
                case GLOBAL_REPORT_SIZE: g.size = value; break;
                case GLOBAL_REPORT_COUNT: g.count = value; break;
                case GLOBAL_REPORT_ID: g.id = value; break;
                case GLOBAL_PUSH: if (depth < 4) stack[depth++] = g; break;
                case GLOBAL_POP: if (depth) g = stack[--depth]; break;
            }
            continue;
        }
        if (type == TYPE_LOCAL) {
            // A 4-byte usage carries its own page in the top half.
            uint32_t usage = size == 4 ? value : (g.page << 16) | value;
            if (tag == LOCAL_USAGE && usage_count < MAX_USAGES) usages[usage_count++] = usage;
            else if (tag == LOCAL_USAGE_MIN) { usage_min = usage; have_range = true; }
            else if (tag == LOCAL_USAGE_MAX) { usage_max = usage; have_range = true; }
            continue;
        }
        if (type != TYPE_MAIN) continue;

        if (tag == MAIN_COLLECTION) {
            collection_depth++;
            uint32_t usage = usage_count ? usages[0] : 0;
            if (value == 1 && !mouse_depth &&                   // application
                (usage == ((PAGE_DESKTOP << 16) | USAGE_MOUSE) ||
                 usage == ((PAGE_DESKTOP << 16) | USAGE_POINTER)))
                mouse_depth = collection_depth;
        } else if (tag == MAIN_END_COLLECTION) {
            if (collection_depth == mouse_depth) {
                mouse_depth = 0;
                if (x_found) break;                             // first mouse is enough
            }
            if (collection_depth) collection_depth--;
        } else if (tag == MAIN_INPUT) {
            uint32_t slot = 0;
            while (slot < id_count && ids[slot] != (uint8_t)g.id) slot++;
            if (slot == id_count && id_count < MAX_IDS) {
                ids[id_count] = (uint8_t)g.id;
                bits[id_count++] = 0;
            }
            uint32_t* offset = slot < MAX_IDS ? &bits[slot] : &bits[0];
            bool constant = value & 1u, variable = value & 2u, relative = value & 4u;
            for (uint32_t i = 0; i < g.count; i++) {
                uint32_t bit = *offset + i * g.size;
                if (constant || !variable || !mouse_depth || bit > 0xFFFF) continue;
                if (x_found && (uint8_t)g.id != found.report_id) continue;
                uint32_t usage = 0;
                if (have_range) usage = usage_min + i <= usage_max ? usage_min + i : usage_max;
                else if (usage_count) usage = usages[i < usage_count ? i : usage_count - 1];
                hid_field_t field = { (uint16_t)bit, (uint8_t)(g.size > 32 ? 32 : g.size),
                                      g.logical_min < 0 };
                if (usage == ((PAGE_DESKTOP << 16) | USAGE_X) && relative && !x_found) {
                    found.x = field;
                    found.report_id = (uint8_t)g.id;
                    x_found = true;
                } else if (usage == ((PAGE_DESKTOP << 16) | USAGE_Y) && relative) {
                    if (!found.y.size) found.y = field;
                } else if (usage == ((PAGE_DESKTOP << 16) | USAGE_WHEEL) && relative) {
                    if (!found.wheel.size) found.wheel = field;
                } else if ((usage >> 16) == PAGE_BUTTON && g.size == 1 &&
                           (usage & 0xFFFF) >= 1 && (usage & 0xFFFF) <= 8) {
                    if (!found.button_count) found.button_bit = (uint16_t)bit;
                    if ((usage & 0xFFFF) > found.button_count &&
                        bit == found.button_bit + (usage & 0xFFFF) - 1)
                        found.button_count = (uint8_t)(usage & 0xFFFF);
                }
            }
            *offset += g.count * g.size;
        }
        // Local items last until the next main item.
        usage_count = 0;
        have_range = false;
        usage_min = usage_max = 0;
    }
    found.valid = x_found && found.y.size;
    if (out) *out = found;
    return found.valid;
}

static int32_t read_field(const hid_field_t* f, const uint8_t* data, uint32_t length) {
    if (!f->size) return 0;
    uint32_t value = 0;
    for (uint32_t i = 0; i < f->size; i++) {
        uint32_t bit = f->bit + i;
        if (bit / 8 >= length) return 0;
        value |= (uint32_t)((data[bit / 8] >> (bit % 8)) & 1u) << i;
    }
    if (f->is_signed && f->size < 32 && (value & (1u << (f->size - 1))))
        value |= ~0u << f->size;
    int32_t v = (int32_t)value;
    if (v > 32767) v = 32767;
    if (v < -32767) v = -32767;
    return v;
}

bool hid_mouse_decode(const hid_mouse_layout_t* layout, const uint8_t* report,
                      uint32_t length, uint8_t* buttons, int32_t* dx, int32_t* dy,
                      int32_t* wheel) {
    if (!layout || !layout->valid || !report || !length) return false;
    if (layout->report_id) {
        if (report[0] != layout->report_id) return false;
        report++;
        length--;
    }
    uint8_t b = 0;
    for (uint32_t i = 0; i < layout->button_count; i++) {
        uint32_t bit = layout->button_bit + i;
        if (bit / 8 < length && (report[bit / 8] >> (bit % 8)) & 1u) b |= (uint8_t)(1u << i);
    }
    if (buttons) *buttons = b;
    if (dx) *dx = read_field(&layout->x, report, length);
    if (dy) *dy = read_field(&layout->y, report, length);
    if (wheel) *wheel = read_field(&layout->wheel, report, length);
    return true;
}
