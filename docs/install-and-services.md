# Install Packages and Services

Minimal-OS builds three resource packages into the kernel image:

| Source directory | Destination after `format` |
| --- | --- |
| `src/resources/install1` | `0:/etc` |
| `src/resources/install2` | `0:/programs` |
| `src/resources/install3` | `0:/services` |

Each directory is packaged as an `.mpkg` archive during `make build-x86_64`.
The archives are embedded in the kernel because formatting happens before the
new MinimaFS disk contains any files. The `format` command extracts the
archives into their destination directories.

## Adding A Service

Place a `.service` file in `src/resources/install3`. No C registration call is
needed. The file is packaged automatically, extracted to `0:/services`, and
discovered after the boot disk is mounted.

Example: `src/resources/install3/example.service`

```ini
[GENERAL]
name="Example Service"
desc="Runs the example program"
exit=Restart

[STARTUP]
exec="0:/programs/example.run"
```

`[STARTUP]` must contain exactly one startup action:

- `exec="0:/programs/example.run"` starts a `.run` bundle directly.
- `exec="run 0:/programs/example.run"` starts a `.run` bundle through the
	command system and monitors it like a direct `exec` path.
- `exec="echo service started"` runs any terminal command once.
- `script="run 0:/programs/example.run"` starts it through the command system.

Command-style `exec` and `script` entries run once without PID tracking, so
`exit=Restart` and `exit=Script` cannot monitor or react to their completion.

The `exit` setting controls what happens after the startup process exits:

- `None` stops monitoring the service.
- `Restart` starts it again after a short delay.
- `Script` runs the command in `exitscript`.

Example with an exit script:

```ini
[GENERAL]
name="Worker"
desc="Background worker"
exit=Script
exitscript="run 0:/programs/worker-failed.run"

[STARTUP]
exec="0:/programs/worker.run"
```

After boot, use:

```text
services
```

to list successfully parsed and registered services. Invalid files, files
without a `.service` extension, and services without `exec` or `script` are
skipped.

## Build And Clean

Build the kernel and regenerate all install packages:

```sh
make build-x86_64
```

`minimaSSL.slib` is generated into `src/resources/install1` and installed as
`0:/etc/minimaSSL.slib` during `format`.

Remove kernel, SDK, generated package, and `.run` build artifacts with:

```sh
make clean
```

The current kernel links the freestanding minimaSSL crypto implementation
statically. The installed `.slib` is the filesystem distribution artifact; the
OS does not currently dynamically load `.slib` code.
