# GitHub Copilot Quota Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a separate TrafficMonitor x64 plugin DLL that displays remaining monthly GitHub Copilot AI Credits as `GC:`.

**Architecture:** Keep the existing Codex plugin untouched and add a parallel GitHub Copilot plugin. Put calculation and parsing in a small testable core, put WinHTTP and local config/token loading in a fetch layer, and keep the TrafficMonitor DLL responsible only for cached display state and background refresh.

**Tech Stack:** C++17, WinHTTP, TrafficMonitor plugin API from `include/PluginInterface.h`, standalone MSBuild `.vcxproj` files, console test executables.

---

## File Structure

- Create: `src/GitHubCopilotQuotaCore.h`
  - Data models and pure functions: config parsing, usage JSON parsing, allowance resolution, billing period calculation, remaining quota math, credit/countdown formatting, GitHub API path building.
- Create: `src/GitHubCopilotQuotaCore.cpp`
  - Implementation of pure functions only. No WinHTTP, file I/O, environment reads, or TrafficMonitor types.
- Create: `src/GitHubCopilotQuotaFetch.h`
  - Fetch result model and public fetch entry point.
- Create: `src/GitHubCopilotQuotaFetch.cpp`
  - `%APPDATA%` config loading, `COPILOT_QUOTA_GITHUB_TOKEN` lookup, UTF-8 file read, WinHTTP requests to `api.github.com`, and live snapshot construction.
- Create: `src/TrafficMonitorGitHubCopilotQuota.cpp`
  - `ITMPlugin` implementation with one item: id `GitHubCopilotQuotaAI`, label `GC:`, value-leading space, tooltip, background refresh.
- Create: `tests/GitHubCopilotQuotaCoreTests.cpp`
  - Console tests for core behavior and gated live fetch.
- Create: `tests/GitHubCopilotPluginSmokeTests.cpp`
  - Console smoke test that loads `TrafficMonitorGitHubCopilotQuota.dll` and verifies plugin metadata.
- Create: `GitHubCopilotQuotaTests.vcxproj`
  - Builds the core/fetch test executable.
- Create: `GitHubCopilotPluginSmokeTests.vcxproj`
  - Builds the new plugin smoke test executable.
- Create: `TrafficMonitorGitHubCopilotQuota.vcxproj`
  - Builds `TrafficMonitorGitHubCopilotQuota.dll`.
- Modify: `README.md`
  - Add build/test/install commands for the new plugin and document the config file/token requirements.
- Modify: `docs/implementation-notes.md`
  - Add GitHub Copilot-specific notes, including `GC:` label cache and GBK config warning.

Do not edit existing Codex item ids, labels, refresh cadence, tests, or docs except for adding parallel GitHub Copilot instructions.

---

### Task 1: Core Tests And Test Project

**Files:**
- Create: `tests/GitHubCopilotQuotaCoreTests.cpp`
- Create: `GitHubCopilotQuotaTests.vcxproj`

- [ ] **Step 1: Write the failing core tests**

Create `tests/GitHubCopilotQuotaCoreTests.cpp` with these tests. The test names define the public core API that the next task must implement.

