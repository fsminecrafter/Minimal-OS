# Minimal-OS
A os made in mostly C and is the most minimal ever made.

find roadmap in roadmap.txt or below (might be old or un-synced!)

<details>

<summary>Commands in Terminal</summary>

### Commands in MinamalOS terminal

* ```help``` List all available commands
* ```echo``` [Text] Echoes text back to terminal
* ```memsize``` Returns the size of available memory in MB
-------------------------------------------------------
#### Audio related commands
* ```play``` [File path] Plays `.adi` files from the disk.
* ```stop``` Stops the playing of music / audio
* ```pause``` toggle pause and unpause of the music / audio
* ```volume``` [0 - 100] Sets the system volume
-------------------------------------------------------
#### MinimaFS related commands
* ```listroot``` list the root of the disk
* ```list```, ```ls``` [Path] (--recurvesive) lists the current or entered path.
* ```cd``` [Path] Change the current directory.
* ```mdr``` [Path + Folder Name] Creates a folder at the entered path.
* ```insert``` [File Path] [Text] Creates a new file with the contents of [Text].
* ```read```, ```rd``` (--metadata) Reads a file and outputs it to the terminal
* ```initdisk``` [Index] Searches and activates the disk(s) available
* ```format``` [Index] Formats the Selected disk to MinimaFS
* ```mount``` [Index] Mounts the selected disk
* ```importmusic``` Takes the music.wav from `src/resources` and puts it on disk as music.adi

-------------------------------------------------------

#### Keyboard related commands
* ```kbrlayout``` Handles kbr layout stuff.
  * ```set``` [inbuiltlayout | kbr file path] sets the current and default keyboard layout.

-------------------------------------------------------

#### Path formatting

0:/Dir/example.txt

0: is the disk

0:/ is the root of that disk or the top directory.

0:/Dir is a folder inside disk 0

0:/Dir/example.txt is a file with .txt suffix which is called "example"

</details>

## ROADMAP

Last synced 2026 - Aug 18 - 9:47

#### X = Done
#### C = Come back later
#### N = New plans

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

#### V 0.4.4X

- [ ] Multithreading And Multicore usage 
- [ ] Services manager 
- [ ] Syscalls and User layer  

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
```make run```
If audio doesnt work use either
```make run-sdl``` or ```make run-alsa```
