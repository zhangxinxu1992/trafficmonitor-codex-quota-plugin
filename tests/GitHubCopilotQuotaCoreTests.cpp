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
    const long long now = 1782086400;
    const long long reset = 1783123200;
    const auto quota = githubcopilotquota::CalculateQuota(1500.0, 270.0);

    Check(githubcopilotquota::FormatQuotaValue(quota, reset, now) == L" 82% 1.2kcr 12d", "quota value should include leading space and reset");
    Check(githubcopilotquota::FormatQuotaValue(quota, 0, now) == L" 82% 1.2kcr", "quota value should omit missing reset");
}

void TestCalculatesBillingPeriod()
{
    const long long now = 1782086400;
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
    const long long now = 1782086400;
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
