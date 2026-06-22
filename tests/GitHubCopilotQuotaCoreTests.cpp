#include "GitHubCopilotQuotaCore.h"
#include "GitHubCopilotQuotaFetch.h"

#include <Windows.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

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

bool IsOnOrBefore(const githubcopilotquota::Date& actual, const githubcopilotquota::Date& expected)
{
    if (actual.year != expected.year)
    {
        return actual.year < expected.year;
    }
    if (actual.month != expected.month)
    {
        return actual.month < expected.month;
    }
    return actual.day <= expected.day;
}

struct FakeGitHubTransport
{
    std::vector<githubcopilotquota::GitHubHttpRequest> requests;
    std::vector<githubcopilotquota::GitHubHttpResponse> responses;
};

bool FakeGitHubRequest(
    const githubcopilotquota::GitHubHttpRequest& request,
    githubcopilotquota::GitHubHttpResponse& response,
    std::wstring& error,
    void* context)
{
    auto* fake = static_cast<FakeGitHubTransport*>(context);
    fake->requests.push_back(request);

    const auto response_index = fake->requests.size() - 1;
    if (response_index >= fake->responses.size())
    {
        error = L"Unexpected GitHub request: " + request.path;
        return false;
    }

    response = fake->responses[response_index];
    return true;
}

githubcopilotquota::GitHubHttpResponse FakeUsageResponse(double credits)
{
    return githubcopilotquota::GitHubHttpResponse{
        200,
        R"({"user":"octocat","usageItems":[{"product":"Copilot AI Credits","sku":"AI Credit","netQuantity":")"
            + std::to_string(credits)
            + R"("}]})"};
}

std::vector<githubcopilotquota::GitHubHttpResponse> FakeUsageResponses(int count, double credits)
{
    std::vector<githubcopilotquota::GitHubHttpResponse> responses;
    for (auto index = 0; index < count; ++index)
    {
        responses.push_back(FakeUsageResponse(credits));
    }
    return responses;
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

void TestRejectsMalformedConfigTotalCredits()
{
    std::wstring error;
    const auto config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": "1500abc"
        })",
        error);

    Check(!config.has_value(), "malformed total_credits should fail config parsing");
    Check(!error.empty(), "malformed total_credits should set error");
}

void TestRejectsNonFiniteConfigTotalCredits()
{
    std::wstring nan_error;
    const auto nan_config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": "nan"
        })",
        nan_error);

    Check(!nan_config.has_value(), "nan total_credits should fail config parsing");
    Check(!nan_error.empty(), "nan total_credits should set error");

    std::wstring inf_error;
    const auto inf_config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": "inf"
        })",
        inf_error);

    Check(!inf_config.has_value(), "inf total_credits should fail config parsing");
    Check(!inf_error.empty(), "inf total_credits should set error");
}

void TestRejectsUnquotedWhitespaceGarbageTotalCredits()
{
    std::wstring error;
    const auto config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": 1500 abc
        })",
        error);

    Check(!config.has_value(), "unquoted total_credits with whitespace garbage should fail config parsing");
    Check(!error.empty(), "unquoted total_credits with whitespace garbage should set error");
}

void TestRejectsFractionalBillingDay()
{
    std::wstring error;
    const auto config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": 1500,
            "billing_day": 15.9
        })",
        error);

    Check(!config.has_value(), "fractional billing_day should fail config parsing");
    Check(!error.empty(), "fractional billing_day should set error");
}

void TestRejectsUnquotedWhitespaceGarbageBillingDay()
{
    std::wstring error;
    const auto config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": 1500,
            "billing_day": 15 abc
        })",
        error);

    Check(!config.has_value(), "unquoted billing_day with whitespace garbage should fail config parsing");
    Check(!error.empty(), "unquoted billing_day with whitespace garbage should set error");
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

void TestExplicitAllowanceOverridesPlan()
{
    githubcopilotquota::PluginConfig config;
    config.plan = L"pro_plus";
    config.total_credits = 2345.0;

    std::wstring error;
    const auto allowance = githubcopilotquota::ResolveAllowance(config, error);

    Check(allowance.has_value(), "explicit allowance should resolve");
    CheckNear(allowance->total_credits, 2345.0, "explicit total credits should override plan allowance");
    Check(allowance->source == L"total_credits", "explicit allowance source should be total_credits");
    Check(error.empty(), "successful explicit allowance resolution should not set error");
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

void TestRejectsMalformedUsageNetQuantity()
{
    std::wstring error;
    const auto usage = githubcopilotquota::ParseUsageJson(
        R"({
            "user": "octocat",
            "usageItems": [
                { "product": "Copilot AI Credits", "sku": "AI Credit", "netQuantity": "7.25cr" }
            ]
        })",
        error);

    Check(!usage.has_value(), "malformed netQuantity should fail usage parsing");
    Check(!error.empty(), "malformed netQuantity should set error");
}

