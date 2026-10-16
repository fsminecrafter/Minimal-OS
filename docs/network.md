# Networking

IPv4 networking over `SYS_NET`: interface status, DNS, TCP clients and
servers, UDP and the experimental ktls transport. Include `net.h`.

Before a program can use the network the interface needs an address. Run
`dhcp` in the terminal first; a `.run` program cannot do it itself (see
[`mos_net_dhcp`](#mos_net_dhcp)).

Things to know about every call here:

- **Host order.** IPv4 addresses are `uint32_t` in host order
  (`192.168.1.10` is `0xC0A8010A`), and ports are plain numbers. You only
  swap bytes for values you put inside your own packets
  ([`mos_htons`](#mos_htons)).
- **Polled.** The kernel stack only makes progress when a process calls
  into it. A program that stops calling `recv` stops receiving. Loop with
  a short [`mos_sleep`](process.md#mos_sleep), or use the blocking helpers
  at the end of this page.
- **One at a time.** The stack is not reentrant. A call that finds it in
  use by another process returns `SYS_ERR_BUSY`; retry it.
- **Handles.** Connections, listeners, UDP bindings and TLS sessions are
  small positive integers in separate namespaces. They belong to the
  process that made them; a dead process's handles are closed the next time
  any process connects, listens or accepts.
- **No driver.** With no network card every call except
  `mos_net_status` and `mos_tcp_state` returns `SYS_ERR_GENERIC`.

## A small HTTP client

```c
#include "net.h"
#include "stdio.h"

int main(void) {
    if (!mos_net_is_up()) { mos_puts("run dhcp first\n"); return 1; }
    uint32_t ip = mos_net_lookup("example.com", 5000);
    if (!ip) { mos_puts("lookup failed\n"); return 1; }

    long c = mos_tcp_connect(ip, 80, 5000);
    if (c <= 0) { printf("connect failed (%ld)\n", c); return 1; }

    const char* req = "HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    uint32_t len = 0; while (req[len]) len++;
    mos_tcp_send_all(c, req, len);

    char buf[512];
    long idle = mos_uptime();
    for (;;) {
        long n = mos_tcp_recv(c, buf, sizeof buf - 1);
        if (n < 0) break;                                  /* closed */
        if (n == 0) {
            if (mos_uptime() - idle > 5000) break;
            mos_sleep(5);
            continue;
        }
        buf[n] = '\0'; mos_puts(buf); idle = mos_uptime();
    }
    mos_tcp_close(c);
    return 0;
}
```

## `mos_net_status`

```c
long mos_net_status(syscall_net_status_t* out);
```

Reports the interface configuration. Never touches the stack, so it never
returns `SYS_ERR_BUSY`.

| `syscall_net_status_t` field | Meaning |
| --- | --- |
| `has_driver` | `1` if a network card driver is loaded |
| `configured` | `1` if the interface has an IPv4 address |
| `mac[6]` | MAC address |
| `local_ip`, `netmask`, `gateway`, `dns` | Host-order IPv4 addresses |

**Returns** `SYS_SUCCESS`, or `SYS_ERR_INVAL` for a NULL pointer.

**Example**

```c
syscall_net_status_t st;
char ip[16];
mos_net_status(&st);
mos_ip_to_string(st.local_ip, ip);
printf("address %s\n", ip);
```

## `mos_net_is_up`

```c
bool mos_net_is_up(void);
```

**Returns** `true` if there is a driver and the interface has an address.

## `mos_net_poll`

```c
long mos_net_poll(void);
```

Processes whatever the network card has received, once. Cheap; safe in a
loop. `recv`, `accept` and `udp_recv` already poll, so most programs never
call this directly.

**Returns** `SYS_SUCCESS`, `SYS_ERR_BUSY`, or `SYS_ERR_GENERIC` with no
driver.

## `mos_net_resolve`

```c
long mos_net_resolve(const char* hostname, uint32_t* out_ip, uint32_t timeout_ms);
```

Looks up a host name with DNS.

| Parameter | Description |
| --- | --- |
| `hostname` | Name to resolve |
| `out_ip` | Receives the host-order address |
| `timeout_ms` | How long to wait; `0` means 5000 |

**Returns** `SYS_SUCCESS`, `SYS_ERR_NOTFOUND` if the name did not resolve,
`SYS_ERR_GENERIC` if the interface has no address, `SYS_ERR_INVAL` for a
NULL argument, `SYS_ERR_BUSY`.

## `mos_net_lookup`

```c
uint32_t mos_net_lookup(const char* host, uint32_t timeout_ms);
```

Accepts either a dotted-quad address or a host name.

**Returns** the host-order address, or `0` on failure.

**Example**

```c
uint32_t ip = mos_net_lookup(argc > 1 ? argv[1] : "10.0.2.2", 3000);
```

## `mos_net_dhcp`

```c
long mos_net_dhcp(uint32_t timeout_ms);
```

Acquires an address with DHCP. Kernel-privileged: a `.run` program always
gets `SYS_ERR_PERM`, because a lease changes the address for every process
on the machine. Use the terminal's `dhcp` command.

## `mos_tcp_connect`

```c
long mos_tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms);
```

Opens a TCP connection and waits for it to be established.

| Parameter | Description |
| --- | --- |
| `ip` | Host-order address, not 0 |
| `port` | Remote port, not 0 |
| `timeout_ms` | Connect timeout; `0` means 5000 |

**Returns** a connection handle (> 0), `SYS_ERR_GENERIC` if the connection
failed or timed out, there is no address or no free handle,
`SYS_ERR_INVAL` for a zero address or port, `SYS_ERR_BUSY`.

## `mos_tcp_send`

```c
long mos_tcp_send(long handle, const void* buf, uint32_t len);
```

Sends up to 4096 bytes of `buf`.

**Returns** the number of bytes sent, which can be fewer than `len` - a
short count is not an error. `SYS_ERR_BADFD` for a bad handle,
`SYS_ERR_INVAL` for a NULL buffer or zero length, `SYS_ERR_GENERIC` on
failure, `SYS_ERR_BUSY`.

**Notes** Use [`mos_tcp_send_all`](#mos_tcp_send_all) to send a whole
buffer.

## `mos_tcp_send_all`

```c
bool mos_tcp_send_all(long handle, const void* buf, uint32_t len);
```

Calls `mos_tcp_send` until all of `buf` is sent.

**Returns** `true` when everything was sent, `false` as soon as a send
fails.

## `mos_tcp_recv`

```c
long mos_tcp_recv(long handle, void* buf, uint32_t len);
```

Reads whatever has arrived, without waiting.

**Returns** the number of bytes read; `0` if nothing has arrived yet;
`SYS_ERR_NOTFOUND` once the peer has closed and everything has been read;
`SYS_ERR_BADFD`, `SYS_ERR_INVAL` or `SYS_ERR_BUSY`.

**Notes** `0` means "try again later", not end of stream. Sleep a few
milliseconds between empty reads.

## `mos_tcp_recv_exact`

```c
bool mos_tcp_recv_exact(long handle, void* buf, uint32_t len, uint32_t timeout_ms);
```

Reads exactly `len` bytes, sleeping between empty reads.

| Parameter | Description |
| --- | --- |
| `timeout_ms` | Gives up after this long with no new data. The timer restarts whenever data arrives |

**Returns** `true` when `len` bytes were read, `false` if the peer closed
or the timeout passed.

**Example**

```c
uint8_t header[4];
if (!mos_tcp_recv_exact(c, header, 4, 2000)) goto fail;
```

## `mos_tcp_state`

```c
long mos_tcp_state(long handle);
```

**Returns** one of `SYSCALL_NET_TCP_CLOSED`, `_CONNECTING`,
`_ESTABLISHED`, `_CLOSING` or `_PEER_CLOSED`, or `SYS_ERR_BADFD`. Never
returns `SYS_ERR_BUSY`.

## `mos_tcp_close`

```c
long mos_tcp_close(long handle);
```

Closes the connection and frees the handle.

**Returns** `SYS_SUCCESS`, `SYS_ERR_BADFD` or `SYS_ERR_BUSY`.

## `mos_tcp_listen`

```c
long mos_tcp_listen(uint16_t port, uint32_t backlog);
```

Starts accepting connections on `port`.

| Parameter | Description |
| --- | --- |
| `port` | Local port, not 0. Only one listener per port machine-wide |
| `backlog` | Connections to queue before accepting; `0` for a small default. Clamped by the kernel |

**Returns** a listener handle (> 0), valid only for
[`mos_tcp_accept`](#mos_tcp_accept) and
[`mos_tcp_unlisten`](#mos_tcp_unlisten). `SYS_ERR_INVAL` if the port is 0
or already has a listener, `SYS_ERR_GENERIC` if there is no address or
no free listener slot, `SYS_ERR_BUSY`.

## `mos_tcp_accept`

```c
long mos_tcp_accept(long listener, uint32_t* out_ip, uint16_t* out_port);
```

Takes the oldest waiting client. Never blocks.

| Parameter | Description |
| --- | --- |
| `listener` | Handle from `mos_tcp_listen` |
| `out_ip`, `out_port` | If not NULL, receive the client's address and port |

**Returns** a connection handle (> 0); `0` if nobody is waiting (or every
connection handle is in use); `SYS_ERR_BADFD`, `SYS_ERR_BUSY`.

**Example**

```c
long ls = mos_tcp_listen(8080, 0);
for (;;) {
    uint32_t ip; uint16_t port;
    long c = mos_tcp_accept(ls, &ip, &port);
    if (c > 0) { serve(c); mos_tcp_close(c); }
    else mos_sleep(10);
}
```

## `mos_tcp_unlisten`

```c
long mos_tcp_unlisten(long listener);
```

Stops listening and frees the listener handle. Connections already
accepted stay open.

**Returns** `SYS_SUCCESS`, `SYS_ERR_BADFD` or `SYS_ERR_BUSY`.

## `mos_tcp_handoff`

```c
long mos_tcp_handoff(long handle, uint32_t pid);
```

Gives a connection you own to another process, typically a child started
with [`mos_exec`](process.md#mos_exec) to serve it. The connection then
closes when that process dies instead of when you do.

| Parameter | Description |
| --- | --- |
| `handle` | Connection handle you own |
| `pid` | New owner, not 0 |

**Returns** `SYS_SUCCESS`, `SYS_ERR_PERM` if you do not own the handle,
`SYS_ERR_BADFD`, `SYS_ERR_INVAL` for pid 0.

**Notes** The child needs nothing special: pass it the handle number on
its command line and it can use it once the handoff is done. Until then
the parent still owns it.

**Example**

```c
char num[24]; /* format c into num */
const char* args[] = { num };
long pid = mos_exec("0:/programs/worker.run", args, 1);
if (pid > 0) mos_tcp_handoff(c, (uint32_t)pid);
```

## `mos_udp_bind`

```c
long mos_udp_bind(uint16_t local_port);
```

Opens a UDP port for receiving (and as a default source port).

**Returns** a UDP handle (> 0), `SYS_ERR_INVAL` for port 0, an error if
the port is taken or all 4 bindings are in use, `SYS_ERR_BUSY`.

## `mos_udp_unbind`

```c
long mos_udp_unbind(long handle);
```

Closes a UDP binding.

**Returns** `SYS_SUCCESS` or an error for a bad handle.

## `mos_udp_recv`

```c
long mos_udp_recv(long handle, void* buf, uint32_t len,
                  uint32_t* out_from_ip, uint16_t* out_from_port);
```

Takes the next queued datagram. Polls the card first; never blocks.

**Returns** the datagram's length (bytes copied), `0` if none is waiting,
`SYS_ERR_BADFD`, `SYS_ERR_INVAL`, `SYS_ERR_BUSY`.

**Notes** A datagram larger than `len` is truncated and the rest is lost.

## `mos_udp_recv_timeout`

```c
long mos_udp_recv_timeout(long handle, void* buf, uint32_t len,
                          uint32_t* out_from_ip, uint16_t* out_from_port,
                          uint32_t timeout_ms);
```

Waits up to `timeout_ms` for a datagram.

**Returns** its length, `0` on timeout, or a negative error.

## `mos_udp_send`

```c
long mos_udp_send(long handle, uint32_t dst_ip, uint16_t dst_port,
                  uint16_t src_port, const void* buf, uint32_t len);
```

Sends one datagram.

| Parameter | Description |
| --- | --- |
| `handle` | A UDP handle; used for its port when `src_port` is 0 |
| `dst_ip` | Destination, or `SYSCALL_NET_IP_BROADCAST` for 255.255.255.255 |
| `dst_port` | Destination port |
| `src_port` | Source port, or `0` to use the handle's |
| `buf`, `len` | Payload, 1 … 1024 bytes |

**Returns** `len`, `SYS_ERR_INVAL` for an empty or oversize payload or no
source port, `SYS_ERR_GENERIC` if sending failed or there is no address,
`SYS_ERR_BUSY`.

**Example**

```c
long u = mos_udp_bind(40000);
mos_udp_send(u, SYSCALL_NET_IP_BROADCAST, 40000, 0, "hello?", 6);
char reply[256]; uint32_t from; uint16_t fport;
long n = mos_udp_recv_timeout(u, reply, sizeof reply, &from, &fport, 2000);
```

**Notes** Under QEMU's `-netdev user` the guest is behind NAT and LAN
broadcasts never reach it; use a tap or bridge network to test discovery.

## `mos_tls_connect`

```c
long mos_tls_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms);
```

Opens a ktls session: a TCP connection plus a hello. The handshake
finishes in the background.

**Returns** a TLS handle (> 0) or a negative error.

**Notes** ktls is a transport shape, not real TLS: there are no
certificates and the session key crosses the wire in the clear. It does
not talk to ordinary HTTPS servers and gives no security. (The terminal's
`wget` uses the kernel's separate TLS 1.3 client.)

## `mos_tls_wait_established`

```c
bool mos_tls_wait_established(long handle, uint32_t timeout_ms);
```

Polls [`mos_tls_state`](#mos_tls_state) until the handshake completes.

**Returns** `true` when established, `false` if it failed or
`timeout_ms` passed.

## `mos_tls_accept`

```c
long mos_tls_accept(long tcp_handle);
```

Turns a TCP connection you own into a server-side ktls session.

**Returns** a TLS handle (> 0) or a negative error. On success the TCP
handle must no longer be used (closing the TLS handle closes it); on
failure it is still yours.

## `mos_tls_state`

```c
long mos_tls_state(long handle);
```

Advances the handshake and reports the session state.

**Returns** `SYSCALL_NET_TLS_CLOSED` (failed or closed),
`SYSCALL_NET_TLS_HANDSHAKING` or `SYSCALL_NET_TLS_ESTABLISHED`, or a
negative error (`SYS_ERR_BUSY`: retry).

## `mos_tls_send`

```c
long mos_tls_send(long handle, const void* buf, uint32_t len);
```

**Returns** bytes sent (at most 4096 per call; short counts are normal),
`SYS_ERR_AGAIN` while the handshake is still running, `SYS_ERR_BUSY`.

## `mos_tls_recv`

```c
long mos_tls_recv(long handle, void* buf, uint32_t len);
```

**Returns** bytes read, `0` if nothing has arrived, `SYS_ERR_NOTFOUND`
once the session is closed and drained.

## `mos_tls_close`

```c
long mos_tls_close(long handle);
```

Closes the session and its TCP connection.

**Returns** `SYS_SUCCESS` or a negative error.

## `mos_ip_parse`

```c
uint32_t mos_ip_parse(const char* dotted);
```

Parses `"a.b.c.d"`.

**Returns** the host-order address, or `0` if the string is not exactly
four decimal octets of 0-255.

**Example**

```c
uint32_t gw = mos_ip_parse("10.0.2.2");   /* 0x0A000202 */
```

## `mos_ip_to_string`

```c
void mos_ip_to_string(uint32_t ip, char* out);
```

Formats a host-order address as dotted decimal. `out` must hold at least
16 bytes.

## `mos_htons`

```c
uint16_t mos_htons(uint16_t v);
```

Swaps the bytes of a 16-bit value (host to network order). `mos_ntohs` is
the same function. Only needed for fields inside your own packet formats.

## `mos_htonl`

```c
uint32_t mos_htonl(uint32_t v);
```

Swaps the bytes of a 32-bit value. `mos_ntohl` is the same function.

## `mos_net_call`

```c
long mos_net_call(syscall_net_request_t* request);
```

Issues `SYS_NET` with a filled `syscall_net_request_t`. Every function on
this page builds a zeroed request, sets `op` and the fields that operation
uses, and calls this.
