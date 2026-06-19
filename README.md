# TrafficMonitor Codex Quota Plugin

TrafficMonitor x64 plugin that displays remaining Codex quota percentage.

Display items:

- `5h:`: remaining percent of the 5-hour Codex window plus reset countdown.
- `7d:`: remaining percent of the 7-day Codex window plus reset countdown.

Example taskbar values:

- `5h: 69% 42m`
- `7d: 89% 6d 1h`

The suffix after the percent is the countdown until that quota window resets. The displayed value includes a leading space because TrafficMonitor trims ordinary whitespace at the edges of plugin labels. The plugin reserves extra width so values such as `6d 23h` are not truncated.

The plugin reads the local Codex CLI auth file and calls the same ChatGPT backend usage endpoint used by Win-CodexBar.

## Build

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\TrafficMonitorCodexQuota.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\CodexQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\PluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

## Test

```powershell
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe

$env:CODEX_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe
```

## Install

```powershell
New-Item -ItemType Directory -Force 'C:\Apps\TrafficMonitor\plugins' | Out-Null
Copy-Item -Force '.\build\x64\Release\TrafficMonitorCodexQuota.dll' 'C:\Apps\TrafficMonitor\plugins\TrafficMonitorCodexQuota.dll'
```

Restart TrafficMonitor after copying the DLL. Enable `CodexQuota5h` and `CodexQuotaWeek` in the taskbar display item settings, or set:

```ini
[task_bar]
plugin_display_item = CodexQuota5h,CodexQuotaWeek
```
