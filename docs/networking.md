# Networking

## Stack Layers

The kernel network path is:

```text
RTL8139 -> Ethernet -> IPv4/ARP -> UDP/TCP -> syscall handles
```

`network_manager` selects the NIC driver. Ethernet frames are passed to IPv4,
which dispatches UDP and TCP payloads. ARP resolves next-hop IPv4 addresses to
MAC addresses. The protocol implementations live in `src/impl/net`.

The stack uses host-order IPv4 addresses in its internal and user syscall APIs.
Only wire headers use network byte order.

## Initialization

`kernel_main()` registers the RTL8139 driver and initializes Ethernet, IP, UDP,
TCP, and `net_syscall`. The NIC must be initialized before protocol traffic can
be sent. `network_manager_poll()` pumps received frames and is called from
blocking protocol operations and user polling wrappers.

The stack has bounded connection, listener, handle, and receive-queue tables.
The `SYS_ERR_BUSY` result means another caller currently owns a non-reentrant
network operation; retry it rather than treating it as a permanent failure.

## DHCP

The terminal `dhcp` command sends a DISCOVER, waits for an OFFER, sends a
REQUEST, and waits for an ACK. It configures:

- guest IPv4 address;
- netmask;
- gateway;
- DNS server.

DHCP is intentionally an operator action. A user `.run` program calling
`mos_net_dhcp()` receives `SYS_ERR_PERM` because changing the lease changes the
machine-wide interface configuration.

## DNS

`dns_resolve()` sends a UDP A-record query to the configured DNS server. The
resolver registers a temporary UDP handler, pumps the NIC while waiting, and
removes the handler when the query completes or times out.

The terminal `wget` command resolves a hostname when the URL does not contain a
numeric address. DNS failures are reported separately from TCP and TLS failures.

## TCP

TCP supports active connections, listeners, accept queues, retransmission,
ACKs, receive buffers, and bounded send windows. User programs receive integer
handles rather than raw `tcp_conn_t*` pointers. Handles are owned by the
creating process and are closed when the owner dies.

Client pattern:

```c
long h = mos_tcp_connect(ip, 80, 5000);
if (h > 0) {
    mos_tcp_send(h, request, request_len);
    mos_net_poll();
    mos_tcp_recv(h, buffer, sizeof buffer);
    mos_tcp_close(h);
}
```

Server pattern:

```c
long listener = mos_tcp_listen(8080, 4);
long client = mos_tcp_accept(listener, &peer_ip, &peer_port);
```

`mos_tcp_accept()` is non-blocking. It returns zero when the backlog is empty
and `SYS_ERR_AGAIN` is normalized to zero by the SDK wrapper. A server should
sleep briefly between attempts. `mos_tcp_handoff()` transfers an accepted
connection to a child process.

## UDP

A user program binds a UDP handle, receives queued datagrams, and sends to a
host-order destination address. Datagrams larger than the destination buffer
are truncated and the remainder is discarded.

Broadcast uses `SYSCALL_NET_IP_BROADCAST`. The QEMU user-mode network backend
does not forward LAN broadcasts beyond the guest, so broadcast discovery needs
a tap or bridged network configuration.

## TLS And Wget

The kernel `wget` command performs a TLS 1.3 client handshake over TCP. The
current tested cipher path is `TLS_AES_128_GCM_SHA256`:

1. create a ClientHello with X25519 key share;
2. parse ServerHello and derive handshake traffic secrets;
3. authenticate encrypted handshake records;
4. verify the server Finished message;
5. derive application traffic secrets;
6. send the HTTP request in an encrypted application record;
7. decrypt the HTTP response.

The TLS client defers certificate verification. It is suitable for the current
controlled integration test but should not be treated as a complete web PKI
client. `tools/debug/https_test.sh` runs an OpenSSL TLS 1.3 server and verifies
this path end to end.
