#pragma once

#include <optional>
#include <string>

namespace githubcopilotquota
{
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
    bool show_remaining_credits{true};
};

struct PluginConfig
{
    std::wstring username;
    DisplayOptions display;
};

enum class GitHubTokenSource
{
    StoredCredential
};

struct GitHubTokenChoice
{
    std::wstring token;
    GitHubTokenSource source{};
};

struct Allowance
{
    double total_credits{};
    std::wstring source;
};

struct UsagePeriod
{
    long long reset_at{};
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

struct DeviceCodeResponse
{
    std::wstring device_code;
    std::wstring user_code;
    std::wstring verification_uri;
    std::wstring verification_uri_complete;
    int expires_in{};
    int interval{};
};

enum class OAuthTokenStatus
{
    Success,
    AuthorizationPending,
    SlowDown,
    ExpiredToken,
    AccessDenied,
    Error
};

struct OAuthTokenResponse
{
    OAuthTokenStatus status{OAuthTokenStatus::Error};
    std::wstring access_token;
    std::wstring token_type;
    std::wstring scope;
    std::wstring error;
};

std::optional<PluginConfig> ParseConfigJson(const std::wstring& json, std::wstring& error);
std::wstring SerializeConfigJson(const PluginConfig& config);
std::optional<GitHubTokenChoice> ResolveGitHubToken(
    const std::wstring& stored_token,
    const PluginConfig& config,
    std::wstring& error);
std::optional<std::wstring> ParseAuthenticatedUserJson(const std::string& json, std::wstring& error);
std::optional<CopilotInternalQuotaSnapshot> ParseCopilotInternalUserJson(const std::string& json, std::wstring& error);
std::optional<DeviceCodeResponse> ParseDeviceCodeJson(const std::string& json, std::wstring& error);
OAuthTokenResponse ParseAccessTokenJson(const std::string& json, std::wstring& error);

Quota CalculateQuota(double total_credits, double consumed_credits);
Quota CalculateQuotaFromRemaining(double total_credits, double remaining_credits, std::optional<double> remaining_percent);

std::wstring FormatCreditCount(double credits);
std::wstring FormatPercent(double percent);
std::wstring FormatResetCountdown(long long reset_at, long long now);
std::wstring FormatResetTime(long long reset_at, long long now);
std::wstring FormatQuotaValue(const Quota& quota, long long reset_at, long long now);
std::wstring FormatQuotaValue(const Quota& quota, long long reset_at, long long now, const DisplayOptions& options);
}
