# TrafficMonitor Agent Quota Plugins

TrafficMonitor x64 plugins that display Codex and GitHub Copilot quota percentages.

Unofficial project. This plugin is not affiliated with OpenAI, GitHub, or the
TrafficMonitor project.

Display items:

- `CX 5h:`: 5-hour Codex window quota plus reset information.
- `CX 7d:`: 7-day Codex window quota plus reset information.

Example taskbar values:

- `CX 5h: 69% 42m`
- `CX 7d: 89% 6d 1h`

By default the percent is remaining quota and the suffix is the countdown until that quota window resets. The displayed value includes a leading space because TrafficMonitor trims ordinary whitespace at the edges of plugin labels. The plugin reserves width from the current display mode: countdown mode uses compact countdown samples, while reset-time mode reserves enough room for values such as `12-31 23:59`.

The Codex plugin reads the local Codex CLI auth file and calls the same ChatGPT backend usage endpoint used by Win-CodexBar.

Optional Codex display configuration is stored at `%APPDATA%\TrafficMonitorCodexQuota\config.json` and can be changed from the plugin options dialog:

```json
{
  "quota_display": "remaining",
  "reset_display": "countdown"
}
```

`quota_display` can be `remaining` or `used`. `reset_display` can be `countdown` or `time`; time mode shows local reset time such as `18:30` or `06-23 18:30`.

## TrafficMonitor GitHub Copilot Quota Plugin

This repository also builds `TrafficMonitorGitHubCopilotQuota.dll`, a separate x64 TrafficMonitor plugin for GitHub Copilot quota.

The plugin exposes one display item:

- `GC:`: GitHub Copilot quota percentage, optional compact remaining-credit value, and reset information.

Example taskbar values:

- `GC: 82% 1.2kcr 12d`
- `GC: 100% 1500cr`

The value text starts with a regular space, for example label `GC:` plus value ` 82% 1.2kcr 12d`, because TrafficMonitor trims ordinary whitespace at plugin-label edges.

Recommended authentication is the TrafficMonitor plugin options dialog:

1. Open the TrafficMonitor GitHub Copilot quota plugin options.
2. Click `Sign in with GitHub`.
3. The plugin shows the GitHub device code and copies it to the clipboard.
4. Complete the GitHub browser/device-code sign-in. If GitHub asks for a code, paste the copied code.

The plugin stores the resulting OAuth token in Windows Credential Manager as a protected local credential for TrafficMonitor. The sign-in flow is only started by the user from options; the background quota refresh never opens a browser. GitHub token environment-variable overrides are not supported.

Optional display configuration is stored at `%APPDATA%\TrafficMonitorGitHubCopilotQuota\config.json` and can be changed from the plugin options dialog:

```json
{
  "username": "YOUR_GITHUB_LOGIN",
  "quota_display": "remaining",
  "reset_display": "countdown",
  "show_remaining_credits": true
}
```

`quota_display` can be `remaining` or `used`. `reset_display` can be `countdown` or `time`. `show_remaining_credits` controls whether the value includes the remaining credit count such as `1.2kcr`; when the percent is set to `used`, the credit count is still remaining credits. The taskbar sample width follows these options, so hidden credit counts do not reserve extra space.

The plugin uses the same Copilot internal quota endpoint pattern as Win-CodexBar. Manual token configuration, GitHub token environment-variable overrides, and GitHub billing API allowance fields are not supported.

Project-specific implementation notes and known pitfalls are in `docs/implementation-notes.md`.

## Versioning

Both plugin DLLs share one release version in `src/PluginVersion.h`. Bump that version once for each tagged repository release, then tag the release as `v<version>`, for example `v1.1`.

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

$env:TRAFFICMONITOR_CODEX_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe

$env:TRAFFICMONITOR_GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST = '1'
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

## License

The plugin code is released under the MIT license. The copied TrafficMonitor
plugin interface header keeps its upstream copyright and license; see
`THIRD_PARTY_NOTICES.md`.
