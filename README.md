# Minimal-OS
Is a OS made in C and Assembly made for x86_64 processors, It currently runs with 1024MB (I havent tested lower yet)

find roadmap in roadmap.txt or below (might be old or un-synced!)

User application developers can find the syscall and SDK integration guide in
[docs/user-sdk-syscalls.md](docs/user-sdk-syscalls.md).
Install packages and service definitions are documented in
[docs/install-and-services.md](docs/install-and-services.md).
The full kernel, boot, storage, networking, process, and build architecture is
summarized in [docs/system-overview.md](docs/system-overview.md).

<details>

<summary>Commands in Terminal</summary>

### Commands in MinimalOS terminal

#### General
* ```help``` [-c | --classic] Shows grouped, paged command help. Press Enter for the next category or `q` to exit. Use -c or --classic to use the old list.
* ```echo``` [Text] Echoes text back to the terminal.
* ```clear``` Clears the terminal.
* ```sleep``` [Seconds | milliseconds"ms"] Pauses for seconds or milliseconds.
* ```systime``` Shows the current system time.

#### System information
* ```info``` Shows system, CPU, PCI, and memory information.
* ```cpu``` Shows CPU information.
* ```gpu``` Shows GPU information.
* ```memsize``` Returns the size of available memory in MB.
* ```usage``` Shows memory and process usage.
* ```chgres``` [Width] [Height] Changes the display resolution.

-------------------------------------------------------
#### Audio related commands
* ```play``` [File path] Plays `.adi` files from the disk.
* ```stop``` Stops music or audio playback.
* ```pause``` Toggles pause and resume of music or audio playback.
* ```volume``` [0 - 100] Sets the system volume.
-------------------------------------------------------
#### Files and directories
* ```list```, ```ls``` [Path] [--recursive] Lists the current or entered path.
* ```listroot```, ```lsroot``` Shows the root of the disk, including metadata.
* ```cd``` [Path] Changes the current directory.
* ```mdr``` [Path] [--hidden] Creates a folder at the entered path.
* ```insert``` [File Path] [Text] Creates or replaces a file with [Text].
* ```read```, ```rd``` [Path] [--metadata] Reads a file.
* ```remove```, ```rm``` [Path] [-r | --recursive] Removes a file or folder.
* ```meta``` [Option] [File] Views or edits file metadata.

#### Storage and packages
* ```initdisk``` [Index] Searches for and initializes an available disk.
* ```format``` [Index] Formats the selected disk as MinimaFS.
* ```mount``` [Index] Mounts the selected disk.
* ```importmusic``` Copies the built-in music file to the disk as `music.adi`.
* ```pkg``` Extracts or creates a package archive.
  * ```unzip``` [Archive] [Target] Extracts to a folder named after the archive by default.
  * ```zip``` [File or folder] [lzss | store] Creates a package archive.
* ```pkginfo``` [Archive] Displays package information.
  * ```installpkg``` [Archive] [Target] Alias for `pkg unzip`.

During `make build-x86_64`, the SDK builds `minimaSSL.slib`, copies it into
`src/resources/install1`, and packages that directory as an embedded
`install1.mpkg`. Running `format` extracts the package into `0:/Etc`, so the
library is installed as `0:/Etc/minimaSSL.slib`. The kernel TLS client is
linked with the same freestanding minimaSSL crypto sources; the current OS has
no dynamic `.slib` loader, so the filesystem copy is not executable code by
itself.

#### Keyboard and programs
* ```kbrlayout``` [set | get | list] Manages keyboard layouts.
* ```run``` [File] Runs a `.run` executable.
* ```services``` Lists registered services.

#### Adding a service

Put a file ending in `.service` in `src/resources/install3`. It is packaged
automatically and installed to `0:/services` when `format` runs. Services are
registered automatically after the boot disk is mounted; no C registration
call is needed.

Example `src/resources/install3/example.service`:

```ini
[GENERAL]
name="Example Service"
desc="Starts an example program"
exit=Restart

[STARTUP]
exec="0:/programs/example.run"
```

Use exactly one `[STARTUP]` entry. `exec` can load a `.run` bundle directly or
run a terminal command such as `exec="echo service started"`.
`script="run 0:/programs/example.run"` starts through the command system.
Command-style entries run once without PID tracking. The `services` command
lists successfully parsed and registered services.

