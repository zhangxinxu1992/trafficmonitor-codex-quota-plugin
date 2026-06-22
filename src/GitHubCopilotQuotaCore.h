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
    bool is_copilot_internal{};
};

struct Quota
{
    double total_credits{};
    double consumed_credits{};
    double remaining_credits{};
    double remaining_percent{};
};

struct CopilotInternalQuotaSnapshot
{
    std::wstring plan;
    std::wstring quota_id;
    double total_credits{};
    double remaining_credits{};
    double remaining_percent{};
    long long reset_at{};
};

std::optional<PluginConfig> ParseConfigJson(const std::wstring& json, std::wstring& error);
std::optional<Allowance> ResolveAllowance(const PluginConfig& config, std::wstring& error);
std::optional<UsageReport> ParseUsageJson(const std::string& json, std::wstring& error);
std::optional<std::wstring> ParseAuthenticatedUserJson(const std::string& json, std::wstring& error);
std::optional<CopilotInternalQuotaSnapshot> ParseCopilotInternalUserJson(const std::string& json, std::wstring& error);

Quota CalculateQuota(double total_credits, double consumed_credits);
Quota CalculateQuotaFromRemaining(double total_credits, double remaining_credits, std::optional<double> remaining_percent);
UsagePeriod CalculateBillingPeriod(int billing_day, long long now);
UsagePeriod CalculateCalendarMonthEstimate(long long now);

std::wstring BuildUsagePath(const std::wstring& username, const Date& date);
std::wstring BuildMonthlyUsagePath(const std::wstring& username, int year, int month);

std::wstring FormatCreditCount(double credits);
std::wstring FormatPercent(double percent);
std::wstring FormatResetCountdown(long long reset_at, long long now);
std::wstring FormatQuotaValue(const Quota& quota, long long reset_at, long long now);
}
