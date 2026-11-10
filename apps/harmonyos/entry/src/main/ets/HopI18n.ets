import { i18n } from '@kit.LocalizationKit';

export function ui(english: string, chinese: string): string {
  try { return i18n.System.getSystemLanguage().toLowerCase().startsWith('zh') ? chinese : english; }
  catch (_) { return english; }
}

export function stateLabel(state: string): string {
  switch (state) {
    case 'disconnected': return ui('Disconnected', '未连接');
    case 'connecting': return ui('Connecting', '正在连接');
    case 'handshake': return ui('Authenticating', '正在验证连接');
    case 'pairing': return ui('Pairing', '正在配对');
    case 'waiting': return ui('Waiting for confirmation', '等待确认');
    case 'saving': return ui('Saving pairing', '正在保存配对');
    case 'local': return ui('Connected', '已连接');
    case 'remote': return ui('Receiving input', '正在接收键鼠');
    case 'leaving': return ui('Returning to Mac', '正在返回 Mac');
    default: return state;
  }
}

export function messageLabel(value: string): string {
  switch (value) {
    case 'Choose a Mac on your trusted local network.': return ui(value, '请选择可信局域网中的 Mac。');
    case 'Connected · input remains on Mac': return ui(value, '已连接 · 当前控制 Mac');
    case 'Receiving Mac keyboard and mouse': return ui(value, '正在接收 Mac 的键盘和鼠标');
    case 'Input returned to Mac': return ui(value, '已返回 Mac');
    case 'Return edge timed out; connection kept': return ui(value, '返回确认超时，连接仍保持');
    case 'Disconnected': return ui(value, '已断开连接');
    case 'Mac disconnected': return ui(value, 'Mac 已断开连接');
    case 'Mac connection closed': return ui(value, 'Mac 连接已关闭');
    case 'Mac unpaired': return ui(value, 'Mac 已解除配对');
    case 'Mac unpaired this device': return ui(value, 'Mac 已解除与本设备的配对');
    case 'Pairing removed': return ui(value, '已解除配对');
    case 'Compare this code on both devices': return ui(value, '请在两台设备核对配对码');
    case 'Both confirmed; storing binding': return ui(value, '双方已确认，正在保存配对');
    case 'Waiting for Mac confirmation': return ui(value, '等待 Mac 确认');
    case 'Pairing could not be saved. Reconnect and pair again.': return ui(value, '无法保存配对，请重新连接并配对。');
    case 'Approve the system input injection dialog, then choose Connect.': return ui(value, '请在系统弹窗中允许输入控制，然后连接。');
    case 'Grant system injection permission first.': return ui(value, '请先允许系统输入控制。');
    case 'Background task/notification unavailable. Relaunch Hop to retry.': return ui(value, '后台任务或通知不可用，请重启 Hop 后重试。');
    case 'Unpair the current Mac before pairing another.': return ui(value, '请先解除当前 Mac 的配对。');
    case 'Enter a Mac LAN IP address.': return ui(value, '请输入 Mac 的局域网 IP 地址。');
    case 'Searching local network…': return ui(value, '正在搜索局域网…');
    case 'mDNS unavailable. Use manual IP.': return ui(value, '自动发现不可用，请手动输入 IP 地址。');
    case 'Select a Mac; discovery alone never starts a new pairing.': return ui(value, '请选择一台 Mac；发现设备不会自动配对。');
    case 'A Mac could not be resolved. Use manual IP.': return ui(value, '无法解析 Mac 地址，请手动输入 IP。');
    case 'Unable to connect to Mac': return ui(value, '无法连接 Mac');
    case 'Injection permission must be granted before connection': return ui(value, '连接前请先授权输入控制');
    case 'Injection authorization revoked': return ui(value, '输入控制权限已撤销');
    case 'Heartbeat timeout': return ui(value, '连接超时');
    default: return value;
  }
}
