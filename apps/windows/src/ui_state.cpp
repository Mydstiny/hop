#include "hop_windows/ui_state.h"
namespace hop::windows {
const UiStrings& ui_strings(UiLanguage language) {
    static const UiStrings en{
        L"Hop", L"Not connected", L"Connected · sharing paused", L"Connected · sharing enabled",
        L"Show status", L"Enable sharing", L"Pause sharing", L"Device layout",
        L"No connected devices", L"Connected computers and their displays will appear here after pairing is implemented.",
        L"Drag to arrange · Double-click to expand displays · Right-click for settings",
        L"Invalid layout: displays overlap or exceed the allowed area.", L"Layout could not be loaded or saved; existing data was retained.", L"Mouse sensitivity (software)", L"Maximum motion rate",
        L"Horizontal scroll speed", L"Vertical scroll speed", L"Reverse horizontal scroll", L"Reverse vertical scroll",
        L"Settings require an online, verified device.", L"Quit",
        L"Connection and pairing are not implemented in this development build."
    };
    static const UiStrings zh{
        L"Hop", L"未连接", L"已连接 · 共享已暂停", L"已连接 · 共享已启用",
        L"显示状态", L"启用共享", L"暂停共享", L"设备布局",
        L"暂无已连接设备", L"完成设备配对后，可在此查看电脑与显示器布局。当前开发版本尚未接入设备连接。",
        L"拖动排列 · 双击展开显示器 · 右键调整设置", L"布局无效：显示器重叠或超出允许范围。", L"无法读取或保存布局，原有数据已保留。", L"鼠标灵敏度（软件倍率）", L"鼠标移动最高转发频率",
        L"横向滚轮速度", L"纵向滚轮速度", L"反转横向滚轮", L"反转纵向滚轮", L"需要在线且已验证的设备才能调整设置。", L"退出",
        L"此开发版本尚未实现设备连接与配对。"
    };
    return language == UiLanguage::chinese ? zh : en;
}
} // namespace hop::windows
