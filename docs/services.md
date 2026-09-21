# Services

## Discovery And Registration

Services are filesystem definitions, not C registration calls. After the boot
disk is mounted, `terminal_program_entry()` calls `service_manager_init()`.
The manager scans `0:/services`, accepts files whose names end in `.service`,
parses them, and creates a monitor process for each valid entry.

A service is registered only when it has at least one `[STARTUP]` action:
`exec` or `script`. The terminal command `services` lists entries that parsed
and registered successfully.

## File Format

```ini
[GENERAL]
name="Example Service"
desc="Runs an example"
exit=Restart
exitscript="run 0:/programs/recovery.run"

[STARTUP]
exec="0:/programs/example.run"
```

Supported general keys:

- `name`: display name; defaults to the filename;
- `desc`: text shown by `services`;
- `exit`: `None`, `Restart`, or `Script`;
- `exitscript`: command used by `Script` after a monitored process exits.

Supported startup keys:

- `exec`: direct `.run` path, `run <path>`, or an arbitrary terminal command;
- `script`: `run <path>` for a launchable `.run`, or another command string.

Values may be quoted. `//` starts a comment outside quotes.

## Startup Forms

A directly monitored executable uses a path:

```ini
[STARTUP]
exec="0:/programs/worker.run"
```

The equivalent command form is also recognized:

```ini
[STARTUP]
exec="run 0:/programs/worker.run"
```

An arbitrary command can be run once:

```ini
[STARTUP]
exec="echo service started"
```

Command-style entries call the terminal command dispatcher and do not expose a
process ID to the service monitor. `exit=Restart` and `exit=Script` therefore
cannot supervise their completion. Use a `.run` path when restart or exit
handling is required.

## Supervision

For a direct `.run` launch, the manager:

1. normalizes the path to a `0:/...` path;
2. launches the bundle as a process;
3. waits for its process ID;
4. applies the exit policy.

`None` stops after exit. `Restart` waits briefly and starts again. `Script`
executes `exitscript` once and then stops.

A failed launch is logged on serial and does not create a usable monitored
service. A malformed file, missing startup action, or unsupported startup
configuration is skipped.

## Packaging A Service

Place the definition in `src/resources/install3`:

```text
src/resources/install3/example.service
```

`make build-x86_64` packages install3 and embeds it in the kernel. Running
`format` extracts it to `0:/services` before service discovery. A referenced
`.run` program should be placed in `src/resources/install2`, which becomes
`0:/programs`.

The complete install mapping is:

```text
install1 -> 0:/Etc
install2 -> 0:/programs
install3 -> 0:/services
```

## Troubleshooting

Use `services` after mounting to inspect registrations. Use the serial console
to distinguish these cases:

- no services directory: install3 was empty or format did not run;
- parse failure: the file syntax or section/key names are invalid;
- no startup action: add exactly one `exec` or `script` key;
- failed to start: the referenced `.run` path is missing or invalid;
- repeated starts: the service has `exit=Restart` and exits normally.
