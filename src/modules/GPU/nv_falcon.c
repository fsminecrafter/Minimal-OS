#include "x86_64/nv_gpu.h"
#include "x86_64/scheduler.h"

/*
 * NVIDIA falcon microcontrollers: state, code/data upload and start.
 * Register usage follows nouveau (nvkm/falcon, gm200 PIO paths).
 */

static inline uint32_t frd(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t reg) {
    return nv_rd32(a, f->base + reg);
}

static inline void fwr(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t reg, uint32_t v) {
    nv_wr32(a, f->base + reg, v);
}

bool nv_falcon_read_state(gpu_adapter_t* a, const nv_falcon_t* f, nv_falcon_state_t* out) {
    *out = (nv_falcon_state_t){ 0 };
    uint32_t hwcfg = frd(a, f, NV_FALCON_HWCFG);
    uint32_t cpuctl = frd(a, f, NV_FALCON_CPUCTL);
    if (nv_pri_error(hwcfg) || nv_pri_error(cpuctl) || hwcfg == 0) return false;
    out->present = true;
    out->hwcfg = hwcfg;
    out->cpuctl = cpuctl;
    out->mailbox0 = frd(a, f, NV_FALCON_MAILBOX0);
    out->mailbox1 = frd(a, f, NV_FALCON_MAILBOX1);
    out->imem_bytes = nv_falcon_imem_bytes(hwcfg);
    out->dmem_bytes = nv_falcon_dmem_bytes(hwcfg);
    out->halted = (cpuctl & NV_FALCON_CPUCTL_HALTED) != 0;
    out->stopped = (cpuctl & NV_FALCON_CPUCTL_STOPPED) != 0;
    return true;
}

static uint32_t word_at(const uint8_t* p, uint32_t i, uint32_t bytes) {
    uint32_t w = 0;
    for (uint32_t k = 0; k < 4; k++)
        if (i + k < bytes) w |= (uint32_t)p[i + k] << (8 * k);
    return w;
}

void nv_falcon_imem_write(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t dst,
                          const void* src, uint32_t bytes, uint32_t tag, bool secure) {
    const uint8_t* p = (const uint8_t*)src;
    fwr(a, f, NV_FALCON_IMEMC(0), (secure ? NV_FALCON_IMEMC_SECURE : 0) | NV_FALCON_MEMC_AINCW | dst);
    // IMEM is filled in 256-byte blocks, each with its tag (virtual page).
    uint32_t padded = (bytes + 255u) & ~255u;
    for (uint32_t i = 0; i < padded; i += 4) {
        if ((i & 255u) == 0) fwr(a, f, NV_FALCON_IMEMT(0), tag++);
        fwr(a, f, NV_FALCON_IMEMD(0), word_at(p, i, bytes));
    }
}

void nv_falcon_dmem_write(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t dst,
                          const void* src, uint32_t bytes) {
    const uint8_t* p = (const uint8_t*)src;
    fwr(a, f, NV_FALCON_DMEMC(0), NV_FALCON_MEMC_AINCW | dst);
    for (uint32_t i = 0; i < bytes; i += 4) fwr(a, f, NV_FALCON_DMEMD(0), word_at(p, i, bytes));
}

void nv_falcon_dmem_read(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t src,
                         void* dst, uint32_t bytes) {
    uint8_t* p = (uint8_t*)dst;
    fwr(a, f, NV_FALCON_DMEMC(0), NV_FALCON_MEMC_AINCR | src);
    for (uint32_t i = 0; i < bytes; i += 4) {
        uint32_t w = frd(a, f, NV_FALCON_DMEMD(0));
        for (uint32_t k = 0; k < 4 && i + k < bytes; k++) p[i + k] = (uint8_t)(w >> (8 * k));
    }
}

void nv_falcon_start(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t boot_vector) {
    fwr(a, f, NV_FALCON_BOOTVEC, boot_vector);
    // Falcons that have been through secure boot only accept the alias.
    if (frd(a, f, NV_FALCON_CPUCTL) & NV_FALCON_CPUCTL_ALIAS_EN)
        fwr(a, f, NV_FALCON_CPUCTL_ALIAS, NV_FALCON_CPUCTL_STARTCPU);
    else
        fwr(a, f, NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_STARTCPU);
}

bool nv_falcon_wait_halted(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t timeout_ms) {
    for (uint32_t t = 0; t <= timeout_ms; t++) {
        uint32_t cpuctl = frd(a, f, NV_FALCON_CPUCTL);
        if (!nv_pri_error(cpuctl) && (cpuctl & NV_FALCON_CPUCTL_HALTED)) return true;
        sleep(1);
    }
    return false;
}

static const nv_falcon_t k_turing[] = {
    { "GSP", NV_FALCON_GSP },   { "SEC2", NV_FALCON_SEC2 }, { "PMU", NV_FALCON_PMU },
    { "FECS", NV_FALCON_FECS }, { "GPCCS", NV_FALCON_GPCCS }, { "NVDEC", NV_FALCON_NVDEC },
    { NULL, 0 },
};

static const nv_falcon_t k_pascal[] = {
    { "SEC2", NV_FALCON_SEC2 }, { "PMU", NV_FALCON_PMU }, { "FECS", NV_FALCON_FECS },
    { "GPCCS", NV_FALCON_GPCCS }, { "NVDEC", NV_FALCON_NVDEC },
    { NULL, 0 },
};

const nv_falcon_t* nv_falcons(uint16_t chipset) {
    if (chipset >= 0x160) return k_turing;
    if (chipset >= 0x132) return k_pascal;
    return NULL;
}
