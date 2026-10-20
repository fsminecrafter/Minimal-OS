#include <stdint.h>
#include <stdbool.h>
#include "x86_64/syscall.h"
#include "x86_64/commandhandler.h"
#include "x86_64/minimafs.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/spinlock.h"
#include "graphics.h"
#include "serial.h"
#include "string.h"
#include "time.h"
#include "../../../Minimal-OS-SDK/libraries/minimaSSL/src/crypto.h"

/*
 * SYS_ROOT: run a kernel terminal command from a user program, after the
 * kernel itself has checked a MOSdesk account's password.
 *
 * The password check happens here rather than in the program asking, so a
 * program cannot skip it. An account with password sign-in turned off (auto
 * login) is granted on an empty password: such an account already gives
 * anyone at the keyboard everything, and the terminal asks y/N first. A good password gives the calling process a
 * ticket for SYS_ROOT_TICKET_MS; commands it runs meanwhile need no
 * password. Each command runs in its own kernel process, and whatever it
 * prints on the kernel terminal is captured for the caller to READ.
 *
 * The account file is the one MOSdesk keeps (mosdesk/settings.h) and the
 * hash is the one mosdesk/password_hash.c makes: PBKDF2-HMAC-SHA256,
 * 4096 rounds, one block.
 */

#define ACCOUNTS_PATH "0:/programs/mosdesk/accounts.dat"
#define ACCOUNTS_MAGIC 0x4D444131u
#define MAX_ACCOUNTS 8u
#define USERNAME_SIZE 32u
#define SALT_SIZE 16u
#define HASH_SIZE 32u
#define AVATAR_PATH_SIZE 128u
#define PASSWORD_ROUNDS 4096u
#define PASSWORD_MAX 63u
#define MAX_TICKETS 8u
#define CAPTURE_SIZE 16384u

typedef struct {
    char username[USERNAME_SIZE];
    uint32_t password_enabled;
    uint8_t password_salt[SALT_SIZE];
    uint8_t password_hash[HASH_SIZE];
    char avatar[AVATAR_PATH_SIZE];
} account_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
    account_t list[MAX_ACCOUNTS];
} accounts_t;

typedef struct {
    uint64_t pid;
    uint64_t expires_ms;
} ticket_t;

static spinlock_t g_root_lock = SPINLOCK_INIT;
static ticket_t g_tickets[MAX_TICKETS];

/* The one command that may run at a time, and its captured output. */
static volatile uint64_t g_job_pid;         // the command's kernel process
static volatile uint64_t g_job_owner;       // the process that asked
static volatile bool g_job_running;
static char g_job_command[256];
static char g_capture[CAPTURE_SIZE];
static volatile uint32_t g_capture_head, g_capture_tail;

static uint64_t caller_pid(void) {
    return current_process ? current_process->pid : 0;
}

/* ---------------------------------------------------------------- hashing */

static void hmac_sha256(const uint8_t key[64], const uint8_t* data,
                        size_t length, uint8_t digest[32]) {
    uint8_t inner[64 + 32];
    uint8_t outer[64 + 32];
    for (size_t i = 0; i < 64u; i++) {
        inner[i] = (uint8_t)(key[i] ^ 0x36u);
        outer[i] = (uint8_t)(key[i] ^ 0x5Cu);
    }
    memcpy(inner + 64u, data, length);
    dlr_sha256(inner, 64u + length, digest);
    memcpy(outer + 64u, digest, 32u);
    dlr_sha256(outer, sizeof(outer), digest);
}

static bool password_matches(const account_t* account, const char* password) {
    size_t length = strlen(password);
    if (!account->password_enabled || !length || length > PASSWORD_MAX)
        return false;
    uint8_t key[64] = {0};
    memcpy(key, password, length);
    uint8_t block[SALT_SIZE + 4u];
    memcpy(block, account->password_salt, SALT_SIZE);
    block[SALT_SIZE] = 0; block[SALT_SIZE + 1u] = 0;
    block[SALT_SIZE + 2u] = 0; block[SALT_SIZE + 3u] = 1;
    uint8_t value[32], digest[32];
    hmac_sha256(key, block, sizeof(block), value);
    memcpy(digest, value, sizeof(value));
    for (uint32_t round = 1; round < PASSWORD_ROUNDS; round++) {
        hmac_sha256(key, value, sizeof(value), value);
        for (size_t i = 0; i < sizeof(value); i++) digest[i] ^= value[i];
    }
    uint8_t difference = 0;
    for (size_t i = 0; i < HASH_SIZE; i++)
        difference |= digest[i] ^ account->password_hash[i];
    memset(key, 0, sizeof(key));
    memset(value, 0, sizeof(value));
    memset(digest, 0, sizeof(digest));
    return difference == 0;
}

