# noVNC desktop

This optional Docker Compose service provides an XFCE desktop with Firefox ESR.
The desktop profile, including Firefox data, is kept in a named Docker volume.

## Start

From the repository root, copy `env/vnc/.env.example` to `env/vnc/.env` and set
`NOVNC_PASSWORD` to exactly eight alphanumeric characters. The VNC password
protocol used by this desktop accepts only eight characters. The local `.env`
file is ignored by Git.

Build and start the desktop:

```sh
docker compose up -d --build
```

Open `http://localhost:6080/vnc.html?autoconnect=true&resize=remote` in a
browser. Enter the VNC password when prompted. Port 6080 is published only on
the Docker host's loopback interface; keep any forwarded workspace port private.

Stop the service with `docker compose down`. The named desktop profile volume
is retained, so starting the service again preserves Firefox data.
