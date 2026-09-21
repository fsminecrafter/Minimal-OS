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
#include "x86_64/servicemanager.h"

#include "vgaterm.h"
#include "minimafshandler.h"
#include "systeminfo.h"
#include "mouse/usbmouse.h"

void usb_keyboard_update_task(void);
extern const char* fs_get_current_directory(void);

// ===========================================
// TERMINAL STATE
// ===========================================

gpu_device_t g_gpu;
terminal_t* terminal;

#define INPUT_BUFFER_SIZE 256
#define COMMAND_HISTORY_SIZE 25
static char input_buffer[INPUT_BUFFER_SIZE];
static uint16_t input_pos = 0;
static bool command_ready = false;
static char command_history[COMMAND_HISTORY_SIZE][INPUT_BUFFER_SIZE];
static uint16_t command_history_count = 0;
static int16_t command_history_index = -1;
static char command_history_current[INPUT_BUFFER_SIZE];

// True while a command process launched via command_execute_async() is
// running. While this is true:
//   - the prompt/input line is not shown (terminal_update() brings it
//     back once the command process finishes)
//   - ordinary keystrokes are ignored by terminal_keyboard_callback()
//   - Ctrl+C kills the running command process instead of editing input
static bool g_command_running = false;
static uint64_t g_last_ctrl_c_ms = 0;
void terminal_keyboard_callback(uint8_t scancode, char character, bool pressed);

/*
 * QEMU's serial device is the only dependable input channel in a
 * display-less test.  Feed its bytes through the same terminal callback as
 * the USB keyboard so headless automation exercises the normal command
 * parser instead of requiring monitor "sendkey" support.
 */
static void terminal_poll_serial_input(void) {
    static bool previous_was_cr = false;

    /* A hostile or accidentally connected serial peer must not starve the
     * terminal process forever. */
    for (uint16_t i = 0; i < INPUT_BUFFER_SIZE && serial_received(); i++) {
        char c = serial_read();

        if (c == '\r') {
            terminal_keyboard_callback(USB_KEY_ENTER, '\n', true);
            previous_was_cr = true;
        } else if (c == '\n') {
            if (!previous_was_cr)
                terminal_keyboard_callback(USB_KEY_ENTER, '\n', true);
            previous_was_cr = false;
        } else if (c == '\b' || (uint8_t)c == 0x7f) {
            terminal_keyboard_callback(USB_KEY_BACKSPACE, '\b', true);
            previous_was_cr = false;
        } else if ((uint8_t)c >= 32 && (uint8_t)c <= 126) {
            terminal_keyboard_callback(0, c, true);
            previous_was_cr = false;
        }
    }
}

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
    graphics_write_textr("Computer@local-");
    graphics_write_textr(fs_get_current_directory());
    graphics_write_textr("$ ");
}

static void terminal_erase_last_char(void) {
    if (input_pos == 0) return;

    input_pos--;
    input_buffer[input_pos] = '\0';

    if (terminal->cursor_x > 0) {
        terminal->cursor_x--;
    } else if (terminal->cursor_y > 0) {
        terminal->cursor_y--;
        terminal->cursor_x = terminal->cols - 1;
    }

    uint16_t saved_x = terminal->cursor_x;
    uint16_t saved_y = terminal->cursor_y;
    graphics_write_textr_char(' ');
    terminal->cursor_x = saved_x;
    terminal->cursor_y = saved_y;
}

static void terminal_erase_input(void) {
    while (input_pos > 0) terminal_erase_last_char();
}

static void terminal_replace_input(const char* text) {
    terminal_erase_input();
    strncpy(input_buffer, text, INPUT_BUFFER_SIZE - 1);
    input_buffer[INPUT_BUFFER_SIZE - 1] = '\0';
    input_pos = (uint16_t)strlen(input_buffer);
    graphics_write_textr(input_buffer);
}

static void terminal_add_history(const char* command) {
    if (!command || command[0] == '\0') return;

    if (command_history_count < COMMAND_HISTORY_SIZE) {
        strncpy(command_history[command_history_count], command, INPUT_BUFFER_SIZE - 1);
        command_history[command_history_count][INPUT_BUFFER_SIZE - 1] = '\0';
        command_history_count++;
        return;
    }

    for (uint16_t i = 1; i < COMMAND_HISTORY_SIZE; i++) {
        strncpy(command_history[i - 1], command_history[i], INPUT_BUFFER_SIZE);
    }
    strncpy(command_history[COMMAND_HISTORY_SIZE - 1], command, INPUT_BUFFER_SIZE - 1);
    command_history[COMMAND_HISTORY_SIZE - 1][INPUT_BUFFER_SIZE - 1] = '\0';
}

static void terminal_history_up(void) {
    if (command_history_count == 0) return;

    if (command_history_index < 0) {
        strncpy(command_history_current, input_buffer, INPUT_BUFFER_SIZE - 1);
        command_history_current[INPUT_BUFFER_SIZE - 1] = '\0';
        command_history_index = (int16_t)command_history_count - 1;
    } else if (command_history_index > 0) {
        command_history_index--;
    }

    terminal_replace_input(command_history[command_history_index]);
}

static void terminal_history_down(void) {
    if (command_history_index < 0) return;

    if (command_history_index < (int16_t)command_history_count - 1) {
        command_history_index++;
        terminal_replace_input(command_history[command_history_index]);
    } else {
        command_history_index = -1;
        terminal_replace_input(command_history_current);
    }
}

// ===========================================
// KEYBOARD CALLBACK
// ===========================================

