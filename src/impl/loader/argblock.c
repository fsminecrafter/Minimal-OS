#include "x86_64/loader/argblock.h"

static size_t ab_strlen(const char* s) {
    size_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}

uint32_t argblock_env_count(const char* env, uint32_t env_len) {
    if (!env) return 0;
    uint32_t count = 0;
    uint32_t i = 0;
    while (i < env_len) {
        uint32_t start = i;
        while (i < env_len && env[i]) i++;
        if (i > start) count++;
        i++;                                   // the NUL
    }
    return count;
}

size_t argblock_size(uint32_t argc, const char* const* argv,
                     const char* env, uint32_t env_len) {
    uint32_t envc = argblock_env_count(env, env_len);
    if (argc > ARGBLOCK_MAX_ARGS || envc > ARGBLOCK_MAX_ENV) return 0;
    size_t bytes = ((size_t)argc + 1 + (size_t)envc + 1) * sizeof(char*);
    for (uint32_t i = 0; i < argc; i++) bytes += ab_strlen(argv ? argv[i] : NULL) + 1;
    // Environment strings, each with its NUL (empty entries are skipped).
    uint32_t i = 0;
    while (env && i < env_len) {
        uint32_t start = i;
        while (i < env_len && env[i]) i++;
        if (i > start) bytes += (i - start) + 1;
        i++;
    }
    return bytes;
}

bool argblock_build(void* region, size_t region_size, uintptr_t user_base,
                    uint32_t argc, const char* const* argv,
                    const char* env, uint32_t env_len,
                    char*** out_argv, char*** out_envp) {
    if (!region || !out_argv || !out_envp) return false;
    size_t need = argblock_size(argc, argv, env, env_len);
    if (need == 0 || need > region_size) return false;

    uint32_t envc = argblock_env_count(env, env_len);
    uint8_t* base = (uint8_t*)region;
    char** argv_array = (char**)base;
    char** envp_array = argv_array + argc + 1;
    uint8_t* cursor = (uint8_t*)(envp_array + envc + 1);

    for (uint32_t i = 0; i < argc; i++) {
        const char* s = (argv && argv[i]) ? argv[i] : "";
        size_t len = ab_strlen(s) + 1;
        for (size_t k = 0; k < len; k++) cursor[k] = (uint8_t)s[k];
        argv_array[i] = (char*)(user_base + (uintptr_t)(cursor - base));
        cursor += len;
    }
    argv_array[argc] = NULL;

    uint32_t e = 0;
    uint32_t i = 0;
    while (env && i < env_len && e < envc) {
        uint32_t start = i;
        while (i < env_len && env[i]) i++;
        uint32_t len = i - start;
        if (len) {
            for (uint32_t k = 0; k < len; k++) cursor[k] = (uint8_t)env[start + k];
            cursor[len] = 0;
            envp_array[e++] = (char*)(user_base + (uintptr_t)(cursor - base));
            cursor += len + 1;
        }
        i++;
    }
    envp_array[e] = NULL;

    *out_argv = (char**)user_base;
    *out_envp = (char**)(user_base + (uintptr_t)((uint8_t*)envp_array - base));
    return true;
}