```cpp
#include "GitHubCopilotQuotaCore.h"
#include "GitHubCopilotQuotaFetch.h"

#include <Windows.h>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void CheckNear(double actual, double expected, const char* message)
{
    Check(std::fabs(actual - expected) < 0.0001, message);
}

void TestParsesConfigWithExplicitAllowance()
{
    std::wstring error;
    const auto config = githubcopilotquota::ParseConfigJson(
        LR"({
            "github_token": "config-token",
            "username": "octocat",
            "plan": "pro",
            "total_credits": 2345,
            "billing_day": 15
        })",
        error);

    Check(config.has_value(), "config with explicit allowance should parse");
    Check(config->github_token == L"config-token", "config token should parse");
    Check(config->username == L"octocat", "username should parse");
    Check(config->plan == L"pro", "plan should parse");
    CheckNear(config->total_credits, 2345.0, "total credits should parse");
    Check(config->has_billing_day, "billing day should be marked present");
    Check(config->billing_day == 15, "billing day should parse");
    Check(error.empty(), "successful config parse should not set error");
}

void TestResolvesAllowanceFromPlan()
{
    githubcopilotquota::PluginConfig config;
    config.plan = L"pro_plus";

    std::wstring error;
    const auto allowance = githubcopilotquota::ResolveAllowance(config, error);

    Check(allowance.has_value(), "known plan should resolve allowance");
    CheckNear(allowance->total_credits, 7000.0, "pro_plus allowance should be 7000");
    Check(allowance->source == L"plan:pro_plus", "allowance source should name plan");
    Check(error.empty(), "successful allowance resolution should not set error");
}

void TestRejectsMissingAllowance()
{
    githubcopilotquota::PluginConfig config;

    std::wstring error;
    const auto allowance = githubcopilotquota::ResolveAllowance(config, error);

    Check(!allowance.has_value(), "missing allowance should fail");
    Check(error.find(L"plan") != std::wstring::npos, "missing allowance error should mention plan");
    Check(error.find(L"total_credits") != std::wstring::npos, "missing allowance error should mention total_credits");
}

void TestParsesUsageReport()
{
    std::wstring error;
    const auto usage = githubcopilotquota::ParseUsageJson(
        R"({
            "timePeriod": { "year": 2026, "month": 6 },
            "user": "octocat",
            "usageItems": [
                { "product": "Copilot AI Credits", "sku": "AI Credit", "unitType": "ai-credits", "netQuantity": 12.5 },
                { "product": "Copilot AI Credits", "sku": "AI Credit", "unitType": "ai-credits", "netQuantity": "7.25" },
                { "product": "Other", "sku": "Ignored", "unitType": "units", "netQuantity": 1000 }
            ]
        })",
        error);

    Check(usage.has_value(), "usage JSON should parse");
    Check(usage->user == L"octocat", "usage user should parse");
    CheckNear(usage->consumed_credits, 19.75, "only Copilot AI credit netQuantity values should be summed");
    Check(error.empty(), "successful usage parse should not set error");
}

void TestParsesUserLogin()
{
    std::wstring error;
    const auto login = githubcopilotquota::ParseAuthenticatedUserJson(R"({"login":"octocat"})", error);

    Check(login.has_value(), "authenticated user JSON should parse");
    Check(*login == L"octocat", "login should parse");
}

void TestFormatsCreditCounts()
{
    Check(githubcopilotquota::FormatCreditCount(0) == L"0cr", "zero credits should format");
    Check(githubcopilotquota::FormatCreditCount(950) == L"950cr", "under 1000 credits should use cr");
    Check(githubcopilotquota::FormatCreditCount(1200) == L"1.2kcr", "1000+ credits should use kcr");
    Check(githubcopilotquota::FormatCreditCount(20000) == L"20.0kcr", "large credits should keep one decimal");
}

void TestCalculatesRemainingQuota()
{
    const auto quota = githubcopilotquota::CalculateQuota(1500.0, 270.0);

    CheckNear(quota.total_credits, 1500.0, "quota should keep total");
    CheckNear(quota.consumed_credits, 270.0, "quota should keep consumed");
    CheckNear(quota.remaining_credits, 1230.0, "quota should subtract consumed from total");
    CheckNear(quota.remaining_percent, 82.0, "quota remaining percent should calculate");
}

void TestClampsOverage()
{
    const auto quota = githubcopilotquota::CalculateQuota(1500.0, 1700.0);

    CheckNear(quota.remaining_credits, 0.0, "overage remaining credits should clamp to zero");
    CheckNear(quota.remaining_percent, 0.0, "overage remaining percent should clamp to zero");
}

void TestFormatsQuotaValue()
{
    const long long now = 1782086400;      // 2026-06-22T00:00:00Z
    const long long reset = 1783123200;    // 2026-07-04T00:00:00Z
    const auto quota = githubcopilotquota::CalculateQuota(1500.0, 270.0);

    Check(githubcopilotquota::FormatQuotaValue(quota, reset, now) == L" 82% 1.2kcr 12d", "quota value should include leading space and reset");
    Check(githubcopilotquota::FormatQuotaValue(quota, 0, now) == L" 82% 1.2kcr", "quota value should omit missing reset");
}

void TestCalculatesBillingPeriod()
{
    const long long now = 1782086400; // 2026-06-22T00:00:00Z
    const auto period = githubcopilotquota::CalculateBillingPeriod(15, now);

    Check(!period.is_calendar_month_estimate, "configured billing day should be exact");
    Check(period.start.year == 2026 && period.start.month == 6 && period.start.day == 15, "billing period start should be current cycle start");
    Check(period.end.year == 2026 && period.end.month == 7 && period.end.day == 15, "billing period end should be next cycle start");
    Check(period.reset_at == 1784073600, "reset timestamp should be next billing day UTC");
    Check(!period.usage_dates.empty(), "billing period should include usage dates");
    Check(period.usage_dates.front().day == 15, "first usage date should be start day");
}

void TestCalculatesCalendarMonthEstimate()
{
    const long long now = 1782086400; // 2026-06-22T00:00:00Z
    const auto period = githubcopilotquota::CalculateCalendarMonthEstimate(now);

    Check(period.is_calendar_month_estimate, "missing billing day should be estimate");
    Check(period.start.year == 2026 && period.start.month == 6 && period.start.day == 1, "calendar estimate should start on first day");
    Check(period.reset_at == 0, "calendar estimate should not claim a reset timestamp");
}

void TestBuildsUsagePaths()
{
    const githubcopilotquota::Date date{2026, 6, 22};

    Check(githubcopilotquota::BuildUsagePath(L"octocat", date) == L"/users/octocat/settings/billing/ai_credit/usage?year=2026&month=6&day=22", "daily usage path should include year month day");
    Check(githubcopilotquota::BuildMonthlyUsagePath(L"octocat", 2026, 6) == L"/users/octocat/settings/billing/ai_credit/usage?year=2026&month=6", "monthly usage path should include year month");
}

void TestLiveFetchWhenRequested()
{
    wchar_t flag[8]{};
    if (GetEnvironmentVariableW(L"GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST", flag, 8) == 0)
    {
        return;
    }

    const auto result = githubcopilotquota::FetchQuotaSnapshot();
    if (!result.success)
    {
        std::wcerr << L"LIVE ERROR: " << result.error << L" status=" << result.http_status << L'\n';
    }
    Check(result.success, "live GitHub Copilot quota fetch should succeed when requested");
    Check(result.snapshot.allowance.total_credits > 0.0, "live snapshot should include allowance");
    Check(result.snapshot.quota.total_credits > 0.0, "live snapshot should include quota total");
}
}

int main()
{
    TestParsesConfigWithExplicitAllowance();
    TestResolvesAllowanceFromPlan();
    TestRejectsMissingAllowance();
    TestParsesUsageReport();
    TestParsesUserLogin();
    TestFormatsCreditCounts();
    TestCalculatesRemainingQuota();
    TestClampsOverage();
    TestFormatsQuotaValue();
    TestCalculatesBillingPeriod();
    TestCalculatesCalendarMonthEstimate();
    TestBuildsUsagePaths();
    TestLiveFetchWhenRequested();

    if (failures != 0)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All GitHub Copilot quota tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Create the test project**

Create `GitHubCopilotQuotaTests.vcxproj` by copying the structure of `CodexQuotaTests.vcxproj` and changing these values:

```xml
<ProjectGuid>{8E3DA4E9-5509-48C7-94EE-C336EF2E10AB}</ProjectGuid>
<RootNamespace>GitHubCopilotQuotaTests</RootNamespace>
```

Use these compile items:

```xml
<ItemGroup>
  <ClCompile Include="src\GitHubCopilotQuotaCore.cpp" />
  <ClCompile Include="src\GitHubCopilotQuotaFetch.cpp" />
  <ClCompile Include="tests\GitHubCopilotQuotaCoreTests.cpp" />
