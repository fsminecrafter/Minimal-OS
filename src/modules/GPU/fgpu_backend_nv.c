#include "x86_64/fgpu_backend.h"
#include "x86_64/fgpu_ir.h"
#include "x86_64/nv_gpu.h"
#include "x86_64/minimafs.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "string.h"

/*
 * FGPU hardware backend for NVIDIA Turing (RTX 20 / GTX 16), and the
 * bring-up report behind 'gpu nvidia'.
 *
 * Status:
 *   1. firmware files: checked (nv_fw.c)                         done
 *   2. GSP boot (FWSEC-FRTS, booter_load on SEC2, GSP-RM)        to do
 *   3. RM channel + TURING_A, first clear (nv_push_clear_xrgb)   to do
 *   4. draws: pushbuffers + SASS from the shader IR (fgpu_ir.c)  IR done, encoder to do
 *
 * Until step 2 works, init() declines and everything runs on the CPU
 * backend. Shader programs are already lowered to the IR here, so
 * the compiler runs on real programs as soon as a card is present.
 */

#define IR_CAPACITY 16384u

typedef struct {
    fgpu_ir_inst_t* code;
    uint32_t count;
    uint32_t regs;
} nv_stage_t;

typedef struct {
    nv_stage_t vs, fs;
} nv_program_t;

static bool compile_stage(const fgpu_program_t* p, bool fragment, nv_stage_t* out) {
    fgpu_ir_t ir = { 0 };
    ir.capacity = IR_CAPACITY;
    ir.code = (fgpu_ir_inst_t*)alloc_unzeroed(sizeof(fgpu_ir_inst_t) * IR_CAPACITY);
    ir.hash = (uint32_t*)alloc_unzeroed(sizeof(uint32_t) * FGPU_IR_HASH_WORDS(IR_CAPACITY));
    bool ok = false;
    const char* why = "out of memory";
    if (ir.code && ir.hash && fgpu_ir_lower(&ir, p, fragment, &why)) {
        fgpu_ir_allocate(&ir);
        out->code = (fgpu_ir_inst_t*)alloc_unzeroed(sizeof(fgpu_ir_inst_t) * (ir.count ? ir.count : 1));
        if (out->code) {
            memcpy(out->code, ir.code, sizeof(fgpu_ir_inst_t) * ir.count);
            out->count = ir.count;
            out->regs = ir.reg_count;
            ok = true;
        }
    }
    if (!ok) {
        serial_write_str("nv: shader IR failed: ");
        serial_write_str(why);
        serial_write_str("\n");
    }
    if (ir.code) free_mem(ir.code);
    if (ir.hash) free_mem(ir.hash);
    return ok;
}

static void* nv_program_create(const fgpu_program_t* p) {
    nv_program_t* np = (nv_program_t*)alloc(sizeof(nv_program_t));
    if (!np) return NULL;
    if (!compile_stage(p, false, &np->vs) || !compile_stage(p, true, &np->fs)) {
        if (np->vs.code) free_mem(np->vs.code);
        free_mem(np);
        return NULL;
    }
    // Next: encode np->vs/fs as SASS (see fgpu_ir.h for the mapping).
    return np;
}

static void nv_program_destroy(void* program) {
    nv_program_t* np = (nv_program_t*)program;
    if (!np) return;
    if (np->vs.code) free_mem(np->vs.code);
    if (np->fs.code) free_mem(np->fs.code);
    free_mem(np);
}

static bool is_turing(uint16_t chipset) { return chipset >= 0x160 && chipset < 0x170; }

static bool nv_init(gpu_adapter_t* a, const char** why) {
    if (!a || a->vendor != GPU_VENDOR_NVIDIA || !a->mmio) { *why = "not an NVIDIA adapter"; return false; }
    if (!is_turing(nv_chipset(a))) { *why = "hardware rendering is planned for Turing first"; return false; }
    *why = "NVIDIA GSP boot is not implemented yet (see 'gpu nvidia')";
    return false;
}

static bool nv_clear(const fgpu_hw_target_t* t, uint32_t flags, uint32_t color, float depth) {
    (void)t; (void)flags; (void)color; (void)depth;
    return false;       // step 3: nv_push_clear_xrgb on a channel
}

static bool nv_draw(const fgpu_hw_draw_t* d, uint32_t* triangles) {
    (void)d; (void)triangles;
    return false;       // step 4
}

static void nv_finish(void) {}

const fgpu_backend_t fgpu_backend_nvidia = {
    .name = "nvidia (Turing)",
    .init = nv_init,
    .program_create = nv_program_create,
    .program_destroy = nv_program_destroy,
    .clear = nv_clear,
    .draw = nv_draw,
    .finish = nv_finish,
};

/* ------------------------------------------------------- bring-up report */

static bool fs_read(void* ctx, uint64_t offset, void* buf, uint32_t len) {
    minimafs_file_handle_t* h = (minimafs_file_handle_t*)ctx;
    if (offset > 0xFFFFFFFFull || !minimafs_seek(h, (uint32_t)offset)) return false;
    return minimafs_read(h, buf, len) == len;
}