static bool load_accounts(accounts_t* accounts) {
    minimafs_file_handle_t* file = minimafs_open(ACCOUNTS_PATH, true);
    if (!file) return false;
    uint32_t got = minimafs_read(file, accounts, sizeof(*accounts));
    minimafs_close(file);
    return got == sizeof(*accounts) && accounts->magic == ACCOUNTS_MAGIC &&
           accounts->count <= MAX_ACCOUNTS;
}

/*
 * Current MOSdesk keeps its single account in settings.dat
 * (mosdesk/settings.h, mosdesk_settings_t) instead of accounts.dat:
 * login_enabled, password salt and hash near the start, the username at
 * the end (files from before usernames existed have none).
 */
#define SETTINGS_PATH "0:/programs/mosdesk/settings.dat"
#define SETTINGS_MAGIC 0x4D445331u
#define SETTINGS_LOGIN_OFFSET 32u
#define SETTINGS_SALT_OFFSET 36u
#define SETTINGS_HASH_OFFSET 52u
#define SETTINGS_USERNAME_OFFSET 828u
#define SETTINGS_SIZE (SETTINGS_USERNAME_OFFSET + USERNAME_SIZE)

static bool load_settings_account(account_t* account) {
    static uint8_t raw[SETTINGS_SIZE];
    memset(raw, 0, sizeof(raw));
    minimafs_file_handle_t* file = minimafs_open(SETTINGS_PATH, true);
    if (!file) return false;
    uint32_t got = minimafs_read(file, raw, sizeof(raw));
    minimafs_close(file);
    uint32_t magic;
    memcpy(&magic, raw, 4);
    if (got < SETTINGS_HASH_OFFSET + HASH_SIZE || magic != SETTINGS_MAGIC) return false;
    memset(account, 0, sizeof(*account));
    uint32_t login;
    memcpy(&login, raw + SETTINGS_LOGIN_OFFSET, 4);
    account->password_enabled = login ? 1u : 0u;
    memcpy(account->password_salt, raw + SETTINGS_SALT_OFFSET, SALT_SIZE);
    memcpy(account->password_hash, raw + SETTINGS_HASH_OFFSET, HASH_SIZE);
    if (got >= SETTINGS_SIZE) {
        memcpy(account->username, raw + SETTINGS_USERNAME_OFFSET, USERNAME_SIZE);
        account->username[USERNAME_SIZE - 1u] = '\0';
    }
    memset(raw, 0, sizeof(raw));
    return true;
}

/* ---------------------------------------------------------------- tickets */

static ticket_t* find_ticket(uint64_t pid) {
    uint64_t now = time_get_uptime_ms();
    for (uint32_t i = 0; i < MAX_TICKETS; i++)
        if (g_tickets[i].pid == pid && g_tickets[i].expires_ms > now)
            return &g_tickets[i];
    return NULL;
}

static uint64_t ticket_left(uint64_t pid) {
    uint64_t flags = spinlock_acquire(&g_root_lock);
    ticket_t* ticket = find_ticket(pid);
    uint64_t left = ticket ? ticket->expires_ms - time_get_uptime_ms() : 0;
    spinlock_release(&g_root_lock, flags);
    return left;
}

static void grant_ticket(uint64_t pid) {
    uint64_t flags = spinlock_acquire(&g_root_lock);
    uint64_t now = time_get_uptime_ms();
    ticket_t* slot = find_ticket(pid);
    for (uint32_t i = 0; !slot && i < MAX_TICKETS; i++)
        if (g_tickets[i].expires_ms <= now) slot = &g_tickets[i];
    if (!slot) slot = &g_tickets[0];
    slot->pid = pid;
    slot->expires_ms = now + SYS_ROOT_TICKET_MS;
    spinlock_release(&g_root_lock, flags);
}

static void drop_ticket(uint64_t pid) {
    uint64_t flags = spinlock_acquire(&g_root_lock);
    for (uint32_t i = 0; i < MAX_TICKETS; i++)
        if (g_tickets[i].pid == pid) g_tickets[i].expires_ms = 0;
    spinlock_release(&g_root_lock, flags);
}

/* ---------------------------------------------------------------- capture */

