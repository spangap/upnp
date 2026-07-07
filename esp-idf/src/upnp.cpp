/**
 * UPnP IGD client — SSDP discovery + SOAP port forwarding.
 *
 * SSDP via raw UDP multicast. XML fetch and SOAP via esp_http_client (plain HTTP on LAN).
 * All HTTP work runs on temp tasks (6KB stack). Renewal timer every 20 minutes.
 */
#include "upnp.h"
#include "storage.h"
#include "cli.h"
#include "log.h"
#include "compat.h"
#include "net.h"
#include "cron.h"
#include <lwip/sockets.h>
#include <esp_http_client.h>
#include "esp_heap_caps.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>

/* ---- State ---- */

static std::string gwHost;             // gateway HTTP host
static int gwPort = 0;                 // gateway HTTP port
static std::string gwControlPath;      // SOAP endpoint path
static std::string gwServiceType;      // WANIPConnection or WANPPPConnection
static std::string extIp;             // external IP from gateway
static std::string prevExtIp;         // for change detection
static char localIp[16] = {};
static bool discovered = false;
static volatile bool syncBusy = false;

struct fwd_t { int extPort; int intPort; char proto[4]; };
static std::vector<fwd_t> activeForwards;
static char prevLocalIp[16] = {};

/* ---- URL parsing ---- */

static bool parseUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    auto start = url.find("://");
    if (start == std::string::npos) return false;
    start += 3;
    auto pathPos = url.find('/', start);
    if (pathPos == std::string::npos) {
        path = "/";
        host = url.substr(start);
    } else {
        host = url.substr(start, pathPos - start);
        path = url.substr(pathPos);
    }
    auto colonPos = host.find(':');
    if (colonPos != std::string::npos) {
        port = atoi(host.c_str() + colonPos + 1);
        host = host.substr(0, colonPos);
    } else {
        port = 80;
    }
    return true;
}

/* ---- HTTP helpers ---- */

struct http_ctx_t { std::string body; };