void TestRejectsNonFiniteUsageNetQuantity()
{
    std::wstring nan_error;
    const auto nan_usage = githubcopilotquota::ParseUsageJson(
        R"({
            "user": "octocat",
            "usageItems": [
                { "product": "Copilot AI Credits", "sku": "AI Credit", "netQuantity": "nan" }
            ]
        })",
        nan_error);

    Check(!nan_usage.has_value(), "nan netQuantity should fail usage parsing");
    Check(!nan_error.empty(), "nan netQuantity should set error");

    std::wstring inf_error;
    const auto inf_usage = githubcopilotquota::ParseUsageJson(
        R"({
            "user": "octocat",
            "usageItems": [
                { "product": "Copilot AI Credits", "sku": "AI Credit", "netQuantity": "inf" }
            ]
        })",
        inf_error);

    Check(!inf_usage.has_value(), "inf netQuantity should fail usage parsing");
    Check(!inf_error.empty(), "inf netQuantity should set error");
}

void TestRejectsUnquotedWhitespaceGarbageUsageNetQuantity()
{
    std::wstring error;
    const auto usage = githubcopilotquota::ParseUsageJson(
        R"({
            "user": "octocat",
            "usageItems": [
                { "product": "Copilot AI Credits", "sku": "AI Credit", "netQuantity": 7.25 cr }
            ]
        })",
        error);

    Check(!usage.has_value(), "unquoted netQuantity with whitespace garbage should fail usage parsing");
    Check(!error.empty(), "unquoted netQuantity with whitespace garbage should set error");
}

void TestParsesUserLogin()
{
    std::wstring error;
    const auto login = githubcopilotquota::ParseAuthenticatedUserJson(R"({"login":"octocat"})", error);

    Check(login.has_value(), "authenticated user JSON should parse");
    Check(*login == L"octocat", "login should parse");
}

void TestDefaultConfigPathUsesAppDataConfigFile()
{
    const auto path = githubcopilotquota::GetDefaultConfigPath();
    const std::wstring suffix = L"TrafficMonitorGitHubCopilotQuota\\config.json";

    Check(path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0,
        "default GitHub Copilot quota config path should end with TrafficMonitorGitHubCopilotQuota\\config.json");

    const DWORD appdata_length = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (appdata_length > 0)
    {
        std::wstring appdata(appdata_length, L'\0');
        const DWORD written = GetEnvironmentVariableW(L"APPDATA", appdata.data(), appdata_length);
        appdata.resize(written);
        Check(path.rfind(appdata, 0) == 0, "default GitHub Copilot quota config path should be under APPDATA");
    }
}

void TestFormatsCreditCounts()
{
    Check(githubcopilotquota::FormatCreditCount(0) == L"0cr", "zero credits should format");
    Check(githubcopilotquota::FormatCreditCount(950) == L"950cr", "under 1000 credits should use cr");
    Check(githubcopilotquota::FormatCreditCount(1200) == L"1.2kcr", "1000+ credits should use kcr");
    Check(githubcopilotquota::FormatCreditCount(20000) == L"20.0kcr", "large credits should keep one decimal");
}

