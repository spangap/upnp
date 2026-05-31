<template>
  <div class="q-gutter-y-md">
    <SettingToggle label="Enable UPnP IGD" k="s.upnp.enable" />

    <q-separator dark />

    <div class="q-gutter-y-sm">
      <SettingText label="External HTTPS port" k="s.upnp.ext_port" />
      <div class="text-caption text-grey-5" style="line-height: 1.35">
        0 uses the same port as the device HTTPS listener (<code>s.net.https_port</code>).
        The device requests TCP mappings for HTTPS and UDP for WebRTC (<code>s.net.webrtc_port</code>).
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
</script>
