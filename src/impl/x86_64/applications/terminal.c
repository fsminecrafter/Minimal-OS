#include "graphics.h"
#include "prochandler.h"
#include "x86_64/gpu.h"
#include "panic.h"
#include "applications/terminal.h"
#include "x86_64/scheduler.h"
#include "serial.h"
#include "string.h"
#include "time.h"

gpu_device_t g_gpu;

void cursorupdater(void) {
    terminal_t* terminal = graphics_get_terminal();

    serial_write_str("Cursor updater process started\n");

    while (1) {
        serial_write_str("Updating cursor...\n");

        terminalUpdateCursor();

        terminal->cursor_visible = !terminal->cursor_visible;

        sleep(500);
    }
}

void terminal_program_entry() {
    g_gpu = *getSystemGPU();
    if (!g_gpu.fb) {
        panic("Failed to initialize GPU", __FILE__, __LINE__, NULL);
    }

    uint32_t width = graphics_get_width();
    uint32_t height = graphics_get_height();

    int16_t cols = width / 8;
    int16_t rows = height / 8;

    terminal_t* terminal = graphics_get_terminal();

    process_t* cursor_process = createProcess("cursorupdater", cursorupdater);
    listAllProcesses();

    graphics_clear(0, 0, 0); // Clear to black
    graphics_set_resolution(cols, rows); // Set terminal mode
    graphics_terminal_set_color(COLOR_WHITE, COLOR_BLACK); // White text
    graphics_write_textr("Welcome to the Minimal OS Terminal!\n"); 

    uint64_t uptime_ms = time_get_uptime_ms();
    string_t uptime_ms_str = str_from_uint(uptime_ms);

    graphics_write_textr("System Date: ");

    const char* data = datetime_str_readable();
    graphics_write_textr(data);
    graphics_write_textr("\n");
    graphics_write_textr("System Uptime: ");
    time_format_uptime(uptime_ms_str.data, uptime_ms_str.capacity);
    graphics_write_textr(uptime_ms_str.data);
    graphics_write_textr("\n");

    terminal->cursor_x = 0;
    terminal->cursor_y = 0;
}