# Implementation Notes

These notes capture project-specific context from the initial implementation and tuning work so future Codex threads can continue without relying on chat history.

## References

TrafficMonitor plugin API:

- https://github.com/zhongyang219/TrafficMonitor/wiki/%E6%8F%92%E4%BB%B6%E5%BC%80%E5%8F%91%E6%8C%87%E5%8D%97

Codex quota query behavior:

- https://github.com/Finesssee/Win-CodexBar

The plugin follows the Win-CodexBar approach for locating Codex credentials and calling the ChatGPT usage endpoint.

## Data Source

Codex credential lookup:

- Use `%CODEX_HOME%\auth.json` when `CODEX_HOME` is set.
- Otherwise use `%USERPROFILE%\.codex\auth.json`.
- Prefer the `OPENAI_API_KEY` field when it is present in the Codex auth file.
- Otherwise use `tokens.access_token` from `auth.json`.
- Send `ChatGPT-Account-Id` when `tokens.account_id` is present.

Usage endpoint:

- `GET https://chatgpt.com/backend-api/wham/usage`

Fields used:

- `rate_limit.primary_window.used_percent`
- `rate_limit.primary_window.reset_at`
- `rate_limit.secondary_window.used_percent`
- `rate_limit.secondary_window.reset_at`

The default displayed percentage is remaining percentage: `100 - used_percent`. The user can switch the display to the raw used percentage from the plugin options dialog.

Optional Codex display configuration is stored at:

- `%APPDATA%\TrafficMonitorCodexQuota\config.json`

Supported Codex display config keys:

- `quota_display`: `remaining` or `used`.
- `reset_display`: `countdown` or `time`.
- `show_reset_info`: `true` or `false`; when false, the taskbar value omits reset countdown/time.

Missing config uses `remaining` plus `countdown`, with reset info visible.

## TrafficMonitor GitHub Copilot Quota Plugin

The GitHub Copilot plugin is built as `TrafficMonitorGitHubCopilotQuota.dll`, separate from the Codex plugin. It exposes one TrafficMonitor item:

- `GitHubCopilotQuotaAI` with label `GC:`

The value text starts with one regular space, for example label `GC:` plus value ` 82% 1.2kcr 12d` displays as `GC: 82% 1.2kcr 12d`. Keep the visible spacing in the value because TrafficMonitor trims ordinary whitespace at plugin-label edges.

The plugin follows the Win-CodexBar Copilot provider approach and queries GitHub's Copilot internal user endpoint:

- `GET https://api.github.com/copilot_internal/user`

Token lookup:

- Prefer the plugin-managed GitHub OAuth token in Windows Credential Manager.
- Plaintext `github_token` config and token environment-variable overrides are not supported.

The preferred user setup path is the plugin options dialog:

1. User clicks `Sign in with GitHub`.
2. The plugin requests a GitHub device code from `POST https://github.com/login/device/code`.
3. The plugin displays the returned `user_code`, copies it to the clipboard, and opens the verification URL in the default browser.
4. If GitHub omits `verification_uri_complete`, the plugin appends `user_code` to `verification_uri` so the browser can prefill the device page when supported.
5. The plugin polls `POST https://github.com/login/oauth/access_token` until GitHub returns an OAuth token or the flow fails.
6. The plugin verifies the token with `GET /user` and `GET /copilot_internal/user`.
7. The plugin stores the token only after both verification requests succeed.

The options dialog reads the stored credential's `UserName` field and shows `Status: signed in as <login>.` in bold when that verified login is available. GitHub token environment-variable overrides are not supported.

Device flow details:

- OAuth client id: `Iv1.b507a08c87ecfe98`
- The OAuth app name shown by GitHub is bound to this `client_id`; local request text cannot rename it. The plugin's own windows, User-Agent, and credential target are TrafficMonitor-scoped.
- Scope: `read:user`
- Credential Manager target: `TrafficMonitorGitHubCopilotQuota:GitHubOAuth`
- Credential type: `CRED_TYPE_GENERIC`
- Credential persistence: `CRED_PERSIST_LOCAL_MACHINE`

Request headers intentionally mirror VS Code Copilot Chat:

- `Accept: application/json`
- `Authorization: token <token>`
- `Editor-Version: vscode/1.96.2`
- `Editor-Plugin-Version: copilot-chat/0.26.7`
- `User-Agent: GitHubCopilotChat/0.26.7`
- `X-Github-Api-Version: 2025-04-01`

Response fields used:

- `copilot_plan`
- `quota_reset_date`
- `quota_snapshots.*.entitlement`
- `quota_snapshots.*.remaining`
- `quota_snapshots.*.percent_remaining`
- fallback: `monthly_quotas` plus `limited_user_quotas`

The internal response provides quota totals, remaining values, remaining percentage, and reset date. GitHub billing API allowance fields such as `plan`, `total_credits`, and `billing_day` are not part of the active configuration.

Optional configuration is stored at:

- `%APPDATA%\TrafficMonitorGitHubCopilotQuota\config.json`

Currently useful optional config keys:

- `username`: displayed in the tooltip only; no `/user` request is needed for quota fetching.
- `quota_display`: `remaining` or `used`.
- `reset_display`: `countdown` or `time`.
- `show_reset_info`: `true` or `false`; when false, the taskbar value omits reset countdown/time.
- `show_remaining_credits`: `true` or `false`; the credit count is always remaining credits when shown.

TrafficMonitor may cache the Copilot plugin label in `C:\Apps\TrafficMonitor\config.ini`:

```ini
GitHubCopilotQuotaAI = GC:
```

Important: `config.ini` is GBK encoded on this machine. Use code page 936 when editing it from scripts. Rewriting it as UTF-8 can garble existing Chinese labels.

## Display Decisions

The taskbar items are:

- `CodexQuota5h` with label `CX 5h:`
- `CodexQuotaWeek` with label `CX 7d:`

Codex has no official short abbreviation in the referenced OpenAI docs. The labels use `CX` as this plugin's compact Codex prefix.

The value text starts with one regular space:

- label `CX 5h:` plus value ` 69% 42m` displays as `CX 5h: 69% 42m`
- label `CX 7d:` plus value ` 89% 6d 1h` displays as `CX 7d: 89% 6d 1h`

Do not move that space into the label. TrafficMonitor trims ordinary whitespace at the leading and trailing edges of plugin labels, so label strings such as `CX 5h: ` or ` CX 5h:` do not reliably show visible spacing.

`GetItemValueSampleText()` follows the current display options so TrafficMonitor does not reserve width for hidden fields. Codex countdown samples:

- ` 100% 4h 59m` for `CodexQuota5h`
- ` 100% 6d 23h` for `CodexQuotaWeek`

Codex reset-time sample:

- ` 100% 12-31 23:59`

Codex hidden-reset sample:

- ` 100%`

GitHub Copilot countdown samples:

- ` 100% 20.0kcr 31d` when remaining credits are shown
- ` 100% 31d` when remaining credits are hidden

GitHub Copilot reset-time samples:

- ` 100% 20.0kcr 12-31 23:59` when remaining credits are shown
- ` 100% 12-31 23:59` when remaining credits are hidden

GitHub Copilot hidden-reset samples:

- ` 100% 20.0kcr` when remaining credits are shown
- ` 100%` when remaining credits are hidden

## Reset Countdown

When reset info is visible, the value suffix is a compact reset countdown:

- minutes for the 5-hour window, for example `42m`
- hours and minutes when useful, for example `4h 30m`
- days and hours for the 7-day window, for example `6d 1h`
- weeks for exact or longer week values, for example `1w`

Keep this compact; TrafficMonitor taskbar space is limited.

When `reset_display` is set to `time`, the suffix is local time:

- same local day: `18:30`
- different local day: `06-23 18:30`

## Refresh Behavior

`ITMPlugin::DataRequired()` must not block TrafficMonitor on network I/O. It should start or reuse a background refresh.

Refresh cadence:

- 5 minutes after a successful fetch.
- 1 minute after an error.

The initial display value is ` ...`. Error display is ` ERR` unless a previous successful value can still be shown.

## TrafficMonitor Config Notes

A local test install commonly uses:

- `C:\Apps\TrafficMonitor`

Installed plugin path:

- `C:\Apps\TrafficMonitor\plugins\TrafficMonitorCodexQuota.dll`

TrafficMonitor may cache plugin label text in:

- `C:\Apps\TrafficMonitor\config.ini`

Important: `config.ini` is GBK encoded on this machine. Use code page 936 when editing it from scripts. Rewriting it as UTF-8 caused existing Chinese labels to become garbled.

Expected cached labels:

```ini
CodexQuota5h = CX 5h:
CodexQuotaWeek = CX 7d:
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
$env:TRAFFICMONITOR_CODEX_QUOTA_RUN_LIVE_TEST = '1'
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
