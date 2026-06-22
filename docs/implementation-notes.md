# Implementation Notes

These notes capture project-specific context from the initial implementation and tuning work so future Codex threads can continue without relying on chat history.

## References

TrafficMonitor plugin API:

- https://github.com/zhongyang219/TrafficMonitor/wiki/%E6%8F%92%E4%BB%B6%E5%BC%80%E5%8F%91%E6%8C%87%E5%8D%97

Codex quota query behavior:

- https://github.com/Finesssee/Win-CodexBar

The plugin follows the Win-CodexBar approach for locating Codex credentials and calling the ChatGPT usage endpoint.

## Data Source

Credential lookup:

- Use `%CODEX_HOME%\auth.json` when `CODEX_HOME` is set.
- Otherwise use `%USERPROFILE%\.codex\auth.json`.
- Prefer `OPENAI_API_KEY` when present.
- Otherwise use `tokens.access_token` from `auth.json`.
- Send `ChatGPT-Account-Id` when `tokens.account_id` is present.

Usage endpoint:

- `GET https://chatgpt.com/backend-api/wham/usage`

Fields used:

- `rate_limit.primary_window.used_percent`
- `rate_limit.primary_window.reset_at`
- `rate_limit.secondary_window.used_percent`
- `rate_limit.secondary_window.reset_at`

The displayed percentage is remaining percentage: `100 - used_percent`.

## GitHub Copilot Quota Plugin

The GitHub Copilot plugin is built as `TrafficMonitorGitHubCopilotQuota.dll`, separate from the Codex plugin. It exposes one TrafficMonitor item:

- `GitHubCopilotQuotaAI` with label `GC:`

The value text starts with one regular space, for example label `GC:` plus value ` 82% 1.2kcr 12d` displays as `GC: 82% 1.2kcr 12d`. Keep the visible spacing in the value because TrafficMonitor trims ordinary whitespace at plugin-label edges.

Configuration is stored at:

- `%APPDATA%\TrafficMonitorGitHubCopilotQuota\config.json`

Token lookup:

- Prefer `COPILOT_QUOTA_GITHUB_TOKEN`.
- Fall back to optional plaintext `github_token` in `config.json`.

API calls:

- `GET https://api.github.com/user` when `username` is omitted.
- `GET https://api.github.com/users/{username}/settings/billing/ai_credit/usage`.

Allowance resolution:

- Prefer `total_credits` when present.
- Otherwise use known plan values: `pro` = 1500, `pro_plus` = 7000, `max` = 20000.

`billing_day` is needed for exact billing-cycle usage and reset countdowns. Without it, the plugin uses a current calendar month estimate and omits the exact reset countdown.

TrafficMonitor may cache the Copilot plugin label in `C:\Apps\TrafficMonitor\config.ini`:

```ini
GitHubCopilotQuotaAI = GC:
```

Important: `config.ini` is GBK encoded on this machine. Use code page 936 when editing it from scripts. Rewriting it as UTF-8 can garble existing Chinese labels.

## Display Decisions

The taskbar items are:

- `CodexQuota5h` with label `5h:`
- `CodexQuotaWeek` with label `7d:`

The weekly label intentionally uses `7d:` instead of `W:` because it visually matches `5h:` better and describes the API's 7-day secondary window.

The value text starts with one regular space:

- label `5h:` plus value ` 69% 42m` displays as `5h: 69% 42m`
- label `7d:` plus value ` 89% 6d 1h` displays as `7d: 89% 6d 1h`

Do not move that space into the label. TrafficMonitor trims ordinary whitespace at the leading and trailing edges of plugin labels, so label strings such as `5h: ` or ` 5h: ` do not reliably show visible spacing.

`GetItemValueSampleText()` must reserve enough width for long countdown values. Current samples:

- ` 100% 4h 59m`
- ` 100% 6d 23h`

## Reset Countdown

The value suffix is a compact reset countdown:

- minutes for the 5-hour window, for example `42m`
- hours and minutes when useful, for example `4h 30m`
- days and hours for the 7-day window, for example `6d 1h`
- weeks for exact or longer week values, for example `1w`

Keep this compact; TrafficMonitor taskbar space is limited.

## Refresh Behavior

`ITMPlugin::DataRequired()` must not block TrafficMonitor on network I/O. It should start or reuse a background refresh.

Refresh cadence:

- 5 minutes after a successful fetch.
- 1 minute after an error.

The initial display value is ` ...`. Error display is ` ERR` unless a previous successful value can still be shown.

## TrafficMonitor Config Notes

The user's TrafficMonitor install path during development was:

- `C:\Apps\TrafficMonitor`

Installed plugin path:

- `C:\Apps\TrafficMonitor\plugins\TrafficMonitorCodexQuota.dll`

TrafficMonitor may cache plugin label text in:

- `C:\Apps\TrafficMonitor\config.ini`

Important: `config.ini` is GBK encoded on this machine. Use code page 936 when editing it from scripts. Rewriting it as UTF-8 caused existing Chinese labels to become garbled.

Expected cached labels:

```ini
CodexQuota5h = 5h:
CodexQuotaWeek = 7d:
```

The visible colon spacing is provided by the value text, not this cache.

## Build And Test Notes

`PluginSmokeTests.exe` loads the plugin DLL from the Release output directory:

- `build\x64\Release\TrafficMonitorCodexQuota.dll`

Always rebuild `TrafficMonitorCodexQuota.vcxproj` before running `PluginSmokeTests`, otherwise the smoke test may inspect an older DLL.

Normal verification sequence:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\TrafficMonitorCodexQuota.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\CodexQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\PluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe
$env:CODEX_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe
```

## Installation Notes

TrafficMonitor locks the installed DLL while it is running. Stop TrafficMonitor before copying a new DLL.

After copying:

1. Verify the installed DLL hash matches `build\x64\Release\TrafficMonitorCodexQuota.dll`.
2. Verify `config.ini` cached labels if labels changed.
3. Restart TrafficMonitor.

Starting TrafficMonitor from automation can trigger UAC. If the process does not appear after launch, check for a UAC confirmation prompt.
