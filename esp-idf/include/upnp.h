/**
 * UPnP IGD client — SSDP discovery + SOAP port forwarding.
 * Forwards HTTPS port on the gateway. All operations async on temp tasks.
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