</ItemGroup>
<ItemGroup>
  <ClInclude Include="src\GitHubCopilotQuotaCore.h" />
  <ClInclude Include="src\GitHubCopilotQuotaFetch.h" />
</ItemGroup>
```

Keep `winhttp.lib`, `stdcpp17`, `WarningLevel` Level4, `SDLCheck`, and the existing output directories.

- [ ] **Step 3: Run the tests to verify RED**

Run:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\GitHubCopilotQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Expected: build fails because `src\GitHubCopilotQuotaCore.h` and `src\GitHubCopilotQuotaFetch.h` do not exist.

- [ ] **Step 4: Commit the failing tests**

```powershell
git add .\tests\GitHubCopilotQuotaCoreTests.cpp .\GitHubCopilotQuotaTests.vcxproj
git commit -m "test: add GitHub Copilot quota core tests"
```

---

### Task 2: Core Models, Parsing, Periods, And Formatting

**Files:**
- Create: `src/GitHubCopilotQuotaCore.h`
- Create: `src/GitHubCopilotQuotaCore.cpp`
- Modify: `GitHubCopilotQuotaTests.vcxproj`

- [ ] **Step 1: Create the core header**

Create `src/GitHubCopilotQuotaCore.h` with this public API:

```cpp
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace githubcopilotquota
{
struct PluginConfig
{
    std::wstring github_token;
    std::wstring username;
    std::wstring plan;
    double total_credits{};
    int billing_day{};
    bool has_billing_day{};
};

struct Allowance
{
    double total_credits{};
    std::wstring source;
};

struct UsageReport
{
    std::wstring user;
    double consumed_credits{};
};

struct Date
{
    int year{};
    int month{};
    int day{};
};

struct UsagePeriod
{
    Date start;
    Date end;
    std::vector<Date> usage_dates;
    long long reset_at{};
    bool is_calendar_month_estimate{};
};

struct Quota
{
    double total_credits{};
    double consumed_credits{};
    double remaining_credits{};
    double remaining_percent{};
};

std::optional<PluginConfig> ParseConfigJson(const std::wstring& json, std::wstring& error);
std::optional<Allowance> ResolveAllowance(const PluginConfig& config, std::wstring& error);
std::optional<UsageReport> ParseUsageJson(const std::string& json, std::wstring& error);
std::optional<std::wstring> ParseAuthenticatedUserJson(const std::string& json, std::wstring& error);

Quota CalculateQuota(double total_credits, double consumed_credits);
UsagePeriod CalculateBillingPeriod(int billing_day, long long now);
UsagePeriod CalculateCalendarMonthEstimate(long long now);

std::wstring BuildUsagePath(const std::wstring& username, const Date& date);
std::wstring BuildMonthlyUsagePath(const std::wstring& username, int year, int month);

std::wstring FormatCreditCount(double credits);
std::wstring FormatPercent(double percent);
std::wstring FormatResetCountdown(long long reset_at, long long now);
std::wstring FormatQuotaValue(const Quota& quota, long long reset_at, long long now);
}
```

- [ ] **Step 2: Create the core implementation**

Create `src/GitHubCopilotQuotaCore.cpp`. Reuse the small string-scanning JSON helpers from `src/CodexQuotaCore.cpp`, but keep them inside this new file's anonymous namespace so the Codex plugin is unchanged. Implement:

```cpp
namespace githubcopilotquota
{
std::optional<Allowance> ResolveAllowance(const PluginConfig& config, std::wstring& error)
{
    error.clear();
    if (config.total_credits > 0.0)
    {
        return Allowance{config.total_credits, L"total_credits"};
    }
    if (config.plan == L"pro")
    {
        return Allowance{1500.0, L"plan:pro"};
    }
    if (config.plan == L"pro_plus")
    {
        return Allowance{7000.0, L"plan:pro_plus"};
    }
    if (config.plan == L"max")
    {
        return Allowance{20000.0, L"plan:max"};
    }

    error = L"GitHub Copilot quota config requires total_credits or a known plan.";
    return std::nullopt;
}

Quota CalculateQuota(double total_credits, double consumed_credits)
{
    if (total_credits < 0.0)
    {
        total_credits = 0.0;
    }
    if (consumed_credits < 0.0)
    {
        consumed_credits = 0.0;
    }

    Quota quota;
    quota.total_credits = total_credits;
    quota.consumed_credits = consumed_credits;
    quota.remaining_credits = total_credits - consumed_credits;
    if (quota.remaining_credits < 0.0)
    {
        quota.remaining_credits = 0.0;
    }
    quota.remaining_percent = total_credits <= 0.0 ? 0.0 : (quota.remaining_credits * 100.0 / total_credits);
    return quota;
}
}
```

Implement the remaining functions to satisfy the test expectations exactly:

- `ParseConfigJson` reads string keys `github_token`, `username`, `plan`, numeric key `total_credits`, numeric key `billing_day`, and treats `billing_day` as present only when it is between 1 and 31.
- `ParseUsageJson` requires `usageItems`, sums `netQuantity` only for items where `product` contains `Copilot AI Credits` or `sku` contains `AI Credit`, and accepts numeric or quoted numeric values.
- `ParseAuthenticatedUserJson` returns `login` or error `GitHub user response does not contain login.`.
- `FormatPercent` rounds half-up and clamps negative/NaN to zero.
- `FormatCreditCount` rounds to whole `cr` under 1000 and uses one decimal `kcr` for 1000 or greater.
- `FormatResetCountdown` matches `codexquota::FormatResetCountdown`.
- `FormatQuotaValue` returns a value-leading-space string.
- `BuildUsagePath` and `BuildMonthlyUsagePath` use decimal month/day values without zero padding.
- Date calculations use UTC. On Windows, use `_mkgmtime` for UTC timestamps.

- [ ] **Step 3: Run the tests to verify GREEN**

Run:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\GitHubCopilotQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
.\build\x64\Release\GitHubCopilotQuotaTests.exe
```

