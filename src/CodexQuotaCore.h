#pragma once

#include <optional>
#include <string>

namespace codexquota
{
struct Credentials
{
    std::wstring access_token;
    std::wstring account_id;
};

struct RateWindow
{
    bool present{};
    double used_percent{};
    int limit_window_seconds{};
    long long reset_at{};
};

struct UsageSnapshot
{
    std::wstring plan_type;
    RateWindow primary;
    RateWindow secondary;
};

std::optional<Credentials> ParseCredentialsJson(const std::wstring& json, std::wstring& error);
std::optional<UsageSnapshot> ParseUsageJson(const std::string& json, std::wstring& error);

std::wstring FormatPercent(double used_percent);
std::wstring FormatRemainingPercent(double used_percent);
std::wstring FormatRemainingWindowText(double used_percent, long long reset_at, long long now);
std::wstring FormatResetCountdown(long long reset_at, long long now);
}
