# TrafficMonitor Agent Quota Plugins

TrafficMonitor x64 plugins for showing Codex and GitHub Copilot quota in the
TrafficMonitor taskbar window.

This is an unofficial project. It is not affiliated with OpenAI, GitHub, or the
TrafficMonitor project.

## Plugins

| Plugin DLL | Shows | Setup |
| --- | --- | --- |
| `TrafficMonitorCodexQuota.dll` | `CX 5h:` and `CX 7d:` Codex quota windows | Sign in with the Codex CLI first. |
| `TrafficMonitorGitHubCopilotQuota.dll` | `GC:` GitHub Copilot quota | Open the plugin options and choose `Sign in with GitHub`. |

The default display uses remaining quota and a compact reset countdown. Plugin
options can switch the percentage to used quota and the reset suffix to local
reset time. The GitHub Copilot plugin can also show or hide remaining credits.

Example taskbar text:

```text
CX 5h: 69% 42m
CX 7d: 89% 6d 1h
GC: 82% 1.2kcr 12d
```

## Install

Download the latest release from
[GitHub Releases](https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins/releases).

1. Download `trafficmonitor-agent-quota-plugins-v<version>.zip`.
2. Extract the ZIP file.
3. Copy the DLL files you want to use into the `plugins` directory beside
   `TrafficMonitor.exe`, for example:

   ```text
   TrafficMonitor
   |-- TrafficMonitor.exe
   `-- plugins
       |-- TrafficMonitorCodexQuota.dll
       `-- TrafficMonitorGitHubCopilotQuota.dll
   ```

4. Restart TrafficMonitor.

TrafficMonitor's official plugin documentation gives the same installation
model: use a plugin DLL that matches the TrafficMonitor architecture, place it
in the `plugins` directory under the TrafficMonitor program directory, restart
TrafficMonitor, then check the plugin manager and enable the desired taskbar
display items. This project publishes x64 plugin DLLs.
See the official
[TrafficMonitor plugin features guide](https://github.com/zhongyang219/TrafficMonitor/wiki/%E6%8F%92%E4%BB%B6%E5%8A%9F%E8%83%BD)
or the [TrafficMonitor site](https://trafficmonitor.org/) for more details.

## Enable Display Items

After restarting TrafficMonitor:

1. Open TrafficMonitor's plugin manager and confirm the DLLs are loaded.
2. Open the taskbar window display settings.
3. Enable the items you want:
   - `CodexQuota5h`
   - `CodexQuotaWeek`
   - `GitHubCopilotQuotaAI`

You can install either DLL or both DLLs.

## Configure

For Codex quota, make sure Codex is already authenticated on the same Windows
user account. The plugin reads the local Codex auth file and refreshes quota in
the background.

For GitHub Copilot quota, open the TrafficMonitor plugin options dialog and use
`Sign in with GitHub`. The plugin stores the OAuth token in Windows Credential
Manager as a TrafficMonitor-scoped local credential.

Each plugin's options dialog controls whether to show remaining quota or used
quota, and whether reset information is shown as a countdown or a local time.

## Release Package

Each release ZIP contains:

```text
TrafficMonitorCodexQuota.dll
TrafficMonitorGitHubCopilotQuota.dll
README.md
LICENSE
THIRD_PARTY_NOTICES.md
```

Only the DLL files are required at runtime. The text files are included so the
downloaded package carries its license and usage notes.

## Development

Build, test, release, and implementation details are kept out of this README:

- [Development guide](docs/development.md)
- [Implementation notes](docs/implementation-notes.md)
- [Codex plugin design](docs/design.md)

## License

The plugin code is released under the MIT license. The copied TrafficMonitor
plugin interface header keeps its upstream copyright and license; see
`THIRD_PARTY_NOTICES.md`.