Expected:

```text
All GitHub Copilot quota tests passed
```

- [ ] **Step 4: Commit**

```powershell
git add .\src\GitHubCopilotQuotaCore.h .\src\GitHubCopilotQuotaCore.cpp .\GitHubCopilotQuotaTests.vcxproj
git commit -m "feat: add GitHub Copilot quota core"
```

---

### Task 3: Fetch Layer And Live Snapshot Construction

**Files:**
- Create: `src/GitHubCopilotQuotaFetch.h`
- Create: `src/GitHubCopilotQuotaFetch.cpp`
- Modify: `tests/GitHubCopilotQuotaCoreTests.cpp`

- [ ] **Step 1: Write the fetch API header**

Create `src/GitHubCopilotQuotaFetch.h`:

```cpp
#pragma once

#include "GitHubCopilotQuotaCore.h"

#include <string>

namespace githubcopilotquota
{
struct QuotaSnapshot
{
    PluginConfig config;
    Allowance allowance;
    UsagePeriod period;
    UsageReport usage;
    Quota quota;
    std::wstring username;
};

struct FetchResult
{
    bool success{};
    QuotaSnapshot snapshot;
    std::wstring error;
    int http_status{};
};

std::wstring GetDefaultConfigPath();
FetchResult FetchQuotaSnapshot();
}
```

