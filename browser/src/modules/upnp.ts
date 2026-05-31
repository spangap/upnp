import { useMenuStore } from 'spangap-browser/stores/menu'
import UpnpPanel from '../panels/UpnpPanel.vue'

export function registerUpnp() {
  useMenuStore().register('settings', 'Settings', [
    { id: 'network', label: 'Network', type: 'submenu',
      children: [
        { id: 'network.upnp', label: 'UPnP', type: 'panel',
          component: UpnpPanel },
      ],
    },
  ])
}
