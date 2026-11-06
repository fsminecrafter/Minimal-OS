#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mosmedia.h"
#include "x86_64/media_abi.h"
#include "x86_64/smp_par.h"
#include "x86_64/syscall.h"

extern uint64_t media_syscall(syscall_media_request_t* request);

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        printf("FAIL: %s (line %d)\n", message, __LINE__); \
        failures++; \
    } \
} while (0)

void* alloc(size_t size) { return malloc(size); }
void free_mem(void* pointer) { free(pointer); }
bool syscall_user_range_ok(const void* pointer, size_t length, bool write) {
    (void)write;
    return pointer != NULL && length != 0;
}

typedef struct {
    smp_par_fn function;
    void* context;
    uint32_t parts;
    uint32_t worker;
    atomic_uint* next;
} parallel_job_t;

static void* parallel_worker(void* argument) {
    parallel_job_t* job = (parallel_job_t*)argument;
    for (;;) {
        uint32_t part = atomic_fetch_add(job->next, 1);
        if (part >= job->parts) break;
        job->function(job->context, part, job->worker);
    }
    return NULL;
}

void smp_par_run(smp_par_fn function, void* context, uint32_t parts) {
    if (!parts) return;
    uint32_t count = parts < 4u ? parts : 4u;
    parallel_job_t jobs[4];
    pthread_t threads[3];
    atomic_uint next;
    uint32_t started = 0;
    atomic_init(&next, 0);
    for (uint32_t worker = 0; worker < count; worker++) {
        jobs[worker] = (parallel_job_t){
            .function = function,
            .context = context,
            .parts = parts,
            .worker = worker,
            .next = &next
        };
        if (worker && pthread_create(&threads[started], NULL, parallel_worker,
                                     &jobs[worker]) == 0) started++;
    }
    parallel_worker(&jobs[0]);
    for (uint32_t i = 0; i < started; i++) pthread_join(threads[i], NULL);
}

typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
} memory_file_t;

static bool memory_write(void* context, uint64_t offset, const void* data, size_t size) {
    memory_file_t* file = (memory_file_t*)context;
    if (offset > SIZE_MAX || size > SIZE_MAX - (size_t)offset) return false;
    size_t end = (size_t)offset + size;
    if (end > file->capacity) {
        size_t capacity = end * 2u;
        uint8_t* grown = (uint8_t*)realloc(file->data, capacity);
        if (!grown) return false;
        memset(grown + file->capacity, 0, capacity - file->capacity);
        file->data = grown;
        file->capacity = capacity;
    }
    memcpy(file->data + (size_t)offset, data, size);
    if (end > file->size) file->size = end;
    return true;
}

static bool memory_read(void* context, uint64_t offset, void* data, size_t size) {
    memory_file_t* file = (memory_file_t*)context;
    if (offset > file->size || size > file->size - (size_t)offset) return false;
    memcpy(data, file->data + (size_t)offset, size);
    return true;
}

static void make_frame(uint32_t* pixels, uint32_t width, uint32_t height,
                       uint32_t frame) {
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t red = (x + frame * 17u) & 255u;
            uint32_t green = (y + frame * 13u) & 255u;
            uint32_t blue = (x ^ y ^ (frame * 29u)) & 255u;
            pixels[(size_t)y * width + x] = 0xFF000000u | red << 16 | green << 8 | blue;
        }
    }
}

static bool decode_via_syscall(mvid_reader_t* reader, memory_file_t* file,
                               uint32_t frame_number, uint32_t* pixels) {
    const uint8_t* entry = reader->index + (size_t)frame_number * MVID_INDEX_ENTRY;
    uint64_t offset = mm_get64(entry);
    uint32_t size = mm_get32(entry + 8);
    if (size > reader->frame_cap || !memory_read(file, offset, reader->frame, size))
        return false;

    syscall_media_request_t request = {
        .size = sizeof(request),
        .version = SYS_MEDIA_ABI_VERSION,
        .op = SYS_MEDIA_DECODE_TILES,
        .flags = frame_number ? SYS_MEDIA_HAS_PREVIOUS_FRAME : 0u,
        .width = reader->info.width,
        .height = reader->info.height,
        .frame_bytes = size,
        .frame = reader->frame,
        .pixels = pixels
    };
    return media_syscall(&request) == SYS_SUCCESS;
}

int main(void) {
    const uint32_t width = 256, height = 192, frame_count = 4;
    size_t pixel_count = (size_t)width * height;
    uint32_t* source = (uint32_t*)malloc(pixel_count * frame_count * sizeof(uint32_t));
    uint32_t* decoded = (uint32_t*)calloc(pixel_count, sizeof(uint32_t));
    CHECK(source && decoded, "allocate test frames");
    if (!source || !decoded) return 1;

    memory_file_t file = {0};
    mvid_writer_t writer;
    CHECK(mvid_writer_begin(&writer, width, height, 30, 1, false, frame_count,
                            false, memory_write, &file, malloc, free),
          "start video writer");
    for (uint32_t frame = 0; frame < frame_count; frame++) {
        uint32_t* pixels = source + (size_t)frame * pixel_count;
        make_frame(pixels, width, height, frame);
        CHECK(mvid_writer_frame(&writer, pixels, width), "write source frame");
    }
    CHECK(mvid_writer_finish(&writer, NULL, 0), "finish video writer");

    mvid_reader_t reader = {0};
    CHECK(mvid_open(&reader, memory_read, &file, file.size, malloc), "open encoded video");
    bool all_decoded = true;
    for (uint32_t frame = 0; frame < frame_count; frame++) {
        if (!decode_via_syscall(&reader, &file, frame, decoded)) {
            all_decoded = false;
            break;
        }
        CHECK(memcmp(decoded, source + (size_t)frame * pixel_count,
                     pixel_count * sizeof(uint32_t)) == 0,
              "parallel decode matches source frame");
    }
    CHECK(all_decoded, "decode key and delta frames");

    if (reader.info.frame_count > 1) {
        const uint8_t* entry = reader.index + MVID_INDEX_ENTRY;
        uint32_t size = mm_get32(entry + 8);
        memory_read(&file, mm_get64(entry), reader.frame, size);
        uint32_t* previous = source;
        memcpy(decoded, previous, pixel_count * sizeof(uint32_t));
        uint8_t saved[4];
        memcpy(saved, reader.frame + 4u + (mm_tiles(width, height) + 7u) / 8u, sizeof(saved));
        memset(reader.frame + 4u + (mm_tiles(width, height) + 7u) / 8u, 0, sizeof(saved));
        syscall_media_request_t request = {
            .size = sizeof(request),
            .version = SYS_MEDIA_ABI_VERSION,
            .op = SYS_MEDIA_DECODE_TILES,
            .flags = SYS_MEDIA_HAS_PREVIOUS_FRAME,
            .width = width,
            .height = height,
            .frame_bytes = size,
            .frame = reader.frame,
            .pixels = decoded
        };
        CHECK(media_syscall(&request) == SYS_ERR_INVAL, "reject malformed tile data");
        CHECK(memcmp(decoded, previous, pixel_count * sizeof(uint32_t)) == 0,
              "malformed frame leaves output unchanged");
    }

    mvid_close(&reader, free);
    free(file.data);
    free(decoded);
    free(source);
    if (!failures) puts("parallel media tests passed");
    return failures ? 1 : 0;
}
