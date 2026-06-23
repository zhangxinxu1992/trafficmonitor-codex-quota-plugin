#include "CodexQuotaCore.h"
#include "CodexQuotaFetch.h"

#include <iostream>
#include <string>
#include <Windows.h>

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

void TestParsesTokenCredentials()
{
    std::wstring error;
    const auto credentials = codexquota::ParseCredentialsJson(
        LR"({
            "tokens": {
                "access_token": "access-token",
                "refresh_token": "ignored-refresh-token",
                "account_id": "account-id"
            }
        })",
        error);

    Check(credentials.has_value(), "tokens credentials should parse");
    Check(credentials->access_token == L"access-token", "access token should parse");
    Check(credentials->account_id == L"account-id", "account id should parse");
    Check(error.empty(), "successful credential parse should not set error");
}

void TestPrefersOpenAiApiKey()
{
    std::wstring error;
    const auto credentials = codexquota::ParseCredentialsJson(
        LR"({
            "OPENAI_API_KEY": "root-key",
            "tokens": {
                "access_token": "access-token",
                "account_id": "account-id"
            }
        })",
        error);

    Check(credentials.has_value(), "OPENAI_API_KEY credentials should parse");
    Check(credentials->access_token == L"root-key", "OPENAI_API_KEY should be preferred");
    Check(credentials->account_id.empty(), "OPENAI_API_KEY should not reuse account id");
}

void TestRejectsMissingCredentials()
{
    std::wstring error;
    const auto credentials = codexquota::ParseCredentialsJson(LR"({"tokens": {}})", error);

    Check(!credentials.has_value(), "missing access token should fail");
    Check(!error.empty(), "missing access token should explain failure");
}

void TestParsesUsageWindows()
{
    std::wstring error;
    const auto usage = codexquota::ParseUsageJson(
        R"({
            "plan_type": "pro",
            "rate_limit": {
                "primary_window": {
                    "used_percent": 24,
                    "limit_window_seconds": 18000,
                    "reset_at": 1781798102
                },
                "secondary_window": {
                    "used_percent": "10",
                    "limit_window_seconds": 604800,
                    "reset_at": 1782363780
                }
            },
            "additional_rate_limits": [
                {
                    "limit_name": "GPT-5.3-Codex-Spark",
                    "metered_feature": "codex_bengalfox"
                }
            ]
        })",
        error);

    Check(usage.has_value(), "usage JSON should parse");
    Check(usage->plan_type == L"pro", "plan_type should parse");
    Check(usage->primary.present, "primary window should be present");
    Check(usage->primary.used_percent == 24.0, "primary used percent should parse");
    Check(usage->primary.limit_window_seconds == 18000, "primary window seconds should parse");
    Check(usage->primary.reset_at == 1781798102, "primary reset_at should parse");
    Check(usage->secondary.present, "secondary window should be present");
    Check(usage->secondary.used_percent == 10.0, "secondary string percent should parse");
    Check(usage->secondary.limit_window_seconds == 604800, "secondary window seconds should parse");
    Check(usage->secondary.reset_at == 1782363780, "secondary reset_at should parse");
    Check(error.empty(), "successful usage parse should not set error");
}

void TestFormatsUsedPercent()
{
    Check(codexquota::FormatPercent(24.4) == L"24%", "percent should round down below .5");
    Check(codexquota::FormatPercent(24.5) == L"25%", "percent should round half up");
    Check(codexquota::FormatPercent(-1.0) == L"0%", "negative percent should clamp");
    Check(codexquota::FormatPercent(125.0) == L"125%", "over-budget percent should be preserved");
}

void TestFormatsRemainingPercent()
{
    Check(codexquota::FormatRemainingPercent(24.4) == L"76%", "remaining percent should subtract from 100 and round");
    Check(codexquota::FormatRemainingPercent(24.5) == L"76%", "remaining percent should round half up");
    Check(codexquota::FormatRemainingPercent(-1.0) == L"100%", "negative used percent should leave full remaining quota");
    Check(codexquota::FormatRemainingPercent(125.0) == L"0%", "over-budget remaining percent should clamp to zero");
}

void TestFormatsRemainingWindowText()
{
    Check(codexquota::FormatRemainingWindowText(24.4, 1700000300, 1700000000) == L"76% 5m", "remaining window text should include reset countdown");
    Check(codexquota::FormatRemainingWindowText(31.0, 1700016200, 1700000000) == L"69% 4h 30m", "taskbar hour countdown should keep minutes");
    Check(codexquota::FormatRemainingWindowText(12.0, 1700522000, 1700000000) == L"88% 6d 1h", "taskbar day countdown should keep hours");
    Check(codexquota::FormatRemainingWindowText(10.0, 1700604800, 1700000000) == L"90% 1w", "weekly countdown should stay compact");
    Check(codexquota::FormatRemainingWindowText(24.4, 0, 1700000000) == L"76%", "missing reset time should omit countdown");
}

long long LocalTimestamp(int year, int month, int day, int hour, int minute)
{
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&local));
}

