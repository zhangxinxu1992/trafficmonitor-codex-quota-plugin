# GitHub Copilot Device Auth Design

> Agent design history: this file is preserved to document the implementation
> reasoning and evolution of the plugin. It is not the current product
> contract; use `README.md`, `docs/implementation-notes.md`, source, and tests
> as the current authority.
>
> Current behavior: GitHub sign-in is scoped to TrafficMonitor and stores the
> OAuth token in Windows Credential Manager. Plaintext `github_token` config
> fallback and GitHub token environment-variable overrides are not supported.

## Goal

Replace the GitHub Copilot quota plugin's manual-token-only setup with a user-initiated GitHub sign-in flow from the TrafficMonitor plugin options dialog.

The token paths are:

- The app-managed token is stored in Windows Credential Manager and is the preferred path.
- Plaintext `github_token` config fallback is not supported in the released plugin.
- GitHub token environment-variable overrides are not supported.

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

1. Windows Credential Manager target `TrafficMonitorGitHubCopilotQuota:GitHubOAuth`
2. No fallback token source; users sign in from TrafficMonitor plugin options

The fetch result does not retain raw token values in the snapshot.

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
