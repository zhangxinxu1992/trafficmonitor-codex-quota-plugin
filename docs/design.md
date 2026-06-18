# TrafficMonitor Codex Quota Plugin Design

## Goal

Build a x64 TrafficMonitor plugin DLL that shows the user's Codex quota usage in the taskbar and main TrafficMonitor display.

The plugin exposes two display items:

- `Codex 5h`: used percentage of the Codex 5-hour primary rate window.
- `Codex Week`: used percentage of the Codex 7-day secondary rate window.

Values are displayed as used percentages, for example `24%`.

## Data Source

The plugin follows the Win-CodexBar Codex provider approach:

1. Read Codex credentials from `%CODEX_HOME%\auth.json` when `CODEX_HOME` is set, otherwise `%USERPROFILE%\.codex\auth.json`.
2. Use `OPENAI_API_KEY` if present, otherwise use `tokens.access_token`.
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

## Runtime Behavior

`ITMPlugin::DataRequired()` must not block TrafficMonitor on network I/O. It starts a background refresh when the cached snapshot is stale and no refresh is already running.

The refresh interval is five minutes after a successful fetch and one minute after an error.

The display item value is:

- `...` before the first fetch completes.
- `<n>%` when the matching window is available.
- `ERR` when the most recent refresh failed and no valid value is available.

The tooltip includes the plan, both windows, reset countdowns, last refresh status, and the last error message when present.

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