void TestHandlesNonFiniteFormattingAndQuotaMath()
{
    const auto infinity = std::numeric_limits<double>::infinity();

    Check(githubcopilotquota::FormatCreditCount(infinity) == L"0cr", "infinite credits should format as zero credits");
    Check(githubcopilotquota::FormatCreditCount(-infinity) == L"0cr", "negative infinite credits should format as zero credits");
    Check(githubcopilotquota::FormatPercent(infinity) == L"0%", "infinite percent should format as zero percent");
    Check(githubcopilotquota::FormatPercent(-infinity) == L"0%", "negative infinite percent should format as zero percent");

    const auto quota_with_infinite_total = githubcopilotquota::CalculateQuota(infinity, 1.0);
    CheckNear(quota_with_infinite_total.total_credits, 0.0, "infinite total credits should clamp to zero");
    Check(std::isfinite(quota_with_infinite_total.remaining_credits), "infinite total should not produce non-finite remaining credits");
    Check(std::isfinite(quota_with_infinite_total.remaining_percent), "infinite total should not produce non-finite remaining percent");

    const auto quota_with_infinite_consumed = githubcopilotquota::CalculateQuota(1500.0, infinity);
    CheckNear(quota_with_infinite_consumed.consumed_credits, 0.0, "infinite consumed credits should clamp to zero");
    Check(std::isfinite(quota_with_infinite_consumed.remaining_credits), "infinite consumed should not produce non-finite remaining credits");
    Check(std::isfinite(quota_with_infinite_consumed.remaining_percent), "infinite consumed should not produce non-finite remaining percent");
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

void TestFormatsMonthlyResetCountdownInDays()
{
    const long long now = 1782086400;

    Check(githubcopilotquota::FormatResetCountdown(now + 7 * 24 * 60 * 60, now) == L"7d", "7-day monthly reset should stay day-based");
    Check(githubcopilotquota::FormatResetCountdown(now + 12 * 24 * 60 * 60, now) == L"12d", "12-day monthly reset should stay day-based");
    Check(githubcopilotquota::FormatResetCountdown(now + 31 * 24 * 60 * 60, now) == L"31d", "31-day monthly reset should stay day-based");
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

void TestBillingPeriodUsageDatesStopAtToday()
{
    const long long now = 1782086400;
    const auto period = githubcopilotquota::CalculateBillingPeriod(15, now);

    Check(period.end.year == 2026 && period.end.month == 7 && period.end.day == 15, "billing period end should remain next reset");
    Check(period.reset_at == 1784073600, "billing reset timestamp should remain next billing day UTC");
    Check(!period.usage_dates.empty(), "billing usage dates should include elapsed cycle dates");
    const auto last = period.usage_dates.back();
    Check(last.year == 2026 && last.month == 6 && last.day == 22, "billing usage dates should stop at today");
    for (const auto& date : period.usage_dates)
    {
        Check(IsOnOrBefore(date, githubcopilotquota::Date{2026, 6, 22}), "billing usage dates should not include future dates");
    }
}

void TestCalculatesBillingPeriodBeforeBillingDay()
{
    const long long now = 1780704000;
    const auto period = githubcopilotquota::CalculateBillingPeriod(15, now);

    Check(!period.is_calendar_month_estimate, "configured billing day before current cycle should be exact");
    Check(period.start.year == 2026 && period.start.month == 5 && period.start.day == 15, "pre-billing-day period start should be previous cycle start");
    Check(period.end.year == 2026 && period.end.month == 6 && period.end.day == 15, "pre-billing-day period end should be current cycle end");
    Check(period.reset_at == 1781481600, "pre-billing-day reset timestamp should be current billing day UTC");
}

void TestCalculatesBillingPeriodWithShorterMonth()
{
    const long long now = 1776816000;
    const auto period = githubcopilotquota::CalculateBillingPeriod(31, now);

    Check(!period.is_calendar_month_estimate, "configured billing day 31 should be exact");
    Check(period.start.year == 2026 && period.start.month == 3 && period.start.day == 31, "short-month period start should use prior month billing day");
    Check(period.end.year == 2026 && period.end.month == 4 && period.end.day == 30, "short-month period end should clamp to last day");
    Check(period.reset_at == 1777507200, "short-month reset timestamp should be clamped billing day UTC");
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

void TestFetchUsesConfigTokenAndClearsSnapshotToken()
{
    FakeGitHubTransport fake;
    fake.responses = FakeUsageResponses(8, 1.0);

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token","username":"octocat","total_credits":100,"billing_day":15})",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed with config token fallback");
    Check(!fake.requests.empty(), "fetch helper should issue usage requests");
    Check(!fake.requests.empty() && fake.requests.front().authorization == L"Bearer config-token", "config token should be used for Authorization header");
    Check(result.snapshot.config.github_token.empty(), "snapshot config should not retain GitHub token");
    CheckNear(result.snapshot.usage.consumed_credits, 8.0, "daily usage should be summed");
}

void TestFetchEnvTokenOverridesConfigToken()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeUsageResponse(2.0));

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token","username":"octocat","total_credits":100})",
        L"env-token",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed with env token");
    Check(fake.requests.size() == 1, "calendar estimate should issue one request");
    Check(!fake.requests.empty() && fake.requests.front().authorization == L"Bearer env-token", "env token should override config token");
}

void TestFetchMissingUsernameCallsUserEndpoint()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(githubcopilotquota::GitHubHttpResponse{200, R"({"login":"octocat"})"});
    fake.responses.push_back(FakeUsageResponse(3.0));

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token","total_credits":100})",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed after resolving missing username");
    Check(fake.requests.size() == 2, "missing username should add user request before usage request");
    Check(fake.requests.size() >= 1 && fake.requests[0].path == L"/user", "missing username should call /user");
    Check(result.snapshot.username == L"octocat", "resolved login should be snapshot username");
}