void terminal_keyboard_callback(uint8_t scancode, char character, bool pressed) {
    if (!pressed) return;  // Only handle key presses

    /*
     * Ctrl+C (ETX, 0x03). The active keyboard layout's ctrl layer already
     * translates Ctrl+C into this control character for us (see
     * usKeyboard.h / swedishKeyboard.h's .ctrl[USB_KEY_C] entry), so
     * there's no need to separately track the ctrl modifier here.
     *
     * While a command process is running, this kills it instead of being
     * treated as input. When nothing is running it just clears whatever
     * is currently typed, same spirit as the existing ESC handling below.
     */
    const usb_keyboard_state_t* keyboard_state = usb_keyboard_get_state();
    bool physical_ctrl_c = scancode == USB_KEY_C && keyboard_state &&
                           keyboard_state->ctrl;
    uint64_t now_ms = time_get_uptime_ms();

    if (physical_ctrl_c && g_last_ctrl_c_ms != 0 &&
        now_ms - g_last_ctrl_c_ms <= 800) {
        if (command_has_last_user_program()) {
            command_terminate_last_user_program();
        } else {
            command_kill_running();
        }
        g_last_ctrl_c_ms = 0;
        if (!g_command_running) {
            graphics_write_textr("^C\n");
            terminalPrompt();
        }
        return;
    }

    if (physical_ctrl_c)
        g_last_ctrl_c_ms = now_ms;

    if (character == 0x03) {
        if (g_command_running || command_has_last_user_program()) {
            serial_write_str("Terminal: Ctrl+C - killing running command\n");
            command_kill_running();
            graphics_write_textr("^C\n");
        } else {
            input_pos = 0;
            input_buffer[0] = '\0';
            command_history_index = -1;
            graphics_write_textr("^C\n");
            terminalPrompt();
        }
        return;
    }

    if (usb_keyboard_key_wait_active()) return;

    /*
     * While a command is running, the input line/prompt is gone (see
     * terminal_update()) - there's nowhere for other keystrokes to go, so
     * ignore them until the command finishes or is killed above.
     */
    if (g_command_running) {
        return;
    }
    
    // Handle special keys
    switch (scancode) {
        case USB_KEY_ENTER:
            // Submit command
            graphics_write_textr("\n");
            input_buffer[input_pos] = '\0';
            terminal_add_history(input_buffer);
            command_history_index = -1;
            command_ready = true;
            serial_write_str("Terminal: Command ready: '");
            serial_write_str(input_buffer);
            serial_write_str("'\n");
            break;
            
        case USB_KEY_BACKSPACE:
            terminal_erase_last_char();
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
            command_history_index = -1;
            graphics_write_textr("^C\n");
            terminalPrompt();
            break;
            
        case USB_KEY_UP:
            terminal_history_up();
            break;
        case USB_KEY_DOWN:
            terminal_history_down();
            break;
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

    /*
     * Set this before actually launching. usb_poll() (and therefore
     * terminal_keyboard_callback()) runs from inside the PIT interrupt
     * handler, so a key event can land at any point between here and the
     * process actually being created. Marking "running" first ensures
     * such an event sees input already blocked rather than racing to
     * submit another command against a launch that hasn't finished.
     */
    g_command_running = true;

    uint64_t pid = command_execute_async(cmd);
    if (pid == 0) {
        /*
         * Nothing was launched (unknown command, empty input, or a
         * command was somehow already running) - command_execute_async()
         * already printed any relevant message. Nothing to wait for.
         */
        g_command_running = false;
        terminalPrompt();
        return;
    }

    /*
     * On success, the prompt is intentionally NOT printed here -
     * terminal_update() prints it once the spawned command process has
     * actually finished (see below).
     */
}

// ===========================================
// TERMINAL UPDATE LOOP
// ===========================================

void terminal_update(void) {
    while (1) {
        terminal_poll_serial_input();
        
        // Process pending command
        if (command_ready) {
            if (!g_command_running) {
                terminal_process_command(input_buffer);
            }
            /*
             * If a command is already running, drop this submission
             * rather than queueing it. terminal_keyboard_callback()
             * already blocks input while g_command_running is true, so
             * this branch shouldn't normally be reachable - kept only as
             * a safety net.
             */
            input_pos = 0;
            input_buffer[0] = '\0';
            command_ready = false;
        }

        // Notice when the in-flight command process has finished and
        // bring the prompt back.
        if (g_command_running) {
            command_poll_running();
            if (!command_is_running()) {
                g_command_running = false;
                graphics_write_textr("\n");
                terminalPrompt();
            }
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

    // Initialize USB mouse driver
    usb_mouse_init();
    
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
    graphics_terminal_clear();
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
                serial_write_str("Terminal: mountdrive returned; loading saved resolution...\n");
                if (systeminfo_load_saved_resolution()) {
                    vgaterm_print("Loaded saved display resolution.\n");
                }
                serial_write_str("Terminal: saved resolution step complete; initializing services...\n");
                service_manager_init();
                serial_write_str("Terminal: service initialization complete\n");
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
        minimafs_drive_t* d = get_drive(0);

        if (!d) {
            serial_write_str("Drive 0 = NULL\n");
        } else if (!d->mounted) {
            serial_write_str("Drive 0 not mounted\n");
        } else {
            vgaterm_print("Drive 0 OK\n");
        }
        }
    }
    
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

    terminalPrompt();
    createProcess("terminal_update", terminal_update);
    createProcess("usb_keyboard_update", usb_keyboard_update_task);
    schedulerInit();

}
