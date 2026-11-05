# noVNC desktop

This optional Docker Compose service provides an XFCE desktop with Firefox ESR.
The desktop profile, including Firefox data, is kept in a named Docker volume.
The repository is mounted at `/Minimal-OS` and linked from
`~/Documents/Minimal-OS`. The host's `$HOME/cross` cross-toolchain is mounted
read-only at both its original path and `~/cross` in the desktop container,
and is added to `PATH`.
The image includes the kernel build dependencies (including QEMU and the
cross-toolchain build libraries) in addition to the desktop packages.

## Start

From the repository root, copy `env/vnc/.env.example` to `env/vnc/.env` and set
`NOVNC_PASSWORD` to exactly eight alphanumeric characters. The VNC password
protocol used by this desktop accepts only eight characters. The local `.env`
file is ignored by Git. The account used to run Compose must have the
cross-toolchain at `$HOME/cross`.

Build and start the desktop:

```sh
docker compose up -d --build
```

Open `http://localhost:6080/vnc.html?autoconnect=true&resize=remote` in a
browser. Enter the VNC password when prompted. Port 6080 is published only on
the Docker host's loopback interface; keep any forwarded workspace port private.

Stop the service with `docker compose down`. The named desktop profile volume
is retained, so starting the service again preserves Firefox data.
