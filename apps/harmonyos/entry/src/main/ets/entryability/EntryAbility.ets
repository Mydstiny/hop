import { UIAbility } from '@kit.AbilityKit';
import { window, display } from '@kit.ArkUI';
import { image } from '@kit.ImageKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { wantAgent } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { statusBarManager } from '@kit.DeskTopExtensionKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { HopService } from '../HopService';

export default class EntryAbility extends UIAbility {
  private icons?: statusBarManager.StatusBarIcon;
  private starting: boolean = false;
  private hiding: boolean = false;
  private windowReady: boolean = false;
  private trayReady: boolean = false;
  onWindowStageCreate(stage: window.WindowStage): void {
    this.windowReady = true;
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
    // A user relaunch can retry a failed continuous task without adding a second icon.
    // Do not background the ability while the initial asynchronous startup is in flight.
    if (!this.windowReady || this.starting) { return; }
    if (!HopService.instance.view.background) {
      this.start().catch((error: Error) => { HopService.instance.view.message = String(error); });
    } else {
      this.retreat().catch(() => {});
    }
  }
  // PC/2-in-1 rejects moveAbilityToBackground (16000061). hideAbility is the
  // supported call, and only after the status-bar icon exists.
  private async retreat(): Promise<void> {
    if (this.hiding || !this.trayReady) { return; }
    this.hiding = true;
    try {
      await this.context.hideAbility();
      hilog.info(0x0000, 'Hop', 'Main ability hidden after statusbar registration');
    } catch (error) {
      hilog.warn(0x0000, 'Hop', 'Hide ability unavailable: %{public}s', String(error));
    } finally {
      this.hiding = false;
    }
  }
  private async icon(light: boolean): Promise<image.PixelMap> {
    // SDK recommendation is 24 vp, not 24 raw pixels. Convert using the display density.
    const density = display.getDefaultDisplaySync().densityPixels;
    if (!Number.isFinite(density) || density <= 0 || density > 8) { throw new Error('Invalid icon display density'); }
    const side = Math.max(1, Math.round(24 * density));
    const pixels = new Uint8Array(side * side * 4);
    for (let y = 0; y < side; y++) {
      for (let x = 0; x < side; x++) {
        const vx = (x + 0.5) / density;
        const vy = (y + 0.5) / density;
        if (vx >= 4 && vx < 20 && vy >= 4 && vy < 20 && (vx < 7 || vx > 16 || (vy > 10 && vy < 14))) {
          const i = (y * side + x) * 4;
          pixels[i] = pixels[i + 1] = pixels[i + 2] = light ? 255 : 0;
          pixels[i + 3] = 255;
        }
      }
    }
    hilog.info(0x0000, 'Hop', 'Icon size %{public}dpx, display density %{public}f', side, density);
    return image.createPixelMap(pixels.buffer, { size: { width: side, height: side }, pixelFormat: image.PixelMapFormat.RGBA_8888 });
  }
  private async start(): Promise<void> {
    if (this.starting) { return; }
    this.starting = true;
    const service = HopService.instance;
    let phase: string = 'initialize';
    let taskStarted: boolean = false;
    try {
      if (!canIUse('SystemCapability.PCService.StatusBarManager')) { throw new Error('PC statusbar is unavailable'); }
      service.initialize(this.context, true);
      if (!this.trayReady) {
        phase = 'icon creation';
        this.icons = { white: await this.icon(true), black: await this.icon(false) };
        const icons = this.icons;
        phase = 'tray registration';
        await new Promise<void>((resolve, reject) => {
          statusBarManager.addToStatusBar(this.context, {
            icons: icons,
            quickOperation: { title: 'Hop', height: 420, abilityName: 'HopStatusBarAbility', moduleName: 'entry' },
            statusBarGroupMenu: [[
              { title: 'Disconnect', menuAction: { abilityName: 'EntryAbility', notifyOnly: true, menuCode: 'disconnect' } },
              { title: 'Quit Hop', menuAction: { abilityName: 'EntryAbility', notifyOnly: true, menuCode: 'quit' } }
            ]]
          }, (error) => {
            if (error && error.code !== 0) { reject(error); } else { resolve(); }
          });
        });
        this.trayReady = true;
        hilog.info(0x0000, 'Hop', 'Statusbar registration callback succeeded');
        statusBarManager.on('rightMenuClick', (event) => {
          if (event.data?.menuCode === 'disconnect') { service.disconnect(); }
          if (event.data?.menuCode === 'quit') {
            this.quit().catch((error: Error) => { service.view.message = `Cannot quit cleanly: ${String(error)}`; });
          }
        });
      }
      phase = 'notification agent';
      const agent = await wantAgent.getWantAgent({
        wants: [{ bundleName: 'dev.hop.input', abilityName: 'EntryAbility' }],
        operationType: wantAgent.OperationType.START_ABILITY,
        requestCode: 1,
        wantAgentFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      });
      // TASK_KEEPING supplies its own required system notification. Calling
      // requestEnableNotification here is invalid: this ability has no UIContent.
      // Do not fake a window or bypass notification verification; task API failures block connections.
      phase = 'continuous task';
      await backgroundTaskManager.startBackgroundRunning(this.context, backgroundTaskManager.BackgroundMode.TASK_KEEPING, agent);
      taskStarted = true;
      phase = 'continuous task verification';
      const tasks = await backgroundTaskManager.getAllContinuousTasks(this.context);
      const ownTasks = tasks.filter((task: backgroundTaskManager.ContinuousTaskInfo) =>
        task.abilityName === 'EntryAbility' && task.wantAgentBundleName === 'dev.hop.input' && !task.suspendState);
      if (ownTasks.length === 0) { throw new Error('No active own continuous task after start'); }
      const task = ownTasks[0];
      hilog.info(0x0000, 'Hop', 'TASK_KEEPING active: task %{public}d, notification %{public}d, modes %{public}s',
        task.continuousTaskId, task.notificationId, task.backgroundModes.join(','));
      service.view.background = true;
      service.browse();
    } catch (error) {
      if (taskStarted) {
        try { await backgroundTaskManager.stopBackgroundRunning(this.context); } catch (_) {}
      }
      service.view.background = false;
      service.disconnect();
      const problem = error as BusinessError;
      service.view.message = `Startup failed at ${phase}: ${problem.code || 0} ${problem.message || String(error)}`;
      hilog.error(0x0000, 'Hop', '%{public}s', service.view.message);
    } finally {
      // Never display a main-window fallback, including errors.
      // Hide only after the status-bar icon exists; a failure must not stick the startup lock.
      this.starting = false;
      await this.retreat();
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
