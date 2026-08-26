/**
 * UPnP IGD client — SSDP discovery + SOAP port forwarding.
 *
 * Forwards this device's HTTPS and WebRTC ports on the gateway, port 80 for an
 * ACME challenge when asked, and every listener net reports as public-facing.
 * Discovery and mapping run on temp tasks; the teardown on net-down is
 * synchronous, because the link is about to go.
 */
#ifndef SPANGAP_UPNP_H
#define SPANGAP_UPNP_H

#include "service.h"

/** UPnP IGD service — registers net event callbacks + CLI at boot. */
class UpnpService : public Service {
public:
    void onInit() override;
};

/** Returns cached external IP, or "" if not discovered. */
const char* upnpExternalIp();

#endif
