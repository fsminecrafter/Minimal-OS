#ifndef SERVICEMANAGER_H
#define SERVICEMANAGER_H

/*
 * Services Manager
 *
 * Scans 0:/services/ for *.service files and starts each one as a
 * background service, supervised by its own monitor process.
 *
 * .service file format (INI-like, // starts a comment):
 *
 *   [GENERAL]
 *   name="My Service"
 *   desc="Its just a random service"
 *   exit=None            // None | Restart | Script
 *   exitscript="run 0:/itdied.run"
 *
 *   [STARTUP]
 *   script="run 0:/itsalive.run"
 *   // -- or --
 *   exec="0:/hello.run"
 *
 * [STARTUP] needs exactly one of `script` or `exec`. `exec` loads the
 * given .run bundle directly. `script="run <path>"` is treated the
 * same way; any other script command is executed once at startup with
 * no process being tracked, so `exit=Restart`/`exit=Script` have
 * nothing to act on for it.
 */

// Scans 0:/services and starts every valid service found there. Safe
// to call even if the directory doesn't exist (does nothing in that
// case). Should be called once, after the boot disk has actually been
// mounted - see terminal_program_entry() in terminal.c.
void service_manager_init(void);

#endif // SERVICEMANAGER_H
