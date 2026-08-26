# upnp — internals

Maintainer reference. The [README](README.md) is the operator guide; this
document is for changing the code without breaking it. It is self-authoritative.

## 1. What this straddle adds

upnp is a self-contained UPnP IGD client built directly on lwIP sockets and
`esp_http_client` — it is not a fork of any library. Everything here is new:

- **SSDP discovery** over raw UDP multicast (`ssdpDiscover`).
- **Device-description + control-URL parsing** from the gateway's root XML
  (`httpGet`, `xmlTag`, `parseUrl`).
- **SOAP client** (`httpSoap`) issuing the IGD actions `GetExternalIPAddress`,
  `AddPortMapping`, and `DeletePortMapping`.
- **The wanted set** (`buildWanted`) — the whole list of mappings this device
  wants right now, rebuilt from settings and `netPublicPorts()` on every sync.
- **Mapping bookkeeping** — the in-RAM `activeForwards` table of installed
  mappings, diffed against the wanted set each sync and used to delete them on
  shutdown.
- **Lifecycle wiring** — net up/down and ports-changed event handlers, a settings
  subscription, a cron renewal entry, and the `upnp` / `upnp update` CLI verbs
  (`upnpInit`).
- **`upnpExternalIp()`** — the only public C API beyond `upnpInit()`; returns the
  cached external IP (or `""`).

## 2. Task model

All network work runs on short-lived temp tasks, never on a caller's task.
`upnpUpdate()` spawns `upnpSyncTask` (`spawnTask(..., "upnp", 6144, …, prio 1,
core 0)`) and returns immediately; the task runs `upnpSyncOnce()` then
`killSelf()`s. A `volatile bool syncBusy` guard keeps `upnpUpdate()` from
starting a second one, so overlapping triggers (net-up + cron + ports-changed +
CLI) coalesce to one run. A trigger that arrives while a sync is in flight sets
`resyncPending` instead of being dropped, and the task loops once more before it
exits — net publishes a port the moment its socket opens, which is exactly when
a sync is likely to be running. The window between the last `resyncPending`
check and clearing `syncBusy` is still a lost race, and it costs a renewal
interval rather than a mapping. `upnpUpdate()` is also gated on `s.upnp.enable`.

`upnpStop()` (on `NET_EV_UPSTREAM_DOWN`) is the exception — it runs the
`DeletePortMapping` calls **synchronously** on the caller, because the network is
about to go down and a deferred task would never get to send them.

## 3. Discovery (`ssdpDiscover`)

A raw `SOCK_DGRAM` socket with a 4-second receive timeout sends an `M-SEARCH` to
`239.255.255.250:1900` with `MX: 3` and
`ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1`. It targets **only**
`InternetGatewayDevice:1`. The M-SEARCH is sent twice (some routers miss the
first datagram); the first response wins.

From the response's `LOCATION` header (parsed case-insensitively) it fetches the
root device XML, looks for a **`WANIPConnection`** service and falls back to
**`WANPPPConnection`** (PPP/DSL gateways), and extracts the `<controlURL>` from
inside that same `<service>` block. The control URL is resolved to an absolute
host/port/path (it may arrive absolute, root-relative, or bare). Discovery
results (`gwHost`, `gwPort`, `gwControlPath`, `gwServiceType`) are cached in the
`discovered` flag and reused until the next `upnpStop()`.

## 4. SOAP mapping flow (`upnpSyncTask`)

Per sync, on the temp task:

1. `netActivity()` (keep the link awake), discover if not already discovered,
   and read the local IP via `netGetLocalIp()` — bail if absent. The local IP is
   compared against `prevLocalIp` and logged only when it changes.
2. `GetExternalIPAddress` → cache `extIp`; compared against `prevExtIp` and
   logged only on change.
3. `buildWanted()` assembles the complete mapping set, in this order:
   - **HTTPS/TCP** — external port is `s.upnp.ext_port`, or `s.net.https_port`
     when that is `0`; internal port is always `s.net.https_port`. Skipped if
     `s.net.https_port <= 0`.
   - **HTTP/TCP** — external 80 → `s.net.http_port`, only while
     `s.upnp.fwd_http`.
   - **WebRTC/UDP** — `s.net.webrtc_port` mapped to itself. Skipped if `0`.
   - **Every port `netPublicPorts()` reports**, mapped to itself. upnp knows
     nothing about the services behind them: net hands it open ports whose
     registrant set `publicFacing`, and each one is a `<hostname>-<port>`
     mapping.

   `wantForward()` drops an entry whose external port + protocol a previous one
   already claimed, so the straddle's own three win over a listener that happens
   to sit on the same external port — the router keys a mapping on that pair,
   and two claims on it are one mapping fighting itself.