#### Diagnostics
* ```testwrite``` Runs the MinimaFS write test.
* ```verifyfile``` Verifies a file on disk.
* ```diskusage``` Shows disk usage.
* ```trace_on``` Enables execution tracing.
* ```trace_off``` Disables execution tracing.
* ```trace_stack``` Shows the execution trace stack.

-------------------------------------------------------

#### Keyboard related commands
* ```kbrlayout``` Handles kbr layout stuff.
  * ```set``` [inbuiltlayout | kbr file path] sets the current and default keyboard layout.
  * ```get``` Outputs the currently used keyboard layout.
  * ```list``` lists all available keyboard layouts.

-------------------------------------------------------

#### Path formatting

0:/Dir/example.txt

0: is the disk

0:/ is the root of that disk or the top directory.

0:/Dir is a folder inside disk 0

0:/Dir/example.txt is a file with .txt suffix which is called "example"

</details>

## ROADMAP

Last synced 2026 - Sep 15 - 09:24

#### X = Done
#### C = Come back later
#### N = New plans

<details>

<summary>Finished</summary>

#### V 0.4.00

- [x] Panic screen 
- [x] simple audio [C]
- [x] allocator 
- [x] startupRoutine 
- [x] Processes and ProcessManager 
- [x] GRAPHICS!!! 🥳🥳🥳

#### V 0.4.05

- [x] Better allocator 
- [x] Time Scheduler 
- [x] Improved scheduler and proc 
- [x] Graphics handler (display.h) 
- [x] Time handler (time.h) 

- [x] Displaying and reading images !limited support!
- [x] Fonts !also a little limited for now!

- [x] USB 1.1 
- [x] Keyboard
- [x] Simple Audio ! 
- [x] File system !!! (MinimaFS)

</details>

#### V 0.4.4X

- [X] Multithreading And Multicore usage 
- [X] Services manager 
- [X] Syscalls and User layer
- [X] .run executable file

#### V 0.4.5X

- [ ] Simple Desktop 
- [ ] Window Manager 
- [ ] MinimaFS file browser (within minimal-os) 
- [ ] Text editor 

## Usage

To run Minimal-OS (direct iso) download from github releases ( [Minimal-OS Downloads](https://github.com/fsminecrafter/Minimal-OS/releases) )
Then after downloading the .iso file of any version use qemu-system-x86_64 which can be downloaded using the systems package manager or Mingw on windows.
And before running the operating system create a disk file for it to use in the same directory as you are running from by using this command ```fallocate -l 256M sata256.img```
Then run this command for the suitable configuration for current Minimal-OS support.
```
qemu-system-x86_64 -cdrom kernel.iso -m 1024M -boot d -d guest_errors,int,cpu_reset,unimp -serial -usb -device usb-kbd stdio \
-audiodev pa,id=speaker -machine pcspk-audiodev=speaker  -audiodev pa,id=audio0 -device AC97,audiodev=audio0 -device ahci,id=ahci \
-drive id=disk0,file=sata256.img,if=none,format=raw -device ide-hd,drive=disk0,bus=ahci.0
```

### Building from Source

 1. Download the source code, ```git clone https://github.com/fsminecrafter/Minimal-OS.git```
 2. Enter the folder and run the dependency install command (this one is for debian 13, Ubuntu 24.04/26.04 or at least tested on them)
```
sudo apt update
sudo apt install -y \
  build-essential \
  bison \
  flex \
  libgmp3-dev \
  libmpc-dev \
  libmpfr-dev \
  texinfo \
  libisl-dev \
  wget \
  curl \
  make \
  nasm \
  xorriso \
  grub-pc-bin \
  grub-common \
  mtools \
  qemu-system
```
3. Run the included gcc / binutils installer
```
./build_gccbinutils.sh
```
4. Create a sata256.img file in root of Minimal-OS project folder.
```
fallocate -l 256M sata256.img
```
5. Compile and run using make
```make run -j$(nproc)```
If audio doesnt work use either
```make run-sdl``` or ```make run-alsa```

## Development For Minimal-OS Via .run Executables
Now you can create .run files for Minimal-OS viaaa... (Drumroll)

[Minimal-OS SDK](https://github.com/fsminecrafter/Minimal-OS-SDK)

More info is found on ```Minimal-OS SDK```s Github Page.


# Development Stuff

Format / Install Placement

* install1 -> 0:/Etc
*  install2 -> 0:/programs
* install3 -> 0:/services