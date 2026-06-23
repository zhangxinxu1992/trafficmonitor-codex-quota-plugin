# TrafficMonitor Codex Quota Plugin Design

## Goal

Build a x64 TrafficMonitor plugin DLL that shows the user's Codex quota percentage in the taskbar and main TrafficMonitor display.

The plugin exposes two display items:

- `CX 5h:`: percentage of the Codex 5-hour primary rate window plus reset information.
- `CX 7d:`: percentage of the Codex 7-day secondary rate window plus reset information.

Values use compact text, for example `CX 5h: 76% 42m` or `CX 7d: 90% 6d 1h`. By default the percentage is remaining quota and the suffix is the countdown until that quota window resets. The user can switch the percentage to used quota, hide reset information, or show visible reset information as local reset time. The taskbar value text starts with a regular space so spacing remains visible after TrafficMonitor trims plugin-label edges. `GetItemValueSampleText()` follows the current display mode: countdown mode uses compact samples such as ` 100% 4h 59m` or ` 100% 6d 23h`, reset-time mode reserves enough width for values such as ` 100% 12-31 23:59`, and hidden reset mode reserves only ` 100%`.

## Data Source

The plugin follows the Win-CodexBar Codex provider approach:

1. Read Codex credentials from `%CODEX_HOME%\auth.json` when `CODEX_HOME` is set, otherwise `%USERPROFILE%\.codex\auth.json`.
2. Use the `OPENAI_API_KEY` field from the Codex auth file if present, otherwise use `tokens.access_token`.
3. Add `ChatGPT-Account-Id` when `tokens.account_id` is present.
4. Send a read-only HTTPS GET request to `https://chatgpt.com/backend-api/wham/usage`.
5. Parse:
   - `rate_limit.primary_window.used_percent`
   - `rate_limit.primary_window.limit_window_seconds`
   - `rate_limit.primary_window.reset_at`
   - `rate_limit.secondary_window.used_percent`
   - `rate_limit.secondary_window.limit_window_seconds`
   - `rate_limit.secondary_window.reset_at`

The first version intentionally ignores `additional_rate_limits`, including Spark-specific quota windows.

## Configuration

Optional Codex display configuration lives at `%APPDATA%\TrafficMonitorCodexQuota\config.json`:

```json
{
  "quota_display": "remaining",
  "reset_display": "countdown",
  "show_reset_info": true
}
```

`quota_display` accepts `remaining` or `used`. `show_reset_info` accepts `true` or `false`. `reset_display` accepts `countdown` or `time` and only affects the taskbar value when `show_reset_info` is `true`. Missing config uses the defaults above.

## Runtime Behavior

`ITMPlugin::DataRequired()` must not block TrafficMonitor on network I/O. It starts a background refresh when the cached snapshot is stale and no refresh is already running.

The refresh interval is five minutes after a successful fetch and one minute after an error.

The display item value is:

- `...` before the first fetch completes.
- `<n>% <reset>` when the matching window is available.
- `ERR` when the most recent refresh failed and no valid value is available.

The tooltip includes the plan, both windows, reset information, last refresh status, and the last error message when present.

## Error Handling

- Missing `auth.json`: show an authentication error in the tooltip.
- Missing token: show an authentication error in the tooltip.
- HTTP 401 or 403: show that `codex login` is required.
- Other HTTP/network/parser failures: keep the last successful value if available and show the error in the tooltip.

## Implementation Shape

The project uses a small shared C++17 core plus two binaries:

- `TrafficMonitorCodexQuota.dll`: the TrafficMonitor plugin.
- `CodexQuotaTests.exe`: console tests for credential parsing, usage JSON parsing, percent formatting, and reset countdown formatting.

The plugin uses WinHTTP for HTTPS and has no third-party runtime dependency.
