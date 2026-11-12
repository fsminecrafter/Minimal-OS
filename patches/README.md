# Patches waiting for another repository

`Minimal-OS-SDK-os0.patch` is the SDK half of the OS-0 work (new syscalls in
`syscall.h`, `thread.h` additions, `process.h`, the environment in `crt0.c`,
`programs/os0test` and docs). It was written in a session that could not push
to the Minimal-OS-SDK repository. Apply it there, on top of commit `26794ca`:

```sh
cd Minimal-OS-SDK
git am ../patches/Minimal-OS-SDK-os0.patch
```

Then push it, point the `Minimal-OS-SDK` submodule at the new commit, and
delete this directory. Until then `make build-x86_64` stops at
`programs/os0test`, which only exists with the patch applied.
