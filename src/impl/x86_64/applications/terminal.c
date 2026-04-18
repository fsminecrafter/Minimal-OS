#include "graphics.h"
#include "prochandler.h"
#include "x86_64/gpu.h"
#include "panic.h"
#include "applications/terminal.h"
#include "x86_64/scheduler.h"
#include "keyboard/usbkeyboard.h"
#include "usb/usb_stack.h"
#include "x86_64/commandhandler.h"
#include "serial.h"
#include "string.h"
#include "time.h"

#include "vgaterm.h"
#include "minimafshandler.h"

// ===========================================
// TERMINAL STATE
// ===========================================

gpu_device_t g_gpu;
terminal_t* terminal;

#define INPUT_BUFFER_SIZE 256
static char input_buffer[INPUT_BUFFER_SIZE];
static uint16_t input_pos = 0;
static bool command_ready = false;

// ===========================================
// TERMINAL RENDERING
// ===========================================

void cursorupdater(void) {
    serial_write_str("Cursor updater process started\n");

    while (1) {
        terminalUpdateCursor();
        terminal->cursor_visible = !terminal->cursor_visible;
        sleep(500);
    }
}

void terminalPrompt(void) {
    graphics_write_textr("Computer@local:~$ ");
}

// ===========================================
// KEYBOARD CALLBACK
// ===========================================

void terminal_keyboard_callback(uint8_t scancode, char character, bool pressed) {
    if (!pressed) return;  // Only handle key presses
    
    // Debug log
    serial_write_str("Terminal: Key scancode=0x");
    serial_write_hex(scancode);
    serial_write_str(" char='");
    if (character >= 32 && character <= 126) {
        char buf[2];
        buf[0] = character;
        buf[1] = 0;
        serial_write_str(buf);
    } else {
        serial_write_str("0x");
        serial_write_hex((uint8_t)character);
    }
    serial_write_str("'\n");
    
    // Handle special keys
    switch (scancode) {
        case USB_KEY_ENTER:
            // Submit command
            graphics_write_textr("\n");
            input_buffer[input_pos] = '\0';
            command_ready = true;
            serial_write_str("Terminal: Command ready: '");
            serial_write_str(input_buffer);
            serial_write_str("'\n");
            break;
            
        case USB_KEY_BACKSPACE:
            if (input_pos > 0) {
                input_pos--;
                input_buffer[input_pos] = '\0';
                
                // Move cursor back
                if (terminal->cursor_x > 0) {
                    terminal->cursor_x--;
                } else if (terminal->cursor_y > 0) {
                    terminal->cursor_y--;
                    terminal->cursor_x = terminal->cols - 1;
                }
                
                // Save current position
                uint16_t saved_x = terminal->cursor_x;
                uint16_t saved_y = terminal->cursor_y;
                
                // Write space and move forward
                graphics_write_textr_char(' ');
                
                // Restore position
                terminal->cursor_x = saved_x;
                terminal->cursor_y = saved_y;
            }
            break;
            
        case USB_KEY_TAB:
            // Tab - insert 4 spaces
            for (int i = 0; i < 4 && input_pos < INPUT_BUFFER_SIZE - 1; i++) {
                input_buffer[input_pos++] = ' ';
                graphics_write_textr_char(' ');
            }
            break;
            
        case USB_KEY_ESCAPE:
            // Clear input
            input_pos = 0;
            input_buffer[0] = '\0';
            graphics_write_textr("^C\n");
            terminalPrompt();
            break;
            
        case USB_KEY_UP:
        case USB_KEY_DOWN:
        case USB_KEY_LEFT:
        case USB_KEY_RIGHT:
        case USB_KEY_HOME:
        case USB_KEY_END:
        case USB_KEY_PAGEUP:
        case USB_KEY_PAGEDOWN:
        case USB_KEY_INSERT:
        case USB_KEY_DELETE:
        case USB_KEY_F1:
        case USB_KEY_F2:
        case USB_KEY_F3:
        case USB_KEY_F4:
        case USB_KEY_F5:
        case USB_KEY_F6:
        case USB_KEY_F7:
        case USB_KEY_F8:
        case USB_KEY_F9:
        case USB_KEY_F10:
        case USB_KEY_F11:
        case USB_KEY_F12:
        case USB_KEY_CAPSLOCK:
        case USB_KEY_NUMLOCK:
        case USB_KEY_SCROLLLOCK:
        case USB_KEY_PRINTSCREEN:
        case USB_KEY_PAUSE:
            // Ignore these special keys
            break;
            
        default:
            // Only print VALID printable ASCII characters
            if (character >= 32 && character <= 126) {
                if (input_pos < INPUT_BUFFER_SIZE - 1) {
                    input_buffer[input_pos++] = character;
                    graphics_write_textr_char(character);
                }
            } else if (character != 0) {
                // Non-ASCII character - log but don't display
                serial_write_str("Terminal: WARNING - Non-ASCII character 0x");
                serial_write_hex((uint8_t)character);
                serial_write_str("\n");
            }
            break;
    }
}

// ===========================================
// COMMAND PROCESSING
// ===========================================

void terminal_process_command(const char* cmd) {
    // Trim leading/trailing spaces
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') {
        terminalPrompt();
        return;
    }
    
    serial_write_str("Terminal: Processing command: '");
    serial_write_str(cmd);
    serial_write_str("'\n");
    
    // Use existing command system
    command_execute(cmd);
    
    terminalPrompt();
}

