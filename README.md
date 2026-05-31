# upnp

## What is this?

**upnp** is a UPnP IGD (Internet Gateway Device) port-mapping client for
[spangap](../spangap) devices: it talks to the home router and asks for
an external TCP port to be forwarded to the device, so the device can be
reachable from outside the LAN.

Paired with [duckdns](../duckdns) (a public DNS name) and
[acme](../acme) (a real TLS cert), it completes the remote-access stack
for a NAT'd device on a residential connection.

## What this straddle owns

```
upnp/
└── esp-idf/
    ├── include/upnp.h
    └── src/upnp.cpp
```

Plus a browser settings panel for the operator to enable/disable, view
current mapping, and force re-map.

## How others use it

```cpp
upnpInit();   // after netInit
```

Configuration:

- `s.upnp.enable` — on/off
- `s.upnp.external_port` — desired external TCP port (router may
  decline)
- `s.upnp.internal_port` — the device-side port to forward to (the
  HTTPS port from `spangap-web`/`tls`)
- Live state: `upnp.external_ip`, `upnp.mapped_port`, `upnp.gateway`
  (ephemeral, mirrored to the browser).

A cron entry re-discovers the gateway and refreshes the lease
periodically; the mapping is re-installed after `NET_EV_UPSTREAM_UP`.

## Dependencies

- [spangap-net](../spangap-net) — IP stack + SSDP multicast.

## What it does NOT do

- It does not punch holes for UDP — TCP only at present.
- It does not configure IPv6 — IPv6 forwarding is typically not gated
  by NAT/IGD on a residential router.
- It does not handle CG-NAT — if the upstream ISP NATs you, port
  mapping won't help. Use a [wg](../wg) tunnel instead.

## Read next

- [INTERNALS.md](INTERNALS.md) — SSDP discovery, lease policy, retries.
- Cross-cutting remote-access doc:
  [spangap-core/docs/remote-access.md](../spangap-core/docs/remote-access.md).