- [ ] **Step 2: Run tests to verify RED**

Run:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\GitHubCopilotQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Expected: build fails because `src\GitHubCopilotQuotaFetch.cpp` is still missing or `FetchQuotaSnapshot` is unresolved.

- [ ] **Step 3: Implement the fetch layer**

Create `src/GitHubCopilotQuotaFetch.cpp`. Copy the RAII `HttpHandle`, `WindowsErrorMessage`, `GetEnvVar`, UTF-8 file reader, WinHTTP status/body helpers, and HTTPS request pattern from `src/CodexQuotaFetch.cpp` into this new file. Keep names in the anonymous namespace.

Implement these concrete behaviors:

```cpp
std::wstring GetDefaultConfigPath()
{
    const auto app_data = GetEnvVar(L"APPDATA");
    if (!app_data.empty())
    {
        return JoinPath(JoinPath(app_data, L"TrafficMonitorGitHubCopilotQuota"), L"config.json");
    }
    return L"TrafficMonitorGitHubCopilotQuota\\config.json";
}
```

`FetchQuotaSnapshot()` flow:

1. Read config JSON from `GetDefaultConfigPath()` if the file exists. Missing config file is allowed only when `COPILOT_QUOTA_GITHUB_TOKEN` and a valid allowance source are otherwise available; because allowance is config-backed in v1, missing config should return `GitHub Copilot quota config not found: <path>`.
2. Parse config with `ParseConfigJson`.
3. Read token from `COPILOT_QUOTA_GITHUB_TOKEN`; if empty, use `config.github_token`.
4. If token is empty, return `Missing GitHub token. Set COPILOT_QUOTA_GITHUB_TOKEN or github_token in config.json.`.
5. Resolve allowance with `ResolveAllowance`.
6. If `config.username` is empty, call `/user` and parse `login`.
7. If `config.has_billing_day`, calculate `CalculateBillingPeriod(config.billing_day, std::time(nullptr))` and fetch one daily usage report for each `period.usage_dates`.
8. If no billing day is configured, calculate `CalculateCalendarMonthEstimate(std::time(nullptr))` and fetch one monthly usage report.
9. Sum all fetched `UsageReport::consumed_credits`.
10. Fill `QuotaSnapshot` and return `success=true`.

All GitHub requests must use:

```text
Accept: application/vnd.github+json
Authorization: Bearer <token>
X-GitHub-Api-Version: 2026-03-10
User-Agent: TrafficMonitorGitHubCopilotQuota/1.0
```

For `401` or `403`, return:

```text
GitHub authentication failed or token lacks Plan read permission.
```

For other non-2xx statuses, return:

```text
GitHub API returned HTTP <status>.
```

- [ ] **Step 4: Run unit tests without live flag**

Run:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\GitHubCopilotQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
.\build\x64\Release\GitHubCopilotQuotaTests.exe
```

Expected:

```text
All GitHub Copilot quota tests passed
```

- [ ] **Step 5: Run gated live test when credentials are configured**

Create `%APPDATA%\TrafficMonitorGitHubCopilotQuota\config.json` on the test machine with the user's real values:

```json
{
  "username": "YOUR_GITHUB_LOGIN",
  "plan": "pro",
  "billing_day": 1
}
```

Then run:

```powershell
$env:COPILOT_QUOTA_GITHUB_TOKEN = '<token-with-plan-read>'
$env:GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\GitHubCopilotQuotaTests.exe
```

Expected:

```text
All GitHub Copilot quota tests passed
```

If the live test fails with `GitHub authentication failed or token lacks Plan read permission.`, fix the token permission before continuing.

- [ ] **Step 6: Commit**

```powershell
git add .\src\GitHubCopilotQuotaFetch.h .\src\GitHubCopilotQuotaFetch.cpp .\tests\GitHubCopilotQuotaCoreTests.cpp
git commit -m "feat: fetch GitHub Copilot AI credit usage"
```

---

### Task 4: Plugin DLL And Smoke Test

**Files:**
- Create: `src/TrafficMonitorGitHubCopilotQuota.cpp`
- Create: `TrafficMonitorGitHubCopilotQuota.vcxproj`
- Create: `tests/GitHubCopilotPluginSmokeTests.cpp`
- Create: `GitHubCopilotPluginSmokeTests.vcxproj`

- [ ] **Step 1: Write the smoke test first**

Create `tests/GitHubCopilotPluginSmokeTests.cpp`:

```cpp
#include "PluginInterface.h"

