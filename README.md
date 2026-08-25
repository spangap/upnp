# upnp — UPnP/IGD port mapping

## What is this?

**upnp** is a UPnP IGD (Internet Gateway Device) port-mapping client for
[spangap](../spangap) devices. It discovers the home router on the LAN and asks
it to map external ports to the device, so the device's HTTPS server and WebRTC
data channel are reachable from outside the NAT.

Paired with [duckdns](../duckdns) (a stable public hostname) and
[acme](../acme) (a real TLS certificate), it completes the remote-access stack
for a NAT'd device on a residential connection. The three are independent — use
any subset. See the cross-cutting overview in
[spangap-core/docs/remote-access.md](../spangap-core/docs/remote-access.md).

## What it does

When the network comes up, upnp finds the gateway by SSDP multicast, reads its
device description, and installs port mappings via SOAP:

- **HTTPS over TCP** — the device's HTTPS listener (`s.net.https_port`, default
  443) mapped from an external TCP port.
- **WebRTC over UDP** — the device's WebRTC data-channel port
  (`s.net.webrtc_port`, default 4433) mapped from the same UDP port.
- **HTTP over TCP**, with `s.upnp.fwd_http` set — the device's HTTP listener
  (`s.net.http_port`) mapped from external port **80**, which is not a
  preference: an ACME HTTP-01 challenge is fetched on port 80 or it is not
  fetched at all.

The external TCP port for HTTPS is `s.upnp.ext_port`, seeded on first boot from
the port the web server is actually configured for. The mappings carry a
3600-second lease and are renewed by a cron entry every 15 minutes, so they survive router
reboots and lease expiry. The device's identity in the router admin UI is its
hostname (`s.net.hostname`) for the TCP mapping and `<hostname>-webrtc` for the
UDP one.

upnp starts automatically when the straddle is in the build — there is no init
call to make. It registers for network up/down events, a cron renewal, and two
CLI verbs.

### Lifecycle

- On `NET_EV_UPSTREAM_UP` (and on the cron tick, and the `upnp update` CLI), upnp
  re-discovers the gateway if needed, refreshes the external IP, and re-installs
  both mappings.
- On `NET_EV_UPSTREAM_DOWN`, upnp **deletes** every mapping it installed
  (synchronously, before the network goes away) and drops its discovered state,
  so it never leaves stale mappings on the router.

## What it does NOT do

- It does not configure IPv6 — IPv6 reachability is typically not gated by
  NAT/IGD on a residential router.
- It does not defeat carrier-grade NAT — if the upstream ISP NATs you, port
  mapping on the home router won't help. Use a [wg](../wg) tunnel instead.
- It has no socket/HTTP control surface of its own; storage and the CLI are the
  only controls, and live status is RAM-only (queried with the `upnp` CLI).

## Settings

upnp owns two settings, surfaced as a generated **Settings → WiFi & Network → UPnP**
pane (an Enable switch, an External-port field and the port-80 switch — no live
mapping view; re-mapping is the `upnp update` CLI):

| Key | Default | Meaning |
|---|---|---|
| `s.upnp.enable` | `0` | Master switch. When `0`, no discovery or mapping happens. |
| `s.upnp.ext_port` | `s.net.https_port` | Desired external TCP port for HTTPS, seeded from the configured HTTPS port because that is a number only the device knows. The router may decline the requested port. |
| `s.upnp.fwd_http` | `0` | Also map external port 80 to the device's HTTP listener — what ACME's web-auth challenge needs. |

It also reads keys owned by other straddles (it never defines or defaults them):

| Key | Owner | Use |
|---|---|---|
| `s.net.https_port` | [spangap-web](../spangap-web) (via net) | Internal HTTPS port mapped over TCP (default 443), and what `s.upnp.ext_port` is seeded from. |
| `s.net.http_port` | [spangap-web](../spangap-web) (via net) | Internal HTTP port, mapped from external 80 while `s.upnp.fwd_http` is set. |
| `s.net.webrtc_port` | [spangap-web](../spangap-web) | WebRTC port mapped over UDP (default 4433). |
| `s.net.hostname` | [spangap-net](../spangap-net) | Used as the mapping description in the router UI. |

There are no storage keys for live mapping state — the discovered gateway,
external IP, and active mappings live only in RAM and are reported by the `upnp`
CLI.

## CLI

```
upnp           UPnP port-mapping status (gateway, external IP, active mappings)
upnp update    renew port mappings + refresh external IP
```

`upnp` prints `disabled` when `s.upnp.enable` is `0`, otherwise
`searching`/`discovered`, then the gateway, external IP, and each active mapping.
Run either on-device through `spangap cli "<command>"`.

## Dependencies

- [spangap-net](../spangap-net) — IP stack + LAN UDP multicast (SSDP) and the
  `NET_EV_UPSTREAM_*` events, local-IP query, and `netActivity()`.

## Read next

- [INTERNALS.md](INTERNALS.md) — SSDP discovery, the SOAP mapping flow, the
  async task model, and maintainer pitfalls.
- [spangap-core/docs/remote-access.md](../spangap-core/docs/remote-access.md) —
  how upnp/duckdns/acme fit together.
