# GitHub Copilot Device Auth Design

## Goal

Replace the GitHub Copilot quota plugin's manual-token-only setup with a user-initiated GitHub sign-in flow from the TrafficMonitor plugin options dialog.

The existing token paths remain compatible:

- `COPILOT_QUOTA_GITHUB_TOKEN` stays the highest-priority override.
- `github_token` in `%APPDATA%\TrafficMonitorGitHubCopilotQuota\config.json` remains a legacy fallback.
- The new app-managed token is stored in Windows Credential Manager and used between the environment variable and legacy plaintext config.

## User Experience

The GitHub Copilot plugin provides `ShowOptionsDialog(hParent)`.

The dialog shows:

- Authentication status: signed out, signed in, signing in, or error.
- Current account label when known.
- `Sign in with GitHub`.
- `Sign out`.

Clicking `Sign in with GitHub` starts GitHub OAuth Device Flow:

1. Request a device code from `https://github.com/login/device/code`.
2. Open GitHub's verification URL in the user's default browser.
3. Poll `https://github.com/login/oauth/access_token` at the returned interval until the user authorizes, denies, or the code expires.
4. Verify the token with `GET /user`.
5. Verify Copilot quota access with `GET /copilot_internal/user`.
6. Store the token in Windows Credential Manager with the account label on the credential.

The plugin never starts this flow automatically. It only runs after the user clicks the options button.

## Authentication Contract

Device flow uses the same public GitHub OAuth client and scope used by Win-CodexBar:

- Client ID: `Iv1.b507a08c87ecfe98`
- Scope: `read:user`

The quota request keeps the existing VS Code Copilot Chat-compatible headers.

Token lookup order:

1. `COPILOT_QUOTA_GITHUB_TOKEN`
2. Windows Credential Manager target `TrafficMonitorGitHubCopilotQuota:GitHubOAuth`
3. Legacy `github_token` in config JSON

The fetch result continues clearing `snapshot.config.github_token` so secrets are not retained in the snapshot.

## Storage

Credential Manager uses `CRED_TYPE_GENERIC` with local-machine persistence for the current Windows user.

Target name:

```text
TrafficMonitorGitHubCopilotQuota:GitHubOAuth
```

The credential blob contains the OAuth token as UTF-16 bytes without the trailing NUL. The user name/comment can contain the GitHub login label for the Windows Credential Manager UI.

Config JSON may store non-secret metadata:

```json
{
  "username": "octocat"
}
```

## Error Handling

- Missing token shows the existing missing-token error with the new sign-in option mentioned in docs and tooltip text.
- HTTP 401/403 still reports GitHub Copilot authentication failure.
- Device flow `authorization_pending` keeps waiting.
- Device flow `slow_down` increases the polling interval.
- Device flow `expired_token` and `access_denied` show stable non-secret errors.
- Stored tokens containing CR/LF are rejected before any request.

## Testing

Unit tests cover:

- Credential-token lookup precedence.
- Successful device-code response parsing.
- Successful access-token response parsing.
- Device flow OAuth errors.
- Sign-in orchestration stores the token only after identity and Copilot quota verification succeed.
- Sign-out deletes the stored credential.

Smoke tests cover:

- The Copilot plugin reports that options are provided.
- Opening the options dialog is not required in smoke tests because it is interactive.

Live tests stay opt-in behind `GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST=1`.