#include <Windows.h>
#include <iostream>
#include <string>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::wstring CurrentExeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value(path);
    const auto slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

bool StartsWith(const std::wstring& value, const wchar_t* prefix)
{
    const std::wstring prefix_value(prefix);
    return value.size() >= prefix_value.size()
        && value.compare(0, prefix_value.size(), prefix_value) == 0;
}
}

int main()
{
    const auto dll_path = CurrentExeDir() + L"\\TrafficMonitorGitHubCopilotQuota.dll";
    HMODULE module = LoadLibraryW(dll_path.c_str());
    if (module == nullptr)
    {
        std::wcerr << L"FAIL: LoadLibrary failed for " << dll_path << L" error " << GetLastError() << L'\n';
        return 1;
    }

    auto get_instance = reinterpret_cast<ITMPlugin* (*)()>(GetProcAddress(module, "TMPluginGetInstance"));
    Check(get_instance != nullptr, "TMPluginGetInstance should be exported");
    ITMPlugin* plugin = get_instance == nullptr ? nullptr : get_instance();
    Check(plugin != nullptr, "TMPluginGetInstance should return a plugin");

    if (plugin != nullptr)
    {
        Check(plugin->GetAPIVersion() >= 7, "plugin API version should support OnInitialize");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_NAME)) == L"GitHub Copilot Quota", "plugin name should match");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_DESCRIPTION)) == L"Displays remaining GitHub Copilot monthly AI Credits.", "plugin description should match");

        IPluginItem* item = plugin->GetItem(0);
        Check(item != nullptr, "first item should exist");
        Check(plugin->GetItem(1) == nullptr, "second item should not exist");

        if (item != nullptr)
        {
            Check(std::wstring(item->GetItemId()) == L"GitHubCopilotQuotaAI", "item id should match");
            Check(std::wstring(item->GetItemName()) == L"GitHub Copilot AI Credits", "item name should match");
            Check(std::wstring(item->GetItemLableText()) == L"GC:", "label should be GC:");
            Check(std::wstring(item->GetItemValueSampleText()) == L" 100% 20.0kcr 31d", "sample should reserve max width");
            Check(std::wstring(item->GetItemValueText()) == L" ...", "initial value should include visible spacing before loading");
            Check(StartsWith(item->GetItemValueText(), L" "), "value should start with visible spacing");
        }
    }

    FreeLibrary(module);

    if (failures != 0)
    {
        std::cerr << failures << " smoke test(s) failed\n";
        return 1;
    }

    std::cout << "GitHub Copilot plugin smoke tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Create smoke test project**

Create `GitHubCopilotPluginSmokeTests.vcxproj` from `PluginSmokeTests.vcxproj` and change:

```xml
<ProjectGuid>{D15DA76A-83CB-4062-A99A-12685F01A1FE}</ProjectGuid>
<RootNamespace>GitHubCopilotPluginSmokeTests</RootNamespace>
```

Use:

```xml
<ItemGroup>
  <ClCompile Include="tests\GitHubCopilotPluginSmokeTests.cpp" />
</ItemGroup>
<ItemGroup>
  <ClInclude Include="include\PluginInterface.h" />
</ItemGroup>
```

- [ ] **Step 3: Create plugin project**

Create `TrafficMonitorGitHubCopilotQuota.vcxproj` from `TrafficMonitorCodexQuota.vcxproj` and change:

```xml
<ProjectGuid>{D4F65011-5C85-41FC-A32D-8910B22F59A8}</ProjectGuid>
<RootNamespace>TrafficMonitorGitHubCopilotQuota</RootNamespace>
<TargetName>TrafficMonitorGitHubCopilotQuota</TargetName>
```

Use:

```xml
<ItemGroup>
  <ClCompile Include="src\GitHubCopilotQuotaCore.cpp" />
  <ClCompile Include="src\GitHubCopilotQuotaFetch.cpp" />
  <ClCompile Include="src\TrafficMonitorGitHubCopilotQuota.cpp" />
</ItemGroup>
<ItemGroup>
  <ClInclude Include="include\PluginInterface.h" />
  <ClInclude Include="src\GitHubCopilotQuotaCore.h" />
  <ClInclude Include="src\GitHubCopilotQuotaFetch.h" />
</ItemGroup>
```

Keep `winhttp.lib`, `stdcpp17`, `WarningLevel` Level4, and x64 Debug/Release configurations.

- [ ] **Step 4: Run smoke test to verify RED**

Run:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\GitHubCopilotPluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
.\build\x64\Release\GitHubCopilotPluginSmokeTests.exe
```

Expected: smoke test fails because `TrafficMonitorGitHubCopilotQuota.dll` does not exist yet.

- [ ] **Step 5: Implement the TrafficMonitor plugin**

Create `src/TrafficMonitorGitHubCopilotQuota.cpp` by following `src/TrafficMonitorCodexQuota.cpp` structure with these concrete differences:

```cpp
class GitHubCopilotQuotaItem final : public IPluginItem
{
public:
    const wchar_t* GetItemName() const override
    {
        return L"GitHub Copilot AI Credits";
    }

