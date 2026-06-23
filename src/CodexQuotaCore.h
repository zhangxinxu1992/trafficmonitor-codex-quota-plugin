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

enum class QuotaDisplayMode
{
    Remaining,
    Used
};

enum class ResetDisplayMode
{
    Countdown,
    Time
};

struct DisplayOptions
{
    QuotaDisplayMode quota_display{QuotaDisplayMode::Remaining};
    ResetDisplayMode reset_display{ResetDisplayMode::Countdown};
    bool show_reset_info{true};
};

struct PluginConfig
{
    DisplayOptions display;
};

std::optional<Credentials> ParseCredentialsJson(const std::wstring& json, std::wstring& error);
std::optional<UsageSnapshot> ParseUsageJson(const std::string& json, std::wstring& error);
std::optional<PluginConfig> ParseConfigJson(const std::wstring& json, std::wstring& error);
std::wstring SerializeConfigJson(const PluginConfig& config);

std::wstring FormatPercent(double used_percent);
std::wstring FormatRemainingPercent(double used_percent);
std::wstring FormatRemainingWindowText(double used_percent, long long reset_at, long long now);
std::wstring FormatWindowText(double used_percent, long long reset_at, long long now, const DisplayOptions& options);
std::wstring FormatResetCountdown(long long reset_at, long long now);
std::wstring FormatResetTime(long long reset_at, long long now);
}