void TestParsesDisplayConfig()
{
    std::wstring error;
    const auto config = codexquota::ParseConfigJson(
        LR"({
            "quota_display": "used",
            "reset_display": "time",
            "show_reset_info": false
        })",
        error);

    Check(config.has_value(), "Codex display config should parse");
    Check(config->display.quota_display == codexquota::QuotaDisplayMode::Used, "Codex quota display should parse as used");
    Check(config->display.reset_display == codexquota::ResetDisplayMode::Time, "Codex reset display should parse as time");
    Check(!config->display.show_reset_info, "Codex reset info display flag should parse");
    Check(error.empty(), "successful Codex display config parse should not set error");

    error.clear();
    const auto default_config = codexquota::ParseConfigJson(L"{}", error);
    Check(default_config.has_value(), "empty Codex display config should parse");
    Check(default_config->display.show_reset_info, "Codex reset info should default to visible");
    Check(codexquota::SerializeConfigJson(*config).find(L"\"show_reset_info\": false") != std::wstring::npos,
        "Codex config serialization should persist hidden reset info");
}

void TestFormatsWindowTextWithDisplayOptions()
{
    codexquota::DisplayOptions options;
    const auto now = LocalTimestamp(2026, 6, 22, 10, 0);
    const auto same_day_reset = LocalTimestamp(2026, 6, 22, 18, 30);
    const auto next_day_reset = LocalTimestamp(2026, 6, 23, 8, 5);

    options.quota_display = codexquota::QuotaDisplayMode::Used;
    Check(codexquota::FormatWindowText(24.4, same_day_reset, now, options) == L"24% 8h 30m",
        "used display should show used percent with countdown");

    options.quota_display = codexquota::QuotaDisplayMode::Remaining;
    options.reset_display = codexquota::ResetDisplayMode::Time;
    Check(codexquota::FormatWindowText(24.4, same_day_reset, now, options) == L"76% 18:30",
        "same-day reset time should show local HH:MM");
    Check(codexquota::FormatWindowText(24.4, next_day_reset, now, options) == L"76% 06-23 08:05",
        "cross-day reset time should show local month-day and time");

    options.show_reset_info = false;
    Check(codexquota::FormatWindowText(24.4, next_day_reset, now, options) == L"76%",
        "hidden Codex reset info should leave only the quota percent");
}

void TestFormatsResourceGraphValueWithDisplayOptions()
{
    codexquota::DisplayOptions options;
    Check(codexquota::FormatResourceGraphValue(24.0, options) == 0.76f,
        "remaining display should graph remaining Codex quota");

    options.quota_display = codexquota::QuotaDisplayMode::Used;
    Check(codexquota::FormatResourceGraphValue(24.0, options) == 0.24f,
        "used display should graph used Codex quota");
    Check(codexquota::FormatResourceGraphValue(-5.0, options) == 0.0f,
        "negative Codex graph values should clamp to zero");
    Check(codexquota::FormatResourceGraphValue(125.0, options) == 1.0f,
        "over-budget Codex graph values should clamp to one");
}

void TestFormatsCountdown()
{
    Check(codexquota::FormatResetCountdown(1700000000, 1700000000) == L"now", "past reset should be now");
    Check(codexquota::FormatResetCountdown(1700000300, 1700000000) == L"5m", "minutes should format");
    Check(codexquota::FormatResetCountdown(1700007200, 1700000000) == L"2h", "whole hours should omit minutes");
    Check(codexquota::FormatResetCountdown(1700090000, 1700000000) == L"1d 1h", "days should format with remaining hours");
}

void TestLiveFetchWhenRequested()
{
    wchar_t flag[8]{};
    if (GetEnvironmentVariableW(L"TRAFFICMONITOR_CODEX_QUOTA_RUN_LIVE_TEST", flag, 8) == 0
        && GetEnvironmentVariableW(L"CODEX_QUOTA_RUN_LIVE_TEST", flag, 8) == 0)
    {
        return;
    }

    const auto result = codexquota::FetchUsageSnapshot();
    if (!result.success)
    {
        std::wcerr << L"LIVE ERROR: " << result.error << L" status=" << result.http_status << L'\n';
    }
    Check(result.success, "live Codex usage fetch should succeed when requested");
    Check(result.usage.primary.present, "live Codex usage should include primary window");
    Check(result.usage.secondary.present, "live Codex usage should include secondary window");
}
}

int main()
{
    TestParsesTokenCredentials();
    TestPrefersOpenAiApiKey();
    TestRejectsMissingCredentials();
    TestParsesUsageWindows();
    TestFormatsUsedPercent();
    TestFormatsRemainingPercent();
    TestFormatsRemainingWindowText();
    TestParsesDisplayConfig();
    TestFormatsWindowTextWithDisplayOptions();
    TestFormatsResourceGraphValueWithDisplayOptions();
    TestFormatsCountdown();
    TestLiveFetchWhenRequested();

    if (failures != 0)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All tests passed\n";
    return 0;
}
