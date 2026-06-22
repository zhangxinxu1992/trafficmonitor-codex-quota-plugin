# TrafficMonitor GitHub Copilot Quota Plugin Design

> Agent design history: this file is preserved to document the implementation
> reasoning and evolution of the plugin. It is not the current product
> contract; use `README.md`, `docs/implementation-notes.md`, source, and tests
> as the current authority.

> Historical note: this original design targeted GitHub's AI Credits billing API. The current implementation instead follows the Win-CodexBar Copilot provider pattern and queries `/copilot_internal/user`; see `README.md` and `docs/implementation-notes.md` for the active behavior. `plan`, `total_credits`, and `billing_day` are no longer required.

## Goal

Build a separate x64 TrafficMonitor plugin DLL that shows the user's remaining GitHub Copilot monthly AI Credits in the taskbar and main TrafficMonitor display.

The plugin exposes one display item:

- `GC:`: remaining monthly GitHub Copilot AI Credits percentage, compact remaining-credit count, and optional reset countdown.

Example taskbar values:

- `GC: 82% 1.2kcr 12d`
- `GC: 100% 1500cr`
- `GC: 0% 0cr now`

The visible label is `GC:` because it is the requested abbreviation for GitHub Copilot. The value text starts with a regular leading space so the rendered taskbar text has visible spacing after the colon even when TrafficMonitor trims plugin-label edges.

## References

- GitHub Copilot AI Credits billing: https://docs.github.com/en/copilot/concepts/billing/usage-based-billing-for-individuals
- GitHub billing usage REST API: https://docs.github.com/en/rest/billing/usage#get-billing-ai-credit-usage-report-for-a-user

## Scope

This is a new plugin alongside the existing Codex quota plugin. It must not change the existing Codex display items, data source, refresh behavior, or tests except where shared helper extraction is explicitly needed by the implementation.

The first version tracks GitHub AI Credits, not legacy Copilot premium requests and not IDE session or weekly rate limits.

## Data Source

Use GitHub's official REST API for AI Credits usage:

1. Read a GitHub token from the TrafficMonitor plugin-managed Windows Credential Manager entry.
2. If the stored credential is empty, read the legacy plaintext `github_token` from `%APPDATA%\TrafficMonitorGitHubCopilotQuota\config.json`. GitHub token environment-variable overrides were removed.
3. Call `GET https://api.github.com/user` to determine the authenticated username unless the config explicitly provides `username`.
4. Calculate the usage period:
   - If `billing_day` is configured, derive the current billing cycle start and end dates.
   - If `billing_day` is absent, use the current calendar month and mark the tooltip as a calendar-month estimate.
5. Fetch usage:
   - For a configured billing cycle, call the usage endpoint once per UTC date in the cycle so partial cycles that cross calendar-month boundaries are counted correctly.
   - Without `billing_day`, call `GET https://api.github.com/users/{username}/settings/billing/ai_credit/usage?year=YYYY&month=MM`.
6. Sum `usageItems[].netQuantity` to get consumed AI Credits for the selected period.

The token must have user `Plan` read permission for the user billing usage endpoint. The plugin does not create or modify budgets.

GitHub's usage endpoint reports consumption. It does not provide a simple per-user included allowance in the same response, so the plugin needs a local allowance source:

```json
{
  "github_token": "optional-legacy-plaintext-token",
  "username": "optional-github-login",
  "plan": "pro",
  "total_credits": 1500,
  "billing_day": 1
}
```

Allowance resolution order:

1. Use `total_credits` when present and greater than zero.
2. Otherwise map `plan` to the currently documented individual-plan allowance:
   - `pro`: 1500 credits
   - `pro_plus`: 7000 credits
   - `max`: 20000 credits
3. If neither `total_credits` nor a known `plan` is configured, fetch usage but show an error state because remaining quota cannot be computed.

`billing_day` is optional. When present, it is used for both usage-period calculation and the reset countdown. When absent, the value omits the countdown and the tooltip says the usage is a current-calendar-month estimate because the reset date is not configured.

## Display Rules

The TrafficMonitor item uses:

- item id: `GitHubCopilotQuotaAI`
- item name: `GitHub Copilot AI Credits`
- label: `GC:`
- sample value: ` 100% 20.0kcr 31d`

Value format:

- Loading before first fetch: ` ...`
- Success with reset countdown: ` <remaining-percent> <remaining-credit-text> <reset>`
- Success without reset countdown: ` <remaining-percent> <remaining-credit-text>`
- Error with no previous value: ` ERR`
- Error with previous value: keep showing the previous value and expose the error in the tooltip.

Credit count formatting:

- Under 1000 credits: whole number plus `cr`, for example `950cr`.
- 1000 credits and above: one decimal `kcr`, for example `1.2kcr`.
- Remaining credits are clamped to zero for display when usage exceeds the included allowance.

Reset countdown formatting uses a compact monthly style. Multi-day GitHub Copilot values remain day-based rather than collapsing to weeks, so `GC:` output stays consistent with the monthly examples:

- `7d`
- `12d`
- `31d`
- `1d 4h`
- `3h 15m`
- `now`

## Runtime Behavior

`ITMPlugin::DataRequired()` must not block TrafficMonitor on network I/O. It starts a background refresh when the cached snapshot is stale and no refresh is already running.

Refresh cadence:

- 15 minutes after a successful fetch.
- 2 minutes after an error.

The longer success interval is acceptable because GitHub billing usage is monthly aggregate data and does not need minute-level freshness in the taskbar.

The tooltip includes:

- GitHub Copilot quota title.
- Username when known.
- Configured plan or explicit total allowance.
- Consumed credits.
- Remaining credits.
- Remaining percentage.
- Reset countdown or reset-date-not-configured note.
- Whether usage is exact for the configured billing cycle or a calendar-month estimate.
- Last refresh status.
- Last error message when present.

## Error Handling

- Missing token: show `ERR`; tooltip tells the user to sign in from TrafficMonitor plugin options or set the legacy `github_token` config fallback.
- HTTP 401 or 403: show authentication or permission error and mention required `Plan` read permission.
- Missing allowance config: show `ERR`; tooltip says `plan` or `total_credits` is required.
- GitHub usage API response without `usageItems`: show parser error.
- Network and non-2xx failures: keep the last successful value if available and show the error in the tooltip.
- Usage above allowance: show `0% 0cr`; tooltip reports consumed credits and allowance so overage is visible.

## Implementation Shape

Keep the implementation close to the existing project pattern:

- `TrafficMonitorGitHubCopilotQuota.dll`: TrafficMonitor plugin.
- `GitHubCopilotQuotaCore.*`: config parsing, usage JSON parsing, allowance calculation, credit/countdown formatting.
- `GitHubCopilotQuotaFetch.*`: GitHub token/config loading and WinHTTP requests.
- `GitHubCopilotQuotaTests.exe`: console tests for config parsing, usage parsing, allowance calculation, formatting, and reset countdown behavior.
- Add or extend smoke tests so the new DLL can be loaded and its item metadata is verified.

Use WinHTTP and the C++17 standard library only. Do not add third-party runtime dependencies.

## Testing

Use test-first implementation for behavior changes.

Core tests cover:

- Config parsing with explicit `total_credits`.
- Config parsing with known `plan`.
- Error when allowance cannot be resolved.
- Usage response parsing and summing `usageItems[].netQuantity`.
- Remaining percent and remaining credit calculation.
- `GC:` metadata and leading-space value convention.
- Credit count formatting for `cr` and `kcr`.
- Reset countdown when `billing_day` is configured.
- Omitted countdown when `billing_day` is absent.

Live tests should be gated behind a separate environment flag, for example `GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST=1`, and should not run by default.

## Installation Notes

Install the built DLL into the TrafficMonitor plugins directory as:

```powershell
Copy-Item -Force '.\build\x64\Release\TrafficMonitorGitHubCopilotQuota.dll' 'C:\Apps\TrafficMonitor\plugins\TrafficMonitorGitHubCopilotQuota.dll'
```

TrafficMonitor may cache plugin labels in `C:\Apps\TrafficMonitor\config.ini`. If label cache updates are needed, preserve the file's GBK encoding.
