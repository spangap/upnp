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
- **Mapping bookkeeping** — the in-RAM `activeForwards` table of installed
  mappings (`trackForward`), used to delete them on shutdown.
- **Lifecycle wiring** — net up/down event handlers, a cron renewal entry, and
  the `upnp` / `upnp update` CLI verbs (`upnpInit`).
- **`upnpExternalIp()`** — the only public C API beyond `upnpInit()`; returns the
  cached external IP (or `""`).

## 2. Task model

All network work runs on short-lived temp tasks, never on a caller's task.
`upnpUpdate()` spawns `upnpSyncTask` (`spawnTask(..., "upnp", 6144, …, prio 1,
core 0)`) and returns immediately; the task does discovery + mapping then
`killSelf()`s. A `volatile bool syncBusy` guard makes `upnpUpdate()` a no-op
while a sync is already in flight, so overlapping triggers (net-up + cron +
CLI) coalesce to one run. `upnpUpdate()` is also gated on `s.upnp.enable`.

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
3. **HTTPS/TCP mapping** — external port is `s.upnp.ext_port`, or `s.net.https_port`
   when that is `0`; internal port is always `s.net.https_port`. Skipped if
   `s.net.https_port <= 0`.
4. **WebRTC/UDP mapping** — `s.net.webrtc_port` mapped to itself (same external
   and internal port). Skipped if `0`.

`addPortMapping` sends `AddPortMapping` with `NewLeaseDuration` **3600**. If the
router answers with an `errorCode` (some reject non-zero leases), it retries once
with lease **0** (permanent). There is no `AddAnyPortMapping` fallback — only
`AddPortMapping`. The mapping descriptions are the hostname (TCP) and
`<hostname>-webrtc` (UDP) so they read clearly in the router admin UI.

DuckDNS updates are **not** piggy-backed on the UPnP sync — they are driven by
their own cron entry in the [duckdns](../duckdns) straddle.

## 5. Renewal & state

`upnpInit` seeds a cron entry `*/15 * * * * N` → `upnp update` (every 15
minutes), which renews the leases ahead of the 3600 s expiry and re-asserts the
mappings after a router reboot. Net-up re-installs immediately; net-down deletes.

`activeForwards` (`std::vector<fwd_t>{extPort,intPort,proto}`) is the in-RAM
record of what was installed, used by `upnpStop()` to delete exactly those
mappings. There is no persisted live state: gateway, external IP, and mappings
are reconstructed on every sync and reported through the `upnp` CLI only.

## 6. Pitfalls

- **Don't leave mappings behind.** `upnpStop()` must delete synchronously on
  net-down; deferring to a task loses the window. Keep `activeForwards` accurate
  so the delete set matches what was installed.
- **`syncBusy` serializes syncs.** It is the only thing preventing two
  `upnpSyncTask`s from racing the shared `gw*`/`extIp`/`activeForwards` state.
  Don't add a second trigger path that bypasses `upnpUpdate()`.
- **Plain HTTP on the LAN.** SSDP and SOAP are unauthenticated HTTP to the
  gateway — correct for IGD, but it means upnp trusts whatever answers the
  multicast. The first M-SEARCH response wins.
- **The header comment "Renewal timer every 20 minutes" is stale.** There is no
  `esp_timer`; renewal is the `*/15` cron entry above. (Code cleanup: fix the
  comment.)

## 7. Code-cleanup notes

These do not describe behavior to rely on — they are pending tidies:

- **`s.upnp.version` config gate.** `upnpInit` reads `s.upnp.version` and seeds
  the cron entry only when it is below `UPNP_VERSION` (1). With no users and the
  no-migrations policy, this version gate should be dropped and the cron default
  seeded unconditionally; the `s.upnp.{enable,ext_port}` defaults already come
  from the generated `settings:` block.
- **Vestigial `browser/` module.** `straddle.yaml` still has a `browser: browser`
  field and a `browser/` directory containing only `package.json` (its `src/`
  has empty `modules/` and `panels/`). The web pane is generated from the
  `settings:` block, so the browser module and the `browser:` field are dead.
- **Stale header doc.** `include/upnp.h` says "Forwards HTTPS port" and "All
  operations async" — it now also maps WebRTC/UDP, and `upnpStop` is synchronous.
