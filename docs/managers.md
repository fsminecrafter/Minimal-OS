# Device managers

Calls into the kernel's GPU, audio, storage and USB managers over
`SYS_MANAGER`. Include `x86_64/user_managers.h`.

These exist for kernel-privileged code such as the boot sequence and the
terminal. A `.run` program may only call
[`mos_audio_has_data`](#mos_audio_has_data); every other operation returns
`SYS_ERR_PERM` to a user process, because it reconfigures hardware the
whole system shares.

## `mos_audio_has_data`

```c
uint64_t mos_audio_has_data(void);
```

**Returns** `1` if the audio manager has data queued, `0` if not. Allowed
for user programs.

## `mos_audio_init`

```c
uint64_t mos_audio_init(void);
```

Initialises the audio manager (AC'97 or PC speaker). Kernel-privileged.

**Returns** `SYS_SUCCESS` or `SYS_ERR_GENERIC`.

## `mos_audio_start`

```c
uint64_t mos_audio_start(void);
```

Starts audio playback. Kernel-privileged.

## `mos_audio_update`

```c
uint64_t mos_audio_update(void);
```

Lets the audio manager refill its buffers. Kernel-privileged.

## `mos_audio_set_sample_rate`

```c
uint64_t mos_audio_set_sample_rate(uint32_t rate_hz);
```

Sets the output sample rate in Hz. Kernel-privileged.

## `mos_gpu_init`

```c
uint64_t mos_gpu_init(void);
```

Initialises the GPU manager. Kernel-privileged.

**Returns** `SYS_SUCCESS` or `SYS_ERR_GENERIC`.

## `mos_storage_init`

```c
uint64_t mos_storage_init(void);
```

Probes storage controllers. Kernel-privileged.

## `mos_usb_manager_init`

```c
uint64_t mos_usb_manager_init(void);
```

Initialises the USB manager. Kernel-privileged.

## `mos_manager`

```c
uint64_t mos_manager(uint64_t manager, uint64_t operation, uint64_t value);
```

Issues `SYS_MANAGER`.

| Parameter | Description |
| --- | --- |
| `manager` | `SYS_MANAGER_GPU`, `_AUDIO`, `_STORAGE` or `_USB` |
| `operation` | `SYS_MANAGER_INIT`, `_UPDATE`, `_START`, `_HAS_DATA` or `_SET_SAMPLE_RATE` |
| `value` | Operation argument (the sample rate), otherwise 0 |

**Returns** the manager's result, or `SYS_ERR_INVAL` for an unknown
manager or operation.
