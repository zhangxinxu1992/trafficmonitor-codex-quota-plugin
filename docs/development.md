# Development

This document keeps build, test, and release details out of the user-facing
README.

## Versioning

Both plugin DLLs share one release version in `src/PluginVersion.h`, backed by
the macros in `src/PluginVersionResource.h`. Bump that version once for each
tagged repository release, then tag the release as `v<version>`, for example
`v1.1`.

## Build

Use Release x64 builds.

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
```

Live tests are optional and depend on the local authenticated accounts:

```powershell
$env:TRAFFICMONITOR_CODEX_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe

$env:TRAFFICMONITOR_GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\GitHubCopilotQuotaTests.exe
```

## Local Install for Testing

TrafficMonitor locks installed plugin DLLs while it is running. Stop
TrafficMonitor before copying new DLLs into the plugin directory.

```powershell
New-Item -ItemType Directory -Force 'C:\Apps\TrafficMonitor\plugins' | Out-Null
Copy-Item -Force '.\build\x64\Release\TrafficMonitorCodexQuota.dll' 'C:\Apps\TrafficMonitor\plugins\TrafficMonitorCodexQuota.dll'
Copy-Item -Force '.\build\x64\Release\TrafficMonitorGitHubCopilotQuota.dll' 'C:\Apps\TrafficMonitor\plugins\TrafficMonitorGitHubCopilotQuota.dll'
```

Restart TrafficMonitor after copying the DLLs. If labels change, remember that
`C:\Apps\TrafficMonitor\config.ini` is GBK encoded on this machine.

## Release Package

The GitHub Actions release workflow builds both plugin DLLs, runs the non-live
tests, and uploads one ZIP:

```text
trafficmonitor-agent-quota-plugins-v<version>.zip
```

The ZIP contains both plugin DLLs plus README, license, and third-party notice
files.