static esp_err_t httpEvent(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        auto* ctx = (http_ctx_t*)evt->user_data;
        if (ctx) ctx->body.append((const char*)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static std::string httpGet(const std::string& url) {
    http_ctx_t ctx;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = httpEvent;
    config.user_data = &ctx;
    config.timeout_ms = 5000;
    auto client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK ? ctx.body : "";
}

static std::string httpSoap(const std::string& action, const std::string& body) {
    std::string envelope =
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
        "<s:Body><u:" + action +
        " xmlns:u=\"urn:schemas-upnp-org:service:" + gwServiceType + ":1\">\r\n" +
        body +
        "</u:" + action + "></s:Body></s:Envelope>\r\n";

    std::string soapAction = "\"urn:schemas-upnp-org:service:" + gwServiceType +
                              ":1#" + action + "\"";

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s", gwHost.c_str(), gwPort, gwControlPath.c_str());

    http_ctx_t ctx;
    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = httpEvent;
    config.user_data = &ctx;
    config.timeout_ms = 5000;
    config.method = HTTP_METHOD_POST;
    auto client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
    esp_http_client_set_header(client, "SOAPAction", soapAction.c_str());
    esp_http_client_set_post_field(client, envelope.c_str(), (int)envelope.size());
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return "";
    dbg("SOAP %s → %d\n", action.c_str(), status);
    return ctx.body;
}

/* ---- XML helpers ---- */

static std::string xmlTag(const std::string& xml, const char* tag) {
    std::string open = std::string("<") + tag + ">";
    std::string close = std::string("</") + tag + ">";
    auto start = xml.find(open);
    if (start == std::string::npos) return "";
    start += open.size();
    auto end = xml.find(close, start);
    if (end == std::string::npos) return "";
    return xml.substr(start, end - start);
}

/* ---- SSDP discovery ---- */

/* Case-insensitive find in string */
static size_t findCI(const std::string& s, const char* needle) {
    size_t nLen = strlen(needle);
    for (size_t i = 0; i + nLen <= s.size(); i++) {
        bool match = true;
        for (size_t j = 0; j < nLen; j++) {
            if (tolower((unsigned char)s[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return std::string::npos;
}

static bool ssdpDiscover() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { err("SSDP: socket failed\n"); return false; }

    struct timeval tv = {4, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in mcast = {};
    mcast.sin_family = AF_INET;
    mcast.sin_port = htons(1900);
    inet_aton("239.255.255.250", &mcast.sin_addr);

    const char* msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
        "\r\n";

    /* Send M-SEARCH twice (some routers miss the first) */
    for (int attempt = 0; attempt < 2; attempt++) {
        int sent = sendto(fd, msearch, strlen(msearch), 0,
                          (struct sockaddr*)&mcast, sizeof(mcast));
        if (sent < 0) {
            err("SSDP: sendto failed: %s (errno %d)\n", strerror(errno), errno);
            close(fd);
            return false;
        }
        dbg("SSDP: M-SEARCH sent (%d bytes)\n", sent);

        char buf[1024];
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            dbg("SSDP: response (%d bytes)\n", n);

            /* Extract LOCATION header (case-insensitive) */
            std::string resp(buf, n);
            auto locPos = findCI(resp, "location:");
            if (locPos == std::string::npos) {
                info("SSDP: response has no LOCATION header\n");
                continue;
            }
            auto valStart = locPos + 9;  // strlen("location:")
            while (valStart < resp.size() && resp[valStart] == ' ') valStart++;
            auto valEnd = resp.find("\r\n", valStart);
            if (valEnd == std::string::npos) valEnd = resp.size();
            std::string location = resp.substr(valStart, valEnd - valStart);
            info("SSDP: %s\n", location.c_str());

            close(fd);

            /* Parse gateway URL */
            std::string descPath;
            if (!parseUrl(location, gwHost, gwPort, descPath)) {
                err("SSDP: bad LOCATION URL\n");
                return false;
            }

            /* Fetch root device XML */
            std::string xml = httpGet(location);
            if (xml.empty()) { err("SSDP: failed to fetch device XML\n"); return false; }
            dbg("SSDP: XML %d bytes\n", (int)xml.size());

            /* Find WANIPConnection or WANPPPConnection service */
            auto svcPos = xml.find("WANIPConnection");
            gwServiceType = "WANIPConnection";
            if (svcPos == std::string::npos) {
                svcPos = xml.find("WANPPPConnection");
                gwServiceType = "WANPPPConnection";
            }
            if (svcPos == std::string::npos) {
                err("SSDP: no WANIPConnection service in XML\n");
                return false;
            }

            /* Extract controlURL from within the same <service> block */
            auto endSvc = xml.find("</service>", svcPos);
            auto ctrlStart = xml.find("<controlURL>", svcPos);
            if (ctrlStart == std::string::npos ||
                (endSvc != std::string::npos && ctrlStart > endSvc)) {
                err("SSDP: no controlURL\n");
                return false;
            }
            ctrlStart += 12;  // strlen("<controlURL>")
            auto ctrlEnd = xml.find("</controlURL>", ctrlStart);
            if (ctrlEnd == std::string::npos) return false;
            gwControlPath = xml.substr(ctrlStart, ctrlEnd - ctrlStart);

            /* Resolve controlURL */
            if (gwControlPath.find("http") == 0) {
                std::string dummy;
                int p;
                parseUrl(gwControlPath, dummy, p, gwControlPath);
            } else if (gwControlPath.empty() || gwControlPath[0] != '/') {
                gwControlPath = "/" + gwControlPath;
            }

            info("gateway: %s:%d %s ctrl=%s\n", gwHost.c_str(), gwPort,
                 gwServiceType.c_str(), gwControlPath.c_str());
            return true;
        }
        dbg("SSDP: no response (attempt %d)\n", attempt + 1);
    }

    close(fd);
    info("SSDP: no gateway found\n");
    return false;
}

/* ---- UPnP operations ---- */

static bool getExternalIp() {
    auto resp = httpSoap("GetExternalIPAddress", "");
    if (resp.empty()) return false;
    auto ip = xmlTag(resp, "NewExternalIPAddress");
    if (ip.empty()) return false;
    extIp = ip;
    return true;
}

static bool addPortMapping(int extPort, int intPort, const char* proto,
                           const char* lip, const char* desc) {
    char args[512];
    snprintf(args, sizeof(args),
        "<NewRemoteHost></NewRemoteHost>\r\n"
        "<NewExternalPort>%d</NewExternalPort>\r\n"
        "<NewProtocol>%s</NewProtocol>\r\n"
        "<NewInternalPort>%d</NewInternalPort>\r\n"
        "<NewInternalClient>%s</NewInternalClient>\r\n"
        "<NewEnabled>1</NewEnabled>\r\n"
        "<NewPortMappingDescription>%s</NewPortMappingDescription>\r\n"
        "<NewLeaseDuration>3600</NewLeaseDuration>\r\n",
        extPort, proto, intPort, lip, desc);
    auto resp = httpSoap("AddPortMapping", args);
    if (resp.find("errorCode") != std::string::npos) {
        /* Some routers reject non-zero lease — retry with 0 */
        snprintf(args, sizeof(args),
            "<NewRemoteHost></NewRemoteHost>\r\n"
            "<NewExternalPort>%d</NewExternalPort>\r\n"
            "<NewProtocol>%s</NewProtocol>\r\n"
            "<NewInternalPort>%d</NewInternalPort>\r\n"
            "<NewInternalClient>%s</NewInternalClient>\r\n"
            "<NewEnabled>1</NewEnabled>\r\n"
            "<NewPortMappingDescription>%s</NewPortMappingDescription>\r\n"
            "<NewLeaseDuration>0</NewLeaseDuration>\r\n",
            extPort, proto, intPort, lip, desc);
        resp = httpSoap("AddPortMapping", args);
    }
    bool ok = !resp.empty() && resp.find("errorCode") == std::string::npos;
    if (ok)
        info("forwarded %s %d → %s:%d\n", proto, extPort, lip, intPort);
    else
        err("forward %s %d → %d failed\n", proto, extPort, intPort);
    return ok;
}

static void deletePortMapping(int extPort, const char* proto) {
    char args[256];
    snprintf(args, sizeof(args),
        "<NewRemoteHost></NewRemoteHost>\r\n"
        "<NewExternalPort>%d</NewExternalPort>\r\n"
        "<NewProtocol>%s</NewProtocol>\r\n",
        extPort, proto);
    httpSoap("DeletePortMapping", args);
    dbg("deleted forward %s %d\n", proto, extPort);
}

static void trackForward(int extPort, int intPort, const char* proto) {
    for (auto& f : activeForwards) {
        if (f.extPort == extPort && strcmp(f.proto, proto) == 0) {
            f.intPort = intPort;
            return;
        }
    }
    fwd_t entry = {extPort, intPort, {}};
    safeStrncpy(entry.proto, proto, sizeof(entry.proto));
    activeForwards.push_back(entry);
}

/* ---- Sync task ---- */

static void upnpSyncTask(void*) {
    syncBusy = true;
    netActivity();

    if (!discovered) {
        if (!ssdpDiscover()) { syncBusy = false; killSelf(); return; }
        discovered = true;
    }

    netGetLocalIp(localIp, sizeof(localIp));
    if (!localIp[0]) { syncBusy = false; killSelf(); return; }

    /* Detect IP change */
    bool ipChanged = strcmp(localIp, prevLocalIp) != 0;
    if (ipChanged) {
        info("local IP: %s\n", localIp);
        safeStrncpy(prevLocalIp, localIp, sizeof(prevLocalIp));
    }

    getExternalIp();
    if (!extIp.empty()) {
        bool extChanged = extIp != prevExtIp;
        if (extChanged) {
            info("external IP: %s\n", extIp.c_str());
            prevExtIp = extIp;
        }
    }

    /* UPnP description tag: hostname for the TCP mapping; "<hostname>-webrtc"
     * for the UDP one (so routers' admin UI lists them clearly). */
    char hostname[32];
    storageGetStr("s.net.hostname", hostname, sizeof(hostname), "");
    char webrtcDesc[40];
    snprintf(webrtcDesc, sizeof(webrtcDesc), "%s-webrtc", hostname);

    /* Forward HTTPS (TCP) */
    int httpsPort = storageGetInt("s.net.https_port", 443);
    if (httpsPort > 0) {
        int extPort = storageGetInt("s.upnp.ext_port", 0);
        if (extPort <= 0) extPort = httpsPort;
        addPortMapping(extPort, httpsPort, "TCP", localIp, hostname);
        trackForward(extPort, httpsPort, "TCP");
    }

    /* Forward WebRTC DataChannel (UDP) */
    int webrtcPort = storageGetInt("s.net.webrtc_port", 0);
    if (webrtcPort > 0) {
        addPortMapping(webrtcPort, webrtcPort, "UDP", localIp, webrtcDesc);
        trackForward(webrtcPort, webrtcPort, "UDP");
    }

    /* DuckDNS updates are driven by cron — don't piggy-back on UPnP renewal. */

    syncBusy = false;
    killSelf();
}

/* ---- Public API ---- */

static void upnpUpdate() {
    if (!storageGetInt("s.upnp.enable") || syncBusy) return;
    spawnTask(upnpSyncTask, "upnp", 6144, nullptr, 1, 0);
}

static void upnpStart(const char*) {
    upnpUpdate();
}

static void upnpStop(const char*) {
    if (!activeForwards.empty() && discovered) {
        /* Delete mappings synchronously (network going down soon) */
        for (auto& f : activeForwards)
            deletePortMapping(f.extPort, f.proto);
        activeForwards.clear();
    }
    discovered = false;
    extIp.clear();
    prevExtIp.clear();
}

const char* upnpExternalIp() {
    return extIp.c_str();
}

static void upnpStatus(cli_write_fn write) {
    char buf[128];
    int n;
    if (!storageGetInt("s.upnp.enable")) {
        n = snprintf(buf, sizeof(buf), "upnp: disabled\n");
        write(buf, (size_t)n);
        return;
    }
    n = snprintf(buf, sizeof(buf), "upnp: %s\n", discovered ? "discovered" : "searching");
    write(buf, (size_t)n);
    if (discovered) {
        n = snprintf(buf, sizeof(buf), "gateway: %s:%d\n", gwHost.c_str(), gwPort);
        write(buf, (size_t)n);
    }
    if (!extIp.empty()) {
        n = snprintf(buf, sizeof(buf), "external IP: %s\n", extIp.c_str());
        write(buf, (size_t)n);
    }
    for (auto& f : activeForwards) {
        n = snprintf(buf, sizeof(buf), "forward: %s %d → %s:%d\n",
                     f.proto, f.extPort, localIp, f.intPort);
        write(buf, (size_t)n);
    }
}

/* Module config version. Bump when adding/changing defaults. See duckdns.cpp. */
#define UPNP_VERSION 1

void UpnpService::onInit() {
    int v = storageGetInt("s.upnp.version", 0);
    if (v < UPNP_VERSION) {
        /* s.upnp.{enable,ext_port} defaults are seeded by the generated
         * spangapSettingsGenDefaults() from this straddle's `settings:` block. */
        cronDefault("*/15 * * * * N", "upnp update");
        storageSet("s.upnp.version", UPNP_VERSION);
    }

    netRegister(NET_EV_UPSTREAM_UP,   upnpStart);
    netRegister(NET_EV_UPSTREAM_DOWN, upnpStop);
    static auto w = [](const char* d, size_t l) { cliPrintf("%.*s", (int)l, d); };
    cliRegisterCmd("upnp update", [](const char* a) {
        if (cliWantsHelp(a)) { cliPrintf("%-*s renew port mappings + refresh external IP\n", CLI_HELP_COL, "upnp update"); return; }
        upnpUpdate();
    });
    cliRegisterCmd("upnp", [](const char* a) {
        if (cliWantsHelp(a)) { cliPrintf("%-*s UPnP port forwarding status\n", CLI_HELP_COL, "upnp"); return; }
        upnpStatus(w);
    });
}
