import { useMenuStore } from 'spangap-browser/stores/menu'
import UpnpPanel from '../panels/UpnpPanel.vue'

export function registerUpnp() {
  useMenuStore().register('settings/network/upnp', 'UPnP', { type: 'panel', component: UpnpPanel })
}
