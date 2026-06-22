# Agent Design History

This directory preserves agent-generated design notes and implementation plans.
They are kept as project history: they show how the plugins were designed,
reviewed, and changed during development.

These files are not the current product contract. When a historical plan
conflicts with the current code or user documentation, use these files instead:

- `README.md`
- `docs/design.md`
- `docs/implementation-notes.md`
- the source and tests in `src/` and `tests/`

Some early GitHub Copilot quota notes describe a superseded GitHub AI Credits
billing API approach and a plaintext `github_token` fallback. The released
plugin now uses GitHub's Copilot internal quota endpoint and TrafficMonitor
plugin options sign-in backed by Windows Credential Manager.