    const wchar_t* GetItemId() const override
    {
        return L"GitHubCopilotQuotaAI";
    }

    const wchar_t* GetItemLableText() const override
    {
        return L"GC:";
    }

    const wchar_t* GetItemValueText() const override;

    const wchar_t* GetItemValueSampleText() const override
    {
        return L" 100% 20.0kcr 31d";
    }

private:
    mutable std::wstring m_value;
};
```

`GitHubCopilotQuotaPlugin::GetInfo` returns:

```cpp
case TMI_NAME:
    return L"GitHub Copilot Quota";
case TMI_DESCRIPTION:
    return L"Displays remaining GitHub Copilot monthly AI Credits.";
case TMI_AUTHOR:
    return L"OpenAI Codex";
case TMI_COPYRIGHT:
    return L"MIT";
case TMI_VERSION:
    return L"1.0.0";
case TMI_URL:
    return L"";
```

`GetItem(0)` returns the single item and `GetItem(1)` returns `nullptr`.

`ValueText()` returns:

```cpp
if (m_has_snapshot)
{
    return githubcopilotquota::FormatQuotaValue(
        m_snapshot.quota,
        m_snapshot.period.reset_at,
        std::time(nullptr));
}

if (m_last_error.empty())
{
    return L" ...";
}
return L" ERR";
```

`ApplyFetchResult()` uses these refresh cadences:

```cpp
if (result.success)
{
    m_snapshot = result.snapshot;
    m_has_snapshot = true;
    m_last_error.clear();
    m_next_refresh = std::chrono::steady_clock::now() + std::chrono::minutes(15);
}
else
{
    m_last_error = result.error.empty() ? L"Unknown GitHub Copilot quota error." : result.error;
    m_next_refresh = std::chrono::steady_clock::now() + std::chrono::minutes(2);
}
```

`BuildTooltipLocked()` includes these lines when a snapshot is available:

```text
GitHub Copilot quota
User: <username>
Allowance: <source> (<total>cr)
Consumed: <consumed>cr
Remaining: <remaining>cr (<percent>%)
Reset: <countdown>
Period: configured billing cycle
Last refresh: OK
```

When `period.is_calendar_month_estimate` is true, use:

```text
Reset: not configured
Period: current calendar month estimate
```

When no snapshot exists, show `Waiting for first refresh.` or `Refreshing...` like the Codex plugin.

- [ ] **Step 6: Run plugin build and smoke test to verify GREEN**

Run:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\TrafficMonitorGitHubCopilotQuota.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\GitHubCopilotPluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
.\build\x64\Release\GitHubCopilotPluginSmokeTests.exe
```

Expected:

```text
GitHub Copilot plugin smoke tests passed
```

- [ ] **Step 7: Commit**

```powershell
git add .\src\TrafficMonitorGitHubCopilotQuota.cpp .\TrafficMonitorGitHubCopilotQuota.vcxproj .\tests\GitHubCopilotPluginSmokeTests.cpp .\GitHubCopilotPluginSmokeTests.vcxproj
git commit -m "feat: add GitHub Copilot TrafficMonitor plugin"
```

---

### Task 5: Documentation And Install Instructions

**Files:**
- Modify: `README.md`
- Modify: `docs/implementation-notes.md`

- [ ] **Step 1: Write documentation changes**

In `README.md`, add a `GitHub Copilot Quota Plugin` section after the Codex display item section:

````markdown
## GitHub Copilot Quota Plugin

This repository also builds `TrafficMonitorGitHubCopilotQuota.dll`, a separate TrafficMonitor x64 plugin that displays remaining GitHub Copilot monthly AI Credits.

Display item:

- `GC:`: remaining monthly GitHub Copilot AI Credits percentage, compact remaining-credit count, and optional reset countdown.

Example taskbar values:

- `GC: 82% 1.2kcr 12d`
- `GC: 100% 1500cr`

The value includes a leading space for the same TrafficMonitor label-trimming reason as the Codex items.

### GitHub Copilot Configuration

Create `%APPDATA%\TrafficMonitorGitHubCopilotQuota\config.json`:

```json
{
  "username": "YOUR_GITHUB_LOGIN",
  "plan": "pro",
  "billing_day": 1
}
```

Set `COPILOT_QUOTA_GITHUB_TOKEN` to a GitHub token with user `Plan` read permission. If the environment variable is not set, the plugin can read `github_token` from `config.json`, but that stores the token as plain text.

Use `total_credits` when the plan allowance is custom:

```json
{
  "username": "YOUR_GITHUB_LOGIN",
  "total_credits": 1500,
  "billing_day": 1
}
```
````

In the build and test sections, add:

```powershell
& $msbuild .\TrafficMonitorGitHubCopilotQuota.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\GitHubCopilotQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\GitHubCopilotPluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
.\build\x64\Release\GitHubCopilotQuotaTests.exe
.\build\x64\Release\GitHubCopilotPluginSmokeTests.exe

$env:GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\GitHubCopilotQuotaTests.exe
```

In the install section, add:

```powershell
Copy-Item -Force '.\build\x64\Release\TrafficMonitorGitHubCopilotQuota.dll' 'C:\Apps\TrafficMonitor\plugins\TrafficMonitorGitHubCopilotQuota.dll'
```

In `docs/implementation-notes.md`, add a `GitHub Copilot Quota Plugin` section with:

````markdown
## GitHub Copilot Quota Plugin

The GitHub Copilot plugin is separate from the Codex plugin and exposes one TrafficMonitor item:

- `GitHubCopilotQuotaAI` with label `GC:`

The plugin uses GitHub's official billing usage API for AI Credits:

- `GET https://api.github.com/user`
- `GET https://api.github.com/users/{username}/settings/billing/ai_credit/usage`

The token comes from `COPILOT_QUOTA_GITHUB_TOKEN` first, then from `%APPDATA%\TrafficMonitorGitHubCopilotQuota\config.json` as `github_token`.

The API reports consumed credits. The plugin resolves the allowance from `total_credits` first, then from `plan`:

- `pro`: 1500
- `pro_plus`: 7000
- `max`: 20000

`billing_day` is needed for exact billing-cycle usage and reset countdown. Without it, the plugin displays a current-calendar-month estimate and omits the reset countdown.

If TrafficMonitor caches the label, keep:

```ini
GitHubCopilotQuotaAI = GC:
```

Preserve `C:\Apps\TrafficMonitor\config.ini` as GBK if editing it.
````

- [ ] **Step 2: Verify docs contain no misleading Codex changes**

Run:

```powershell
git diff -- README.md docs/implementation-notes.md
```

Expected: diff only adds GitHub Copilot sections and does not alter existing Codex display rules.

- [ ] **Step 3: Commit**

```powershell
git add .\README.md .\docs\implementation-notes.md
git commit -m "docs: document GitHub Copilot quota plugin"
```

---

### Task 6: Full Verification

**Files:**
- No source edits unless verification exposes a failure.

- [ ] **Step 1: Build all Release x64 projects**

Run:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\TrafficMonitorCodexQuota.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\CodexQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\PluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\TrafficMonitorGitHubCopilotQuota.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\GitHubCopilotQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\GitHubCopilotPluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Expected: each MSBuild command exits `0`.

- [ ] **Step 2: Run non-live tests**

Run:

```powershell
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe
.\build\x64\Release\GitHubCopilotQuotaTests.exe
.\build\x64\Release\GitHubCopilotPluginSmokeTests.exe
```

Expected output includes:

```text
All tests passed
Plugin smoke tests passed
All GitHub Copilot quota tests passed
GitHub Copilot plugin smoke tests passed
```

- [ ] **Step 3: Run live tests when credentials are available**

Run:

```powershell
$env:CODEX_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe

$env:GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\GitHubCopilotQuotaTests.exe
```

Expected: all live tests pass. If GitHub live tests fail due token permissions, document the exact error in the final response and do not claim live verification passed.

- [ ] **Step 4: Check git state**

Run:

```powershell
git status --short
git log --oneline -5
```

Expected: `git status --short` is empty after all commits. The latest commits correspond to the tasks above.

---

## Self-Review Checklist

- Spec coverage:
  - Separate DLL: Task 4.
  - One item with `GC:` label: Task 4 smoke test and plugin.
  - Leading-space value convention: Task 1 formatting test and Task 4 smoke test.
  - GitHub AI Credits API: Task 3.
  - Token/config lookup: Task 3 and Task 5.
  - Allowance from `total_credits` or `plan`: Task 1 and Task 2.
  - Billing day exact cycle and calendar-month estimate: Task 1 and Task 2.
  - Tooltip contents and errors: Task 4.
  - Release x64 verification: Task 6.
- Placeholder scan:
  - The plan contains no incomplete-work markers or unspecified edge-handling steps.
- Type consistency:
  - Namespace is consistently `githubcopilotquota`.
  - Item id is consistently `GitHubCopilotQuotaAI`.
  - DLL name is consistently `TrafficMonitorGitHubCopilotQuota.dll`.
  - Test executable names are consistently `GitHubCopilotQuotaTests.exe` and `GitHubCopilotPluginSmokeTests.exe`.
