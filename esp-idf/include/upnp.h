/**
 * UPnP IGD client — SSDP discovery + SOAP port forwarding.
 * Forwards HTTPS port on the gateway. All operations async on temp tasks.
 */
#ifndef SECCAM_UPNP_H
#define SECCAM_UPNP_H

/** Register UPnP net event callbacks + CLI. Call from main after netInit(). */
void upnpInit();

/** Returns cached external IP, or "" if not discovered. */
const char* upnpExternalIp();

#endif