// ===========================================
// TERMINAL UPDATE LOOP
// ===========================================

void terminal_update(void) {
    while (1) {
        
        // Process pending command
        if (command_ready) {
            terminal_process_command(input_buffer);
            input_pos = 0;
            input_buffer[0] = '\0';
            command_ready = false;
        }
        sleep(10);
    }
}

// ===========================================
// KEYBOARD INITIALIZATION
// ===========================================

void terminal_init_keyboard(void) {
    serial_write_str("Terminal: Initializing keyboard...\n");
    
    commandhandler_init();

    // Initialize USB keyboard driver
    usb_keyboard_init();
    
    // Load Swedish keyboard layout
    extern const keyboard_layout_t usb_layout_se_qwerty;
    usb_keyboard_load_layout(&usb_layout_se_qwerty);
    
    // Register our callback
    usb_keyboard_set_callback(terminal_keyboard_callback);
    
    // Check what keyboard we have
    usb_device_t* usb_kbd = usb_get_keyboard();
    
    if (usb_kbd && usb_is_configured(usb_kbd)) {
        serial_write_str("Terminal: USB keyboard detected!\n");
        graphics_write_textr("Keyboard: USB (");
        graphics_write_textr(usb_get_speed_string(usb_kbd));
        graphics_write_textr(")\n");
    } else {
        serial_write_str("Terminal: PS/2 keyboard (or no keyboard)\n");
        graphics_write_textr("Keyboard: PS/2 fallback\n");
        
        // Initialize PS/2 bridge if available
        #ifdef HAVE_PS2_KEYBOARD
        extern void ps2_usb_bridge_init(void);
        ps2_usb_bridge_init();
        #endif
    }
    
    serial_write_str("Terminal: Keyboard ready!\n");
}



// ===========================================
// TERMINAL INITIALIZATION
// ===========================================

void terminal_program_entry(void) {
    serial_write_str("=== TERMINAL STARTING ===\n");
    
    g_gpu = *getSystemGPU();
    if (!g_gpu.fb) {
        panic("Failed to initialize GPU", __FILE__, __LINE__, NULL);
    }

    uint32_t width = graphics_get_width();
    uint32_t height = graphics_get_height();

    int16_t cols = width / 8;
    int16_t rows = height / 8;

    terminal = graphics_get_terminal();

    // Clear screen and setup
    graphics_clear(0, 0, 0);
    graphics_set_resolution(cols, rows);
    graphics_terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    
    // Initialize command system
    serial_write_str("Terminal: Initializing command system...\n");
    commandhandler_init();
    vgaterm_init();
    
    // Initialize keyboard
    terminal_init_keyboard();
    
    // Start cursor updater process
    serial_write_str("Terminal: Starting cursor process...\n");
    process_t* cursor_process = createProcess("cursorupdater", cursorupdater);
    
    // Display welcome message
    graphics_write_textr("========================================\n");
    graphics_write_textr("  Welcome to MinimalOS Terminal!\n");
    graphics_write_textr("========================================\n\n");
    
    graphics_write_textr("System: ");
    graphics_write_textr(datetime_str_readable());
    graphics_write_textr("\n");
    
    graphics_write_textr("Uptime: ");
    graphics_write_textr(uptime_str_human());
    graphics_write_textr("\n\n");
    
    // Keyboard status was already printed by terminal_init_keyboard()
    graphics_write_textr("Type a command and press Enter.\n");
    graphics_write_textr("Type 'help' for available commands.\n\n");
    
    serial_write_str("=== TERMINAL READY ===\n");

    if (vgaterm_ask_yn("Mount disk?", true)) {
        vgaterm_print("Mounting disk...\n");
        command_execute("initdisk");
        minimafs_disk_device_t* device;
        device = getminimadrive();
        if (!device) {
            vgaterm_print("/cr255g0b0/No disk device found/cr255g255b255/\n");
        } else {
            int success = mountdrive(device, 0);
            if (success == 1) {
                vgaterm_print("Mount succeded.\n");
            }else if (success == 2) {
                vgaterm_print("/cr255g0b0/MinimaFS: Drive already mounted/cr255g255b255/\n");
            }else if (success == 3) {
                vgaterm_print("/cr255g0b0/MinimaFS: Failed to parse storage.desc/cr255g255b255/\n");
            }else if (success == 4) {
                vgaterm_print("/cr255g0b0/MinimaFS: Invalid root block/cr255g255b255/\n");
            }else if (success == 5) {
                vgaterm_print("/cr255g0b0/MinimaFS: Drive too large for bitmap/cr255g255b255/\n");
            } else {
                vgaterm_print("/cr255g0b0/Mount failed.\n");
            }
        minimafs_drive_t* d = get_drive(1);

        if (!d) {
            serial_write_str("Drive 1 = NULL\n");
        } else if (!d->mounted) {
            serial_write_str("Drive 1 not mounted\n");
        } else {
            vgaterm_print("Drive 1 OK\n");
        }
        }
    }

    terminalPrompt();
    createProcess("terminal_update", terminal_update);
    createProcess("usb_keyboard_update", usb_keyboard_update);
    schedulerInit();

}