static bool capture_char(char c) {
    if (!g_job_running || caller_pid() != g_job_pid) return false;
    uint64_t flags = spinlock_acquire(&g_root_lock);
    uint32_t next = (g_capture_head + 1u) % CAPTURE_SIZE;
    if (next != g_capture_tail) {       // full: the oldest text is kept
        g_capture[g_capture_head] = c;
        g_capture_head = next;
    }
    spinlock_release(&g_root_lock, flags);
    return true;
}

static void job_entry(void) {
    serial_write_str("root: running '");
    serial_write_str(g_job_command);
    serial_write_str("'\n");
    command_execute(g_job_command);
    g_job_running = false;
    process_exit();
}

/* --------------------------------------------------------------- syscall */

uint64_t root_syscall(syscall_root_t* request) {
    if (!request) return SYS_ERR_INVAL;
    uint64_t pid = caller_pid();
    switch (request->op) {
        case SYS_ROOT_AUTH: {
            request->username[sizeof(request->username) - 1u] = '\0';
            request->password[sizeof(request->password) - 1u] = '\0';
            static accounts_t accounts;
            bool ok = false;
            static account_t settings_account;
            if (!ok && load_settings_account(&settings_account) &&
                (!settings_account.username[0] ||
                 !strcmp(settings_account.username, request->username))) {
                ok = settings_account.password_enabled
                    ? password_matches(&settings_account, request->password)
                    : request->password[0] == '\0';
            }
            memset(&settings_account, 0, sizeof(settings_account));
            if (!ok && load_accounts(&accounts)) {
                for (uint32_t i = 0; i < accounts.count && !ok; i++) {
                    accounts.list[i].username[USERNAME_SIZE - 1u] = '\0';
                    if (strcmp(accounts.list[i].username, request->username))
                        continue;
                    /* An auto-login account has no password to check; its
                     * terminal asks the user to confirm instead. */
                    if (!accounts.list[i].password_enabled)
                        ok = request->password[0] == '\0';
                    else
                        ok = password_matches(&accounts.list[i],
                                              request->password);
                }
            }
            memset(request->password, 0, sizeof(request->password));
            memset(&accounts, 0, sizeof(accounts));
            if (!ok) {
                sleep(1000);            // slows down guessing
                serial_write_str("root: authentication failed\n");
                return SYS_ERR_PERM;
            }
            grant_ticket(pid);
            request->ticket_ms_left = ticket_left(pid);
            return SYS_SUCCESS;
        }
        case SYS_ROOT_RUN: {
            if (!ticket_left(pid)) return SYS_ERR_PERM;
            request->command[sizeof(request->command) - 1u] = '\0';
            if (!request->command[0]) return SYS_ERR_INVAL;
            if (g_job_running) return SYS_ERR_BUSY;
            strcpy(g_job_command, request->command);
            g_capture_head = g_capture_tail = 0;
            g_job_owner = pid;
            graphics_text_capture = capture_char;
            g_job_running = true;
            /* The job is not captured until its pid is known; nothing has
             * run yet at that point. */
            g_job_pid = UINT64_MAX;
            process_t* process = proc_create("root", job_entry);
            if (!process) {
                g_job_running = false;
                return SYS_ERR_GENERIC;
            }
            g_job_pid = process->pid;
            request->ticket_ms_left = ticket_left(pid);
            return SYS_SUCCESS;
        }
        case SYS_ROOT_READ: {
            if (g_job_owner != pid) return SYS_ERR_PERM;
            uint64_t flags = spinlock_acquire(&g_root_lock);
            uint32_t length = 0;
            while (g_capture_tail != g_capture_head &&
                   length < SYS_ROOT_OUTPUT_SIZE) {
                request->output[length++] = g_capture[g_capture_tail];
                g_capture_tail = (g_capture_tail + 1u) % CAPTURE_SIZE;
            }
            bool running = g_job_running;
            bool more = g_capture_tail != g_capture_head;
            spinlock_release(&g_root_lock, flags);
            request->output_length = length;
            request->running = running || more ? 1u : 0u;
            return SYS_SUCCESS;
        }
        case SYS_ROOT_STATUS:
            request->ticket_ms_left = ticket_left(pid);
            request->running = g_job_running && g_job_owner == pid;
            return SYS_SUCCESS;
        case SYS_ROOT_DROP:
            drop_ticket(pid);
            return SYS_SUCCESS;
        default:
            return SYS_ERR_INVAL;
    }
}
