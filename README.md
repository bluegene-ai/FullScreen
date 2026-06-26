# FullScreen Browser

全屏独占网页浏览器，双击启动后全屏显示配置的网页，具备防关闭保护和密码验证。

## 功能

- **全屏独占** — 覆盖整个桌面（含任务栏），窗口始终置顶，鼠标自动隐藏
- **防关闭保护** — 拦截 Alt+F4、Ctrl+W、Win 键、Alt+Tab、Alt+Space、Ctrl+Esc
- **密码保护** — 关闭/修改设置需要密码验证，连续 5 次错误自动锁定
- **WebView2 渲染** — 基于 Edge Chromium 内核，支持现代网页标准
- **URL 可达性监测** — 目标页面不可达时显示自定义提示，恢复后自动重新加载
- **单文件部署** — 编译产物为单个 `.exe`，无需额外 DLL

## 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+Shift+F12` | 弹出密码验证 → 退出或修改设置 |

## 使用说明

1. **首次运行** — 弹出设置对话框，配置：
   - 目标网页 URL（需以 `http://` 或 `https://` 开头）
   - 操作密码（用于退出/修改设置）
   - 不可达提示信息（网页无法加载时显示）
   - 页面缩放比例（50% - 300%）
2. **后续运行** — 直接全屏加载目标网页
3. **退出/修改设置** — 按 `Ctrl+Shift+F12`，输入密码后选择操作

## 编译

### 环境要求

- Visual Studio 2022（Community 或以上）
- WebView2 SDK（NuGet 包）

### 步骤

```batch
# 1. 安装 WebView2 SDK
nuget install Microsoft.Web.WebView2 -OutputDirectory packages

# 2. 在 x64 Native Tools 命令提示符中编译
build.bat
```

编译产物：`build\FullScreenBrowser.exe`

## 配置文件

配置文件存储在 `%APPDATA%\FullScreenBrowser\config.dat`，密码以 XOR+Base64 混淆存储。

删除配置文件可恢复为首次运行状态。

## 技术栈

- C++17 / Win32 API
- WebView2（Edge Chromium）
- MSVC 静态 CRT 链接