4. Reconcile: every `activeForwards` entry with no counterpart in the wanted set
   is `DeletePortMapping`ed **first** (a mapping whose service is gone is a hole
   in the NAT that outlives what it was opened for, and the router is the only
   place that record lives), then every wanted mapping is asserted with
   `AddPortMapping` — unconditionally, since that is also the lease renewal and
   what re-installs the set after a router reboot. `activeForwards` then becomes
   the wanted set, accepted or not: a mapping we asked for and did not get is
   one we still want deleted if it turns out to be there, and deleting a mapping
   that was never installed is a no-op.

`addPortMapping` sends `AddPortMapping` with `NewLeaseDuration` **3600**. If the
router answers with an `errorCode` (some reject non-zero leases), it retries once
with lease **0** (permanent). There is no `AddAnyPortMapping` fallback — only
`AddPortMapping`. The mapping description is the hostname for HTTPS and
`<hostname>-http` / `<hostname>-webrtc` / `<hostname>-<port>` for the rest, so
they read clearly in the router admin UI — the port is the only thing upnp knows
about a published listener, and it is enough to tell one line from another.

DuckDNS updates are **not** piggy-backed on the UPnP sync — they are driven by
their own cron entry in the [duckdns](../duckdns) straddle.

## 5. Renewal & state

`upnpApplyCron` keeps `s.cron.tab.upnp = "*/15 * * * * N upnp update"` in step
with `s.upnp.enable` (present while enabled — `storageDefault`, so a schedule
tweak survives — removed on disable), applied at init and via a
storage-task-hosted subscription on the enable key. The 15-minute tick renews
the leases ahead of the 3600 s expiry and re-asserts the mappings after a router
reboot. Net-up re-installs immediately; net-down deletes.

Two more triggers keep the set current between ticks, both landing on
`upnpUpdate()` so they cost at most one extra sync: `NET_EV_PORTS_CHANGED` (a
published listener opened or closed) and a second storage-task-hosted
subscription on the `s.upnp.` prefix (the external port or the port-80 switch
moved — a change to the mappings themselves, and the reconcile withdraws the old
one as it installs the new).

`activeForwards` (`std::vector<fwd_t>{extPort,intPort,proto,desc}`) is the
in-RAM record of what was installed, diffed against the wanted set on every sync
and used by `upnpStop()` to delete exactly those mappings. There is no persisted
live state: gateway, external IP, and mappings are reconstructed on every sync
and reported through the `upnp` CLI only.

## 6. Pitfalls

- **Don't leave mappings behind.** `upnpStop()` must delete synchronously on
  net-down; deferring to a task loses the window. Keep `activeForwards` accurate
  so the delete set matches what was installed.
- **`syncBusy` serializes syncs.** It is the only thing preventing two
  `upnpSyncTask`s from racing the shared `gw*`/`extIp`/`activeForwards` state.
  Don't add a second trigger path that bypasses `upnpUpdate()` — a new trigger
  calls it and lets `resyncPending` handle the overlap.
- **The wanted set is built, never accumulated.** `buildWanted()` starts from
  nothing every sync, so a mapping disappears from the router by disappearing
  from settings or from net's list. Anything that keeps state across syncs
  instead would need its own withdrawal path.
- **Plain HTTP on the LAN.** SSDP and SOAP are unauthenticated HTTP to the
  gateway — correct for IGD, but it means upnp trusts whatever answers the
  multicast. The first M-SEARCH response wins.

## 7. Code-cleanup notes

These do not describe behavior to rely on — they are pending tidies:

- **Vestigial `browser/` module.** `straddle.yaml` still has a `browser: browser`
  field and a `browser/` directory containing only `package.json` (its `src/`
  has empty `modules/` and `panels/`). The web pane is generated from the
  `settings:` block, so the browser module and the `browser:` field are dead.
