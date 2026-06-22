# GitHub Copilot Device Auth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add user-triggered GitHub sign-in for the TrafficMonitor GitHub Copilot quota plugin.

**Architecture:** Keep the quota parser and network transport in the existing Copilot core/fetch files. Add small auth helpers for device-flow JSON parsing, token lookup, Credential Manager storage, and an Options dialog that starts sign-in without blocking TrafficMonitor data refresh.

**Tech Stack:** C++17, WinHTTP, Win32 dialogs, Windows Credential Manager (`wincred.h` / `Advapi32.lib`), TrafficMonitor plugin API.

---

## File Structure

- Modify `src/GitHubCopilotQuotaCore.h/.cpp`: add device-flow response parsing and token-source helpers that are unit-testable without Windows Credential Manager.
- Modify `src/GitHubCopilotQuotaFetch.h/.cpp`: add Credential Manager token read/write/delete wrappers, device-flow HTTP request helpers, and update quota fetch token precedence.
- Modify `src/TrafficMonitorGitHubCopilotQuota.cpp`: implement `ShowOptionsDialog` and a small modal Win32 dialog with Sign in / Sign out actions.
- Modify `tests/GitHubCopilotQuotaCoreTests.cpp`: add TDD coverage for auth parsing, token precedence, and sign-in orchestration through fake transports/storage.
- Modify `tests/GitHubCopilotPluginSmokeTests.cpp`: verify options are now provided.
- Modify `TrafficMonitorGitHubCopilotQuota.vcxproj` and `GitHubCopilotQuotaTests.vcxproj`: link `Advapi32.lib`; add any new source/header files if the implementation is split.
- Modify `README.md` and `docs/implementation-notes.md`: document Options sign-in, credential storage, and fallback token behavior.

## Task 1: Token Source And Device Flow Parsing

- [ ] Write failing tests in `tests/GitHubCopilotQuotaCoreTests.cpp` for token precedence: env token, stored token, legacy config token, and missing token.
- [ ] Write failing tests for parsing GitHub device-code JSON including `verification_uri_complete`.
- [ ] Write failing tests for parsing access-token success JSON and OAuth error JSON.
- [ ] Implement minimal structs and helpers in `src/GitHubCopilotQuotaCore.h/.cpp`.
- [ ] Build and run `GitHubCopilotQuotaTests.exe`; confirm new tests pass.

## Task 2: Credential Manager Storage

- [ ] Write failing tests around storage abstraction by passing fake read/write/delete callbacks, not the real Windows credential store.
- [ ] Implement real `ReadStoredGitHubToken`, `WriteStoredGitHubToken`, and `DeleteStoredGitHubToken` using `CredReadW`, `CredWriteW`, `CredDeleteW`, and `CredFree`.
- [ ] Add `Advapi32.lib` to Copilot plugin and Copilot test projects.
- [ ] Build and run `GitHubCopilotQuotaTests.exe`.

## Task 3: Fetch Precedence And Sign-In Orchestration

- [ ] Write a failing test showing `FetchQuotaSnapshotFromConfigJson` uses the stored token when the environment variable is empty.
- [ ] Write a failing test showing env token overrides stored and config tokens.
- [ ] Write a failing test for sign-in orchestration: device-code request, token polling, `/user` verification, `/copilot_internal/user` verification, then token storage.
- [ ] Implement the smallest fetch/auth orchestration helpers needed by the tests.
- [ ] Build and run `GitHubCopilotQuotaTests.exe`.

## Task 4: Options Dialog

- [ ] Write a failing smoke test that `ShowOptionsDialog` returns something other than `OR_OPTION_NOT_PROVIDED` for the Copilot plugin.
- [ ] Implement a modal Win32 options dialog directly in `src/TrafficMonitorGitHubCopilotQuota.cpp`.
- [ ] The dialog starts sign-in only when the user clicks `Sign in with GitHub`.
- [ ] The dialog shows stable status text and never displays raw tokens.
- [ ] Build and run `GitHubCopilotPluginSmokeTests.exe`.

## Task 5: Documentation And Verification

- [ ] Update `README.md` with Options sign-in as the recommended path and environment variable as an override.
- [ ] Update `docs/implementation-notes.md` with Credential Manager target, token precedence, and live-test notes.
- [ ] Run the full Release x64 verification sequence:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\TrafficMonitorCodexQuota.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\CodexQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\PluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\TrafficMonitorGitHubCopilotQuota.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\GitHubCopilotQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\GitHubCopilotPluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe
.\build\x64\Release\GitHubCopilotQuotaTests.exe
.\build\x64\Release\GitHubCopilotPluginSmokeTests.exe
```

- [ ] If local GitHub credentials are available, run `GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST=1` and document the exact result.
