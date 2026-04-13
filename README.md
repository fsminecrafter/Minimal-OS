# Minimal-OS
A os made in mostly C and is the most minimal ever made.

find roadmap in roadmap.txt or below (might be old or un-synced!)

<details>

<summary>Commands in Terminal</summary>

### Commands in MinamalOS terminal

* ```help``` List all available commands
* ```echo``` [Text] Echoes text back to terminal
* ```memsize``` Returns the size of available memory in MB
* ```initdisk``` Searches and activates the disk(s) available
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

* Panic screen [X]
* simple audio [C]
* allocator [X]
* startupRoutine [X]
* Processes and ProcessManager [X]
* GRAPHICS!!! [X] 🥳🥳🥳

#### V 0.4.05

* Better allocator [X]
* Time Scheduler [X]
* Improved scheduler and proc [X]
* Graphics handler (display.h) [X]
* Time handler (time.h) [X]

* Displaying and reading images [X] !limited support!
* Fonts [X] !also a little limited for now!

* USB 1.1 [X]
* Keyboard [X]
* Simple Audio ! []
* System calls !! []
* File system !!! [X] (MinimaFS)
