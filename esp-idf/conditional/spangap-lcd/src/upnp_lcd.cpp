/**
 * upnp_lcd.cpp — on-device Settings → Net → UPnP pane (LVGL).
 *
 * Mirrors the browser UpnpPanel: an enable switch and an external-port field.
 * This whole file lives under conditional/spangap-lcd/, compiled only when the
 * lcd straddle is staged, so no #if guard is needed.
 */
#include "lcd.h"

/* On-device Settings → Net → UPnP pane. Mirrors the browser UpnpPanel. */
static void upnpSettingsPane(void* arg) {
    lv_obj_t* p = static_cast<lv_obj_t*>(arg);
    lcdSettingSection(p, "UPnP");
    lcdSettingSwitch (p, "Enable",        "s.upnp.enable");
    lcdSettingText   (p, "External port", "s.upnp.ext_port");   /* 0 = use HTTPS port */
}

/* Register the on-device UPnP settings pane — a when:-gated init: hook
 * (spangap/spangap-lcd). Plain C++ linkage to match the generated dispatcher's
 * forward decl. */
void upnpLcdRegister(void) {
    lcdRegisterSettings("Net/UPnP", "UPnP", upnpSettingsPane);
}