void TestFetchConfiguredUsernameSkipsUserEndpoint()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeUsageResponse(4.0));

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token","username":"octocat","total_credits":100})",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed with configured username");
    Check(fake.requests.size() == 1, "configured username should only issue usage request");
    Check(!fake.requests.empty() && fake.requests.front().path != L"/user", "configured username should skip /user");
}

void TestFetchAuthErrorsUseStableMessage()
{
    for (const auto status : {401, 403})
    {
        FakeGitHubTransport fake;
        fake.responses.push_back(githubcopilotquota::GitHubHttpResponse{status, "{}"});

        const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
            LR"({"github_token":"config-token","username":"octocat","total_credits":100})",
            L"",
            1782086400,
            FakeGitHubRequest,
            &fake);

        Check(!result.success, "auth HTTP status should fail fetch helper");
        Check(result.http_status == status, "auth HTTP status should be reported");
        Check(result.error == L"GitHub authentication failed or token lacks Plan read permission.", "auth error should use stable message");
    }
}

void TestFetchNonSuccessHttpStatusIncludesStatus()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(githubcopilotquota::GitHubHttpResponse{500, "{}"});

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token","username":"octocat","total_credits":100})",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(!result.success, "non-2xx HTTP status should fail fetch helper");
    Check(result.http_status == 500, "non-2xx HTTP status should be reported");
    Check(result.error == L"GitHub API returned HTTP 500.", "non-2xx error should include HTTP status");
}

void TestFetchBillingDayRequestsDailyUsageThroughToday()
{
    FakeGitHubTransport fake;
    fake.responses = FakeUsageResponses(8, 1.0);

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token","username":"octocat","total_credits":100,"billing_day":15})",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "billing-day fetch helper should succeed");
    Check(fake.requests.size() == 8, "billing-day mode should request elapsed daily usage only");
    Check(!fake.requests.empty() && fake.requests.front().path == L"/users/octocat/settings/billing/ai_credit/usage?year=2026&month=6&day=15", "billing-day first request should be cycle start");
    Check(!fake.requests.empty() && fake.requests.back().path == L"/users/octocat/settings/billing/ai_credit/usage?year=2026&month=6&day=22", "billing-day last request should be today");
}

void TestFetchCalendarEstimateRequestsMonthlyUsageOnce()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeUsageResponse(5.0));

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token","username":"octocat","total_credits":100})",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "calendar-estimate fetch helper should succeed");
    Check(fake.requests.size() == 1, "calendar-estimate mode should issue one monthly request");
    Check(!fake.requests.empty() && fake.requests.front().path == L"/users/octocat/settings/billing/ai_credit/usage?year=2026&month=6", "calendar-estimate request should use monthly path");
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
    TestRejectsMalformedConfigTotalCredits();
    TestRejectsNonFiniteConfigTotalCredits();
    TestRejectsUnquotedWhitespaceGarbageTotalCredits();
    TestRejectsFractionalBillingDay();
    TestRejectsUnquotedWhitespaceGarbageBillingDay();
    TestResolvesAllowanceFromPlan();
    TestExplicitAllowanceOverridesPlan();
    TestRejectsMissingAllowance();
    TestParsesUsageReport();
    TestRejectsMalformedUsageNetQuantity();
    TestRejectsNonFiniteUsageNetQuantity();
    TestRejectsUnquotedWhitespaceGarbageUsageNetQuantity();
    TestParsesUserLogin();
    TestDefaultConfigPathUsesAppDataConfigFile();
    TestFormatsCreditCounts();
    TestHandlesNonFiniteFormattingAndQuotaMath();
    TestCalculatesRemainingQuota();
    TestClampsOverage();
    TestFormatsQuotaValue();
    TestFormatsMonthlyResetCountdownInDays();
    TestCalculatesBillingPeriod();
    TestBillingPeriodUsageDatesStopAtToday();
    TestCalculatesBillingPeriodBeforeBillingDay();
    TestCalculatesBillingPeriodWithShorterMonth();
    TestCalculatesCalendarMonthEstimate();
    TestBuildsUsagePaths();
    TestFetchUsesConfigTokenAndClearsSnapshotToken();
    TestFetchEnvTokenOverridesConfigToken();
    TestFetchMissingUsernameCallsUserEndpoint();
    TestFetchConfiguredUsernameSkipsUserEndpoint();
    TestFetchAuthErrorsUseStableMessage();
    TestFetchNonSuccessHttpStatusIncludesStatus();
    TestFetchBillingDayRequestsDailyUsageThroughToday();
    TestFetchCalendarEstimateRequestsMonthlyUsageOnce();
    TestLiveFetchWhenRequested();

    if (failures != 0)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All GitHub Copilot quota tests passed\n";
    return 0;
}
