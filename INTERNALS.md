# upnp — internals

## Discovery

SSDP M-SEARCH on the lwIP UDP multicast group 239.255.255.250:1900,
filtered for an IGD `urn:schemas-upnp-org:device:InternetGatewayDevice`
response. The control URL is parsed out of the device description XML.

## Mapping

`AddPortMapping` (or `AddAnyPortMapping` when the requested external
port is in use) with a lease duration; the cron entry refreshes
ahead of expiry. On a request where the router declines the desired
external port, the assigned port is recorded back to
`upnp.mapped_port`.

## When upstream changes

`netRegister(NET_EV_UPSTREAM_UP, …)` re-discovers and re-installs the
mapping. `NET_EV_UPSTREAM_DOWN` drops local state but does not attempt
a `DeletePortMapping` — the router will time it out.

## Limits

- TCP only.
- One mapping at a time (HTTPS port).
- IGD-1 and IGD-2 both supported.
- Some consumer routers lie about success or silently rate-limit
  AddPortMapping; the operator panel shows the last response code.
