# Minimal-OS
A os made in mostly C and is the most minimal ever made.

find roadmap in roadmap.txt or below (might be old or un-synced!)

<details>

<summary>Commands in Terminal</summary>

### Commands in MinamalOS terminal

* ```help``` List all available commands
* ```echo``` [Text] Echoes text back to terminal
* ```memsize``` Returns the size of available memory in MB
* ```initdisk``` [Index] Searches and activates the disk(s) available
* ```format``` [Index] Formats the Selected disk to MinimaFS
* ```mount``` [Index] Mounts the selected disk
* ```createmusicfile``` Takes the music.wav from `src/resources` and puts it on disk as music.adi
* ```play``` [File path] Plays `.adi` files from the disk.
  * ```stop``` Stops the playing of music / audio
  * ```pause``` toggle pause and unpause of the music / audio
  * ```volume``` [0 - 100] Sets the system volume
* ```read``` (--metadata) Reads a file and outputs it to the terminal
* ```listroot``` list the root of the disk

</details>

## ROADMAP

Last synced 2026 - Mar 21 - 16:05

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
- [ ] Simple Audio ! 
- [x] System calls !! 
- [x] File system !!! (MinimaFS)
