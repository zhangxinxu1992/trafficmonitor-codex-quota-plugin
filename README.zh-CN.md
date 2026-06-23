# TrafficMonitor Agent Quota Plugins

[English](README.md) | 简体中文

这是用于 TrafficMonitor 的 x64 插件，可以在 TrafficMonitor 任务栏窗口中显示 Codex 和 GitHub Copilot 的额度信息。

这是一个非官方项目，与 OpenAI、GitHub 或 TrafficMonitor 项目没有从属关系。

## 插件

| 插件 DLL | 显示内容 | 准备工作 |
| --- | --- | --- |
| `TrafficMonitorCodexQuota.dll` | `CX 5h:` 和 `CX 7d:` 两个 Codex 额度窗口 | 先使用 Codex CLI 完成登录。 |
| `TrafficMonitorGitHubCopilotQuota.dll` | `GC:` GitHub Copilot 额度 | 打开插件选项，点击 `Sign in with GitHub` 登录。 |

默认显示剩余额度和紧凑的重置倒计时。插件选项可以切换为已用额度、隐藏重置信息，或把重置信息从倒计时切换为本地重置时间。GitHub Copilot 插件还可以选择是否显示剩余 credits。启用 TrafficMonitor 的资源占用图后，插件会提供已用额度百分比作为图形数值，因此剩余额度很低时柱状图会接近填满。

任务栏显示示例：

```text
CX 5h: 69% 42m
CX 7d: 89% 6d 1h
GC: 82% 1.2kcr 12d
```

## 安装

从 [GitHub Releases](https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins/releases) 下载最新版本。

1. 下载 `trafficmonitor-agent-quota-plugins-v<version>.zip`。
2. 解压 ZIP 文件。
3. 将需要使用的 DLL 文件复制到 `TrafficMonitor.exe` 旁边的 `plugins` 目录，例如：

   ```text
   TrafficMonitor
   |-- TrafficMonitor.exe
   `-- plugins
       |-- TrafficMonitorCodexQuota.dll
       `-- TrafficMonitorGitHubCopilotQuota.dll
   ```

4. 重启 TrafficMonitor。

TrafficMonitor 官方插件文档也采用同样的安装方式：使用与 TrafficMonitor 架构匹配的插件 DLL，将 DLL 放入 TrafficMonitor 程序目录下的 `plugins` 文件夹，重启 TrafficMonitor，然后在插件管理和任务栏显示设置中启用需要的显示项。本项目发布的是 x64 插件 DLL。更多细节可以查看官方的 [TrafficMonitor 插件功能说明](https://github.com/zhongyang219/TrafficMonitor/wiki/%E6%8F%92%E4%BB%B6%E5%8A%9F%E8%83%BD) 或 [TrafficMonitor 官网](https://trafficmonitor.org/)。

## 启用显示项

重启 TrafficMonitor 后：

1. 打开 TrafficMonitor 的插件管理，确认 DLL 已加载。
2. 打开任务栏窗口的显示设置。
3. 启用需要的显示项：
   - `CodexQuota5h`
   - `CodexQuotaWeek`
   - `GitHubCopilotQuotaAI`

可以只安装其中一个 DLL，也可以两个都安装。

## 配置

Codex 额度插件要求同一个 Windows 用户账户下已经完成 Codex 登录。插件会读取本机 Codex 认证文件，并在后台刷新额度。

GitHub Copilot 额度插件需要在 TrafficMonitor 插件选项中点击 `Sign in with GitHub` 登录。插件会把 OAuth token 作为 TrafficMonitor 专用的本地凭据保存到 Windows 凭据管理器。

两个插件的选项都可以控制显示剩余额度还是已用额度、是否显示重置信息，以及已显示的重置信息使用倒计时还是本地时间。

## 发布包内容

每个 Release ZIP 包含：

```text
TrafficMonitorCodexQuota.dll
TrafficMonitorGitHubCopilotQuota.dll
README.md
README.zh-CN.md
LICENSE
THIRD_PARTY_NOTICES.md
```

运行时只需要 DLL 文件。文本文件随包提供，是为了让下载包自带许可和使用说明。

## 开发

构建、测试、发布和实现细节不放在主 README 中：

- [开发指南](docs/development.md)
- [实现说明](docs/implementation-notes.md)
- [Codex 插件设计](docs/design.md)

## 许可证

插件代码使用 MIT 许可证发布。复制自 TrafficMonitor 的插件接口头文件保留其上游版权和许可证；详情见 `THIRD_PARTY_NOTICES.md`。