// Checks one firmware file; returns true if it is usable.
static bool check_file(const char* chip, const nv_fw_entry_t* e, char* result, size_t result_size) {
    char path[160];
    snprintf(path, sizeof(path), NV_FW_DIR "%s/%s", chip, e->file);
    minimafs_file_handle_t* h = minimafs_open(path, true);
    if (!h) {
        snprintf(result, result_size, "missing");
        return false;
    }
    nv_fw_file_t f = { fs_read, h, minimafs_size(h) };
    const char* why = "";
    bool ok = true;
    if (e->kind == NV_FW_KIND_BIN) {
        nv_fw_bin_hdr_t hdr;
        ok = nv_fw_parse_bin(&f, &hdr, &why);
        if (ok) snprintf(result, result_size, "ok, %u bytes of ucode", (unsigned)hdr.data_size);
    } else {
        uint64_t size = 0;
        for (int i = 0; ok && i < 2 && e->sections[i]; i++) {
            ok = nv_fw_elf_section(&f, e->sections[i], NULL, i == 0 ? &size : NULL, &why);
            if (!ok) snprintf(result, result_size, "bad: %s %s", e->sections[i], why);
        }
        if (ok) snprintf(result, result_size, "ok, image %llu KiB", (unsigned long long)(size >> 10));
        why = NULL;
    }
    if (!ok && why) snprintf(result, result_size, "bad: %s", why);
    minimafs_close(h);
    return ok;
}

// `text` padded with spaces to `width` (the kernel's snprintf has no %-Ns).
static const char* padded(const char* text, uint32_t width, char* buf, size_t size) {
    size_t n = 0;
    while (text[n] && n + 1 < size) { buf[n] = text[n]; n++; }
    while (n < width && n + 1 < size) buf[n++] = ' ';
    buf[n] = 0;
    return buf;
}

void nv_bringup_report(gpu_adapter_t* a, void (*print)(const char* text)) {
    char pad[48];
    char line[160];
    uint16_t chip = nv_chipset(a);
    snprintf(line, sizeof(line), "%s, %s (chipset 0x%x)\n", a->name, a->family, (unsigned)chip);
    print(line);
    if (!chip) {
        print("  The registers (BAR0) do not answer; nothing else to check.\n");
        return;
    }

    const char* name = nv_chip_name(chip);
    uint32_t count = 0;
    const nv_fw_entry_t* files = nv_fw_manifest(chip, &count);
    print("\nStep 1  firmware files\n");
    uint32_t good = 0;
    if (!files) {
        print("  No firmware list for this chip yet (Turing comes first).\n");
    } else {
        snprintf(line, sizeof(line), "  From linux-firmware nvidia/%s/, copied to " NV_FW_DIR "%s/\n",
                 name, name);
        print(line);
        for (uint32_t i = 0; i < count; i++) {
            char result[96];
            if (check_file(name, &files[i], result, sizeof(result))) good++;
            snprintf(line, sizeof(line), "  %s %s\n", padded(files[i].file, 36, pad, sizeof(pad)), result);
            print(line);
        }
        snprintf(line, sizeof(line), "  %u of %u present and well-formed\n", (unsigned)good,
                 (unsigned)count);
        print(line);
    }

    print("\nMicrocontrollers (read only)\n");
    const nv_falcon_t* fl = nv_falcons(chip);
    for (; fl && fl->name; fl++) {
        nv_falcon_state_t st;
        if (!nv_falcon_read_state(a, fl, &st)) {
            snprintf(line, sizeof(line), "  %s 0x%06x  not reachable (powered off?)\n",
                     padded(fl->name, 6, pad, sizeof(pad)), (unsigned)fl->base);
        } else {
            char state[12];
            snprintf(line, sizeof(line), "  %s 0x%06x  IMEM %3u KiB  DMEM %3u KiB  %s mailbox 0x%x\n",
                     padded(fl->name, 6, pad, sizeof(pad)), (unsigned)fl->base,
                     (unsigned)(st.imem_bytes >> 10), (unsigned)(st.dmem_bytes >> 10),
                     padded(st.halted ? "halted" : (st.stopped ? "stopped" : "running"), 8,
                            state, sizeof(state)),
                     (unsigned)st.mailbox0);
        }
        print(line);
    }

    print("\nStep 2  boot GSP-RM                       not implemented yet\n");
    print("Step 3  channel + TURING_A, first clear   waits for step 2\n");
    print("Step 4  draws: pushbuffers + shader code  shader IR ready, SASS encoder to do\n");
    const char* why = "";
    const fgpu_backend_t* hw = fgpu_hw_backend(&why);
    snprintf(line, sizeof(line), "\nRendering: %s%s%s\n", hw ? hw->name : "FGPU CPU backend",
             hw ? "" : " - ", hw ? "" : why);
    print(line);
}
