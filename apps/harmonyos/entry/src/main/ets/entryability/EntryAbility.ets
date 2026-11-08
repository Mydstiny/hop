import { UIAbility } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { image } from '@kit.ImageKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { wantAgent } from '@kit.AbilityKit';
import { notificationManager } from '@kit.NotificationKit';
import { statusBarManager } from '@kit.DeskTopExtensionKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { HopService } from '../HopService';

export default class EntryAbility extends UIAbility {
  private icons?: statusBarManager.StatusBarIcon;
  private starting: boolean = false;
  onWindowStageCreate(stage: window.WindowStage): void {
    // Deliberately do not load any content into the UIAbility main window.
    // Only the system-owned statusbar popup loads an ArkUI page.
    // API14+ public Stage API; configured by matching ability metadata.
    // There is no public Window.hide() in the audited SDK.
    try { stage.removeStartingWindow().catch(() => {}); } catch (_) {}
    this.start().catch((error: Error) => {
      HopService.instance.view.message = `Cannot start tray service: ${String(error)}`;
    });
  }
  onForeground(): void {
    // Notification/launcher activations must not turn this tray utility into a window.
    if (!this.starting) { this.context.moveAbilityToBackground().catch(() => {}); }
  }
  private async icon(light: boolean): Promise<image.PixelMap> {
    const pixels = new Uint8Array(24 * 24 * 4);
    for (let y = 4; y < 20; y++) {
      for (let x = 4; x < 20; x++) {
        if (x < 7 || x > 16 || (y > 10 && y < 14)) {
          const i = (y * 24 + x) * 4;
          pixels[i] = pixels[i + 1] = pixels[i + 2] = light ? 255 : 0;
          pixels[i + 3] = 255;
        }
      }
    }
    return image.createPixelMap(pixels.buffer, { size: { width: 24, height: 24 }, pixelFormat: image.PixelMapFormat.RGBA_8888 });
  }
  private async start(): Promise<void> {
    if (this.starting) { return; }
    this.starting = true;
    const service = HopService.instance;
    try {
      if (!canIUse('SystemCapability.PCService.StatusBarManager')) { throw new Error('PC statusbar is unavailable'); }
      service.initialize(this.context, true);
      this.icons = { white: await this.icon(true), black: await this.icon(false) };
      statusBarManager.addToStatusBar(this.context, {
        icons: this.icons,
        quickOperation: { title: 'Hop', height: 620, abilityName: 'HopStatusBarAbility', moduleName: 'entry' },
        statusBarGroupMenu: [[
          { title: 'Disconnect', menuAction: { abilityName: 'EntryAbility', notifyOnly: true, menuCode: 'disconnect' } },
          { title: 'Quit Hop', menuAction: { abilityName: 'EntryAbility', notifyOnly: true, menuCode: 'quit' } }
        ]]
      });
      statusBarManager.on('rightMenuClick', (event) => {
        if (event.data?.menuCode === 'disconnect') { service.disconnect(); }
        if (event.data?.menuCode === 'quit') {
          this.quit().catch((error: Error) => { service.view.message = `Cannot quit cleanly: ${String(error)}`; });
        }
      });
      await notificationManager.requestEnableNotification(this.context);
      const agent = await wantAgent.getWantAgent({
        wants: [{ bundleName: 'dev.hop.input', abilityName: 'EntryAbility' }],
        operationType: wantAgent.OperationType.START_ABILITY,
        requestCode: 1,
        wantAgentFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      });
      // TASK_KEEPING is the legitimate PC/2-in-1 mode; not fake audio/data transfer.
      // The platform continuous task supplies its required persistent notification.
      await backgroundTaskManager.startBackgroundRunning(this.context, backgroundTaskManager.BackgroundMode.TASK_KEEPING, agent);
      service.view.background = true;
      service.browse();
    } catch (error) {
      service.view.background = false;
      service.view.message = 'Background task/notification unavailable. Relaunch Hop and allow notifications.';
      hilog.error(0x0000, 'Hop', 'Startup failed: %{public}s', String(error));
      service.disconnect();
    } finally {
      // Never display a main-window fallback, including errors.
      await this.context.moveAbilityToBackground();
      this.starting = false;
    }
  }
  private async quit(): Promise<void> {
    HopService.instance.stop();
    try { await backgroundTaskManager.stopBackgroundRunning(this.context); } catch (_) {}
    try { statusBarManager.removeFromStatusBar(this.context); } catch (_) {}
    await this.context.terminateSelf();
  }
  onDestroy(): void {
    HopService.instance.stop();
    backgroundTaskManager.stopBackgroundRunning(this.context).catch(() => {});
    this.icons?.white.release().catch(() => {});
    this.icons?.black.release().catch(() => {});
  }
}
