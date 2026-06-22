# TrafficMonitor Codex Quota Plugin

TrafficMonitor x64 plugin that displays remaining Codex quota percentage.

Display items:

- `5h:`: remaining percent of the 5-hour Codex window plus reset countdown.
- `7d:`: remaining percent of the 7-day Codex window plus reset countdown.

Example taskbar values:

- `5h: 69% 42m`
- `7d: 89% 6d 1h`

The suffix after the percent is the countdown until that quota window resets. The displayed value includes a leading space because TrafficMonitor trims ordinary whitespace at the edges of plugin labels. The plugin reserves extra width so values such as `6d 23h` are not truncated.

The Codex plugin reads the local Codex CLI auth file and calls the same ChatGPT backend usage endpoint used by Win-CodexBar.

## GitHub Copilot Quota Plugin

This repository also builds `TrafficMonitorGitHubCopilotQuota.dll`, a separate x64 TrafficMonitor plugin for GitHub Copilot quota.

The plugin exposes one display item:

- `GC:`: remaining GitHub Copilot quota percentage, compact remaining-count value, and optional reset countdown.

Example taskbar values:

- `GC: 82% 1.2kcr 12d`
- `GC: 100% 1500cr`

The value text starts with a regular space, for example label `GC:` plus value ` 82% 1.2kcr 12d`, because TrafficMonitor trims ordinary whitespace at plugin-label edges.

Recommended authentication is the plugin options dialog:

1. Open the GitHub Copilot quota plugin options in TrafficMonitor.
2. Click `Sign in with GitHub`.
3. The plugin shows the GitHub device code and copies it to the clipboard.
4. Complete the GitHub browser/device-code sign-in. If GitHub asks for a code, paste the copied code.

The plugin stores the resulting OAuth token in Windows Credential Manager as a protected local credential. The sign-in flow is only started by the user from options; the background quota refresh never opens a browser.

`COPILOT_QUOTA_GITHUB_TOKEN` is still supported as the highest-priority override. With that environment variable set, no config file or stored credential is required.

Optional configuration is stored at `%APPDATA%\TrafficMonitorGitHubCopilotQuota\config.json`. Use it only when you want a tooltip username or a legacy plaintext fallback token:

```json
{
  "username": "YOUR_GITHUB_LOGIN"
}
```

Plaintext token fallback is still supported for compatibility, but plugin options sign-in or the environment variable is preferred:

```json
{
  "github_token": "YOUR_GITHUB_TOKEN",
  "username": "YOUR_GITHUB_LOGIN"
}
```

The plugin uses the same Copilot internal quota endpoint pattern as Win-CodexBar, so `plan`, `total_credits`, and `billing_day` are not required.

Project-specific implementation notes and known pitfalls are in `docs/implementation-notes.md`.

## Build

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\TrafficMonitorCodexQuota.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\CodexQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\PluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\TrafficMonitorGitHubCopilotQuota.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\GitHubCopilotQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\GitHubCopilotPluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

## Test

```powershell
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe
.\build\x64\Release\GitHubCopilotQuotaTests.exe
.\build\x64\Release\GitHubCopilotPluginSmokeTests.exe

$env:CODEX_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe

$env:GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\GitHubCopilotQuotaTests.exe
```

## Install

```powershell
New-Item -ItemType Directory -Force 'C:\Apps\TrafficMonitor\plugins' | Out-Null
Copy-Item -Force '.\build\x64\Release\TrafficMonitorCodexQuota.dll' 'C:\Apps\TrafficMonitor\plugins\TrafficMonitorCodexQuota.dll'
Copy-Item -Force '.\build\x64\Release\TrafficMonitorGitHubCopilotQuota.dll' 'C:\Apps\TrafficMonitor\plugins\TrafficMonitorGitHubCopilotQuota.dll'
```

Restart TrafficMonitor after copying the DLL. Enable `CodexQuota5h`, `CodexQuotaWeek`, and `GitHubCopilotQuotaAI` in the taskbar display item settings, or set:

```ini
[task_bar]
plugin_display_item = CodexQuota5h,CodexQuotaWeek,GitHubCopilotQuotaAI
```
