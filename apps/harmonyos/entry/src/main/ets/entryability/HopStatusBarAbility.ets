import { Want, UIExtensionContentSession } from '@kit.AbilityKit';
import { StatusBarViewExtensionAbility } from '@kit.DeskTopExtensionKit';
import { HopService } from '../HopService';

export default class HopStatusBarAbility extends StatusBarViewExtensionAbility {
  onSessionCreate(_want: Want, session: UIExtensionContentSession): void {
    try {
      HopService.instance.initialize(this.context, false);
      HopService.instance.browse();
    } catch (error) {
      HopService.instance.view.message = `Cannot initialize Hop settings: ${String(error)}`;
    }
    session.loadContent('pages/Index');
  }
}
