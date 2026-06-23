#include "GitHubCopilotQuotaCore.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace
{
struct Date
{
    int year{};
    int month{};
    int day{};
};

std::wstring Trim(const std::wstring& value)
{
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
    {
        return {};
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string Trim(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool IsJsonWhitespace(wchar_t ch)
{
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

bool IsJsonWhitespace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

float ClampPercentForGraph(double percent)
{
    if (!std::isfinite(percent) || percent < 0.0)
    {
        return 0.0f;
    }
    if (percent > 100.0)
    {
        return 1.0f;
    }
    return static_cast<float>(percent / 100.0);
}

bool HasOnlyTrailingWhitespace(const std::wstring& value, std::size_t start)
{
    for (auto index = start; index < value.size(); ++index)
    {
        if (!IsJsonWhitespace(value[index]))
        {
            return false;
        }
    }
    return true;
}

bool HasOnlyTrailingWhitespace(const std::string& value, std::size_t start)
{
    for (auto index = start; index < value.size(); ++index)
    {
        if (!IsJsonWhitespace(value[index]))
        {
            return false;
        }
    }
    return true;
}

std::wstring ToWide(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
}

std::optional<std::wstring> ParseJsonStringAt(const std::wstring& json, std::size_t quote_pos)
{
    if (quote_pos == std::wstring::npos || quote_pos >= json.size() || json[quote_pos] != L'"')
    {
        return std::nullopt;
    }

    std::wstring value;
    bool escaped = false;
    for (auto index = quote_pos + 1; index < json.size(); ++index)
    {
        const auto ch = json[index];
        if (escaped)
        {
            value.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == L'\\')
        {
            escaped = true;
            continue;
        }
        if (ch == L'"')
        {
            return value;
        }
        value.push_back(ch);
    }

    return std::nullopt;
}

std::optional<std::string> ParseJsonStringAt(const std::string& json, std::size_t quote_pos)
{
    if (quote_pos == std::string::npos || quote_pos >= json.size() || json[quote_pos] != '"')
    {
        return std::nullopt;
    }

    std::string value;
    bool escaped = false;
    for (auto index = quote_pos + 1; index < json.size(); ++index)
    {
        const auto ch = json[index];
        if (escaped)
        {
            value.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\')
        {
            escaped = true;
            continue;
        }
        if (ch == '"')
        {
            return value;
        }
        value.push_back(ch);
    }

    return std::nullopt;
}

std::optional<std::size_t> FindJsonValueStart(const std::wstring& json, const std::wstring& key)
{
    const std::wstring quoted_key = L"\"" + key + L"\"";
    const auto key_pos = json.find(quoted_key);
    if (key_pos == std::wstring::npos)
    {
        return std::nullopt;
    }

    const auto colon_pos = json.find(L':', key_pos + quoted_key.size());
    if (colon_pos == std::wstring::npos)
    {
        return std::nullopt;
    }

    const auto value_pos = json.find_first_not_of(L" \t\r\n", colon_pos + 1);
    if (value_pos == std::wstring::npos)
    {
        return std::nullopt;
    }
    return value_pos;
}

std::optional<std::size_t> FindJsonValueStart(const std::string& json, const std::string& key)
{
    const std::string quoted_key = "\"" + key + "\"";
    const auto key_pos = json.find(quoted_key);
    if (key_pos == std::string::npos)
    {
        return std::nullopt;
    }

    const auto colon_pos = json.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos)
    {
        return std::nullopt;
    }

    const auto value_pos = json.find_first_not_of(" \t\r\n", colon_pos + 1);
    if (value_pos == std::string::npos)
    {
        return std::nullopt;
    }
    return value_pos;
}

std::optional<std::wstring> FindJsonStringValue(const std::wstring& json, const std::wstring& key)
{
    const auto value_pos = FindJsonValueStart(json, key);
    if (!value_pos.has_value() || json[*value_pos] != L'"')
    {
        return std::nullopt;
    }
    return ParseJsonStringAt(json, *value_pos);
}

std::optional<std::string> FindJsonStringValue(const std::string& json, const std::string& key)
{
    const auto value_pos = FindJsonValueStart(json, key);
    if (!value_pos.has_value() || json[*value_pos] != '"')
    {
        return std::nullopt;
    }
    return ParseJsonStringAt(json, *value_pos);
}

std::optional<std::wstring> FindJsonScalarText(const std::wstring& json, const std::wstring& key)
{
    const auto value_pos = FindJsonValueStart(json, key);
    if (!value_pos.has_value())
    {
        return std::nullopt;
    }

    if (json[*value_pos] == L'"')
    {
        return ParseJsonStringAt(json, *value_pos);
    }

    const auto value_end = json.find_first_of(L",}]", *value_pos);
    return Trim(json.substr(*value_pos, value_end == std::wstring::npos ? std::wstring::npos : value_end - *value_pos));
}

std::optional<bool> FindJsonBool(const std::wstring& json, const std::wstring& key)
{
    const auto text = FindJsonScalarText(json, key);
    if (!text.has_value())
    {
        return std::nullopt;
    }

    const auto value = Trim(*text);
    if (value == L"true")
    {
        return true;
    }
    if (value == L"false")
    {
        return false;
    }
    return std::nullopt;
}

std::optional<std::string> FindJsonScalarText(const std::string& json, const std::string& key)
{
    const auto value_pos = FindJsonValueStart(json, key);
    if (!value_pos.has_value())
    {
        return std::nullopt;
    }

    if (json[*value_pos] == '"')
    {
        return ParseJsonStringAt(json, *value_pos);
    }

    const auto value_end = json.find_first_of(",}]", *value_pos);
    return Trim(json.substr(*value_pos, value_end == std::string::npos ? std::string::npos : value_end - *value_pos));
}

std::optional<double> FindJsonDouble(const std::wstring& json, const std::wstring& key)
{
    const auto text = FindJsonScalarText(json, key);
    if (!text.has_value())
    {
        return std::nullopt;
    }

    try
    {
        std::size_t parsed{};
        const auto value = std::stod(*text, &parsed);
        if (!HasOnlyTrailingWhitespace(*text, parsed))
        {
            return std::nullopt;
        }
        if (!std::isfinite(value))
        {
            return std::nullopt;
        }
        return value;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<double> FindJsonDouble(const std::string& json, const std::string& key)
{
    const auto text = FindJsonScalarText(json, key);
    if (!text.has_value())
    {
        return std::nullopt;
    }

    try
    {
        std::size_t parsed{};
        const auto value = std::stod(*text, &parsed);
        if (!HasOnlyTrailingWhitespace(*text, parsed))
        {
            return std::nullopt;
        }
        if (!std::isfinite(value))
        {
            return std::nullopt;
        }
        return value;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<int> FindJsonInt(const std::string& json, const std::string& key)
{
    const auto value = FindJsonDouble(json, key);
    if (!value.has_value())
    {
        return std::nullopt;
    }
    const auto rounded = std::round(*value);
    if (std::fabs(*value - rounded) > 0.0001)
    {
        return std::nullopt;
    }
    return static_cast<int>(rounded);
}

std::optional<std::string> JsonObjectAt(const std::string& json, std::size_t object_pos)
{
    if (object_pos == std::string::npos || object_pos >= json.size() || json[object_pos] != '{')
    {
        return std::nullopt;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (auto index = object_pos; index < json.size(); ++index)
    {
        const auto ch = json[index];
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                in_string = false;
            }
            continue;
        }

        if (ch == '"')
        {
            in_string = true;
        }
        else if (ch == '{')
        {
            ++depth;
        }
        else if (ch == '}')
        {
            --depth;
            if (depth == 0)
            {
                return json.substr(object_pos, index - object_pos + 1);
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> FindJsonObject(const std::string& json, const std::string& key)
{
    const auto value_pos = FindJsonValueStart(json, key);
    if (!value_pos.has_value())
    {
        return std::nullopt;
    }
    return JsonObjectAt(json, *value_pos);
}

std::optional<std::string> FirstNestedJsonObject(const std::string& json)
{
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t index = 0; index < json.size(); ++index)
    {
        const auto ch = json[index];
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                in_string = false;
            }
            continue;
        }

        if (ch == '"')
        {
            in_string = true;
        }
        else if (ch == '{')
        {
            ++depth;
            if (depth == 2)
            {
                return JsonObjectAt(json, index);
            }
        }
        else if (ch == '}' && depth > 0)
        {
            --depth;
        }
    }

    return std::nullopt;
}

bool IsLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int DaysInMonth(int year, int month)
{
    static constexpr int days_by_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsLeapYear(year))
    {
        return 29;
    }
    if (month < 1 || month > 12)
    {
        return 31;
    }
    return days_by_month[month - 1];
}

long long TimestampFromDate(const Date& date)
{
    std::tm utc{};
    utc.tm_year = date.year - 1900;
    utc.tm_mon = date.month - 1;
    utc.tm_mday = date.day;
    utc.tm_isdst = 0;
    return static_cast<long long>(::_mkgmtime(&utc));
}

std::optional<long long> ParseResetTimestamp(const std::string& value)
{
    if (value.size() >= 10 && value[4] == '-' && value[7] == '-')
    {
        try
        {
            const auto year = std::stoi(value.substr(0, 4));
            const auto month = std::stoi(value.substr(5, 2));
            const auto day = std::stoi(value.substr(8, 2));
            if (month >= 1 && month <= 12 && day >= 1 && day <= DaysInMonth(year, month))
            {
                return TimestampFromDate(Date{year, month, day});
            }
        }
        catch (...)
        {
        }
    }

    return std::nullopt;
}

std::optional<githubcopilotquota::CopilotInternalQuotaSnapshot> QuotaFromObject(
    const std::string& key,
    const std::string& object)
{
    const auto placeholder = FindJsonScalarText(object, "placeholder").value_or("");
    if (placeholder == "true")
    {
        return std::nullopt;
    }

    const auto entitlement = FindJsonDouble(object, "entitlement");
    const auto remaining = FindJsonDouble(object, "remaining");
    auto percent_remaining = FindJsonDouble(object, "percent_remaining");
    const auto quota_id = FindJsonStringValue(object, "quota_id").value_or(key);

    if (entitlement == 0.0 && remaining == 0.0 && percent_remaining.value_or(0.0) == 0.0 && quota_id.empty())
    {
        return std::nullopt;
    }

    if (!entitlement.has_value() || *entitlement <= 0.0)
    {
        return std::nullopt;
    }
    if (!remaining.has_value())
    {
        return std::nullopt;
    }
    if (!percent_remaining.has_value())
    {
        percent_remaining = *remaining * 100.0 / *entitlement;
    }

    githubcopilotquota::CopilotInternalQuotaSnapshot snapshot;
    snapshot.quota_id = ToWide(quota_id);
    snapshot.total_credits = *entitlement;
    snapshot.remaining_credits = *remaining;
    snapshot.remaining_percent = *percent_remaining;
    return snapshot;
}

std::optional<githubcopilotquota::CopilotInternalQuotaSnapshot> QuotaFromLimitedCounts(
    const std::string& quota_id,
    const std::optional<double>& entitlement,
    const std::optional<double>& remaining)
{
    if (!entitlement.has_value() || !remaining.has_value() || *entitlement <= 0.0)
    {
        return std::nullopt;
    }

    githubcopilotquota::CopilotInternalQuotaSnapshot snapshot;
    snapshot.quota_id = ToWide(quota_id);
    snapshot.total_credits = *entitlement;
    snapshot.remaining_credits = *remaining;
    snapshot.remaining_percent = *remaining * 100.0 / *entitlement;
    return snapshot;
}

std::optional<githubcopilotquota::QuotaDisplayMode> ParseQuotaDisplayMode(const std::wstring& text)
{
    const auto value = Trim(text);
    if (value.empty() || value == L"remaining")
    {
        return githubcopilotquota::QuotaDisplayMode::Remaining;
    }
    if (value == L"used")
    {
        return githubcopilotquota::QuotaDisplayMode::Used;
    }
    return std::nullopt;
}

std::optional<githubcopilotquota::ResetDisplayMode> ParseResetDisplayMode(const std::wstring& text)
{
    const auto value = Trim(text);
    if (value.empty() || value == L"countdown")
    {
        return githubcopilotquota::ResetDisplayMode::Countdown;
    }
    if (value == L"time")
    {
        return githubcopilotquota::ResetDisplayMode::Time;
    }
    return std::nullopt;
}

std::wstring QuotaDisplayModeText(githubcopilotquota::QuotaDisplayMode mode)
{
    return mode == githubcopilotquota::QuotaDisplayMode::Used ? L"used" : L"remaining";
}

std::wstring ResetDisplayModeText(githubcopilotquota::ResetDisplayMode mode)
{
    return mode == githubcopilotquota::ResetDisplayMode::Time ? L"time" : L"countdown";
}

std::wstring EscapeJsonString(const std::wstring& value)
{
    std::wstring escaped;
    escaped.reserve(value.size());
    for (const auto ch : value)
    {
        if (ch == L'\\' || ch == L'"')
        {
            escaped.push_back(L'\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

}

namespace githubcopilotquota
{
std::optional<PluginConfig> ParseConfigJson(const std::wstring& json, std::wstring& error)
{
    error.clear();

    PluginConfig config;
    if (const auto username = FindJsonStringValue(json, L"username"))
    {
        config.username = Trim(*username);
    }
    if (const auto quota_display = FindJsonStringValue(json, L"quota_display"))
    {
        const auto mode = ParseQuotaDisplayMode(*quota_display);
        if (!mode.has_value())
        {
            error = L"TrafficMonitor GitHub Copilot quota config quota_display must be remaining or used.";
            return std::nullopt;
        }
        config.display.quota_display = *mode;
    }
    if (const auto reset_display = FindJsonStringValue(json, L"reset_display"))
    {
        const auto mode = ParseResetDisplayMode(*reset_display);
        if (!mode.has_value())
        {
            error = L"TrafficMonitor GitHub Copilot quota config reset_display must be countdown or time.";
            return std::nullopt;
        }
        config.display.reset_display = *mode;
    }
    if (FindJsonValueStart(json, L"show_remaining_credits").has_value())
    {
        const auto show_remaining_credits = FindJsonBool(json, L"show_remaining_credits");
        if (!show_remaining_credits.has_value())
        {
            error = L"TrafficMonitor GitHub Copilot quota config show_remaining_credits must be true or false.";
            return std::nullopt;
        }
        config.display.show_remaining_credits = *show_remaining_credits;
    }
    if (FindJsonValueStart(json, L"show_reset_info").has_value())
    {
        const auto show_reset_info = FindJsonBool(json, L"show_reset_info");
        if (!show_reset_info.has_value())
        {
            error = L"TrafficMonitor GitHub Copilot quota config show_reset_info must be true or false.";
            return std::nullopt;
        }
        config.display.show_reset_info = *show_reset_info;
    }

    return config;
}

std::wstring SerializeConfigJson(const PluginConfig& config)
{
    std::wostringstream stream;
    stream << L"{\n";
    bool first = true;
    auto write_separator = [&] {
        if (!first)
        {
            stream << L",\n";
        }
        first = false;
    };
    auto write_string = [&](const wchar_t* key, const std::wstring& value) {
        if (value.empty())
        {
            return;
        }
        write_separator();
        stream << L"  \"" << key << L"\": \"" << EscapeJsonString(value) << L"\"";
    };

    write_string(L"username", config.username);

    write_separator();
    stream << L"  \"quota_display\": \"" << QuotaDisplayModeText(config.display.quota_display) << L"\"";
    write_separator();
    stream << L"  \"reset_display\": \"" << ResetDisplayModeText(config.display.reset_display) << L"\"";
    write_separator();
    stream << L"  \"show_reset_info\": " << (config.display.show_reset_info ? L"true" : L"false");
    write_separator();
    stream << L"  \"show_remaining_credits\": " << (config.display.show_remaining_credits ? L"true" : L"false");
    stream << L"\n}\n";
    return stream.str();
}

std::optional<GitHubTokenChoice> ResolveGitHubToken(
    const std::wstring& stored_token,
    const PluginConfig& config,
    std::wstring& error)
{
    error.clear();

    const auto trimmed_stored_token = Trim(stored_token);
    if (!trimmed_stored_token.empty())
    {
        return GitHubTokenChoice{trimmed_stored_token, GitHubTokenSource::StoredCredential};
    }

    (void)config;
    error = L"Missing GitHub token. Sign in from TrafficMonitor plugin options.";
    return std::nullopt;
}

std::optional<DeviceCodeResponse> ParseDeviceCodeJson(const std::string& json, std::wstring& error)
{
    error.clear();

    DeviceCodeResponse response;
    const auto device_code = FindJsonStringValue(json, "device_code");
    const auto user_code = FindJsonStringValue(json, "user_code");
    const auto verification_uri = FindJsonStringValue(json, "verification_uri");
    const auto expires_in = FindJsonInt(json, "expires_in");
    const auto interval = FindJsonInt(json, "interval");
    if (!device_code.has_value() || !user_code.has_value() || !verification_uri.has_value()
        || !expires_in.has_value() || !interval.has_value())
    {
        error = L"GitHub device code response is missing required fields.";
        return std::nullopt;
    }

    response.device_code = ToWide(*device_code);
    response.user_code = ToWide(*user_code);
    response.verification_uri = ToWide(*verification_uri);
    if (const auto complete = FindJsonStringValue(json, "verification_uri_complete"))
    {
        response.verification_uri_complete = ToWide(*complete);
    }
    response.expires_in = *expires_in;
    response.interval = *interval;
    return response;
}

OAuthTokenResponse ParseAccessTokenJson(const std::string& json, std::wstring& error)
{
    error.clear();

    OAuthTokenResponse response;
    if (const auto oauth_error = FindJsonStringValue(json, "error"))
    {
        if (*oauth_error == "authorization_pending")
        {
            response.status = OAuthTokenStatus::AuthorizationPending;
            return response;
        }
        if (*oauth_error == "slow_down")
        {
            response.status = OAuthTokenStatus::SlowDown;
            return response;
        }
        if (*oauth_error == "expired_token")
        {
            response.status = OAuthTokenStatus::ExpiredToken;
            return response;
        }
        if (*oauth_error == "access_denied")
        {
            response.status = OAuthTokenStatus::AccessDenied;
            return response;
        }
        response.status = OAuthTokenStatus::Error;
        response.error = ToWide(FindJsonStringValue(json, "error_description").value_or(*oauth_error));
        error = response.error.empty() ? L"GitHub OAuth returned an unknown error." : response.error;
        return response;
    }

    const auto access_token = FindJsonStringValue(json, "access_token");
    if (!access_token.has_value() || Trim(ToWide(*access_token)).empty())
    {
        response.status = OAuthTokenStatus::Error;
        error = L"GitHub OAuth token response does not contain access_token.";
        response.error = error;
        return response;
    }

    response.status = OAuthTokenStatus::Success;
    response.access_token = Trim(ToWide(*access_token));
    response.token_type = ToWide(FindJsonStringValue(json, "token_type").value_or(""));
    response.scope = ToWide(FindJsonStringValue(json, "scope").value_or(""));
    return response;
}

std::optional<std::wstring> ParseAuthenticatedUserJson(const std::string& json, std::wstring& error)
{
    error.clear();

    if (const auto login = FindJsonStringValue(json, "login"))
    {
        const auto value = Trim(ToWide(*login));
        if (!value.empty())
        {
            return value;
        }
    }

    error = L"GitHub user response does not contain login.";
    return std::nullopt;
}

std::optional<CopilotInternalQuotaSnapshot> ParseCopilotInternalUserJson(const std::string& json, std::wstring& error)
{
    error.clear();

    auto plan = FindJsonStringValue(json, "copilot_plan").value_or("");
    long long reset_at = 0;
    if (const auto reset_text = FindJsonStringValue(json, "quota_reset_date"))
    {
        reset_at = ParseResetTimestamp(*reset_text).value_or(0);
    }

    std::optional<CopilotInternalQuotaSnapshot> selected;
    if (const auto snapshots = FindJsonObject(json, "quota_snapshots"))
    {
        for (const auto& key : {"premium_interactions", "premium", "chat", "completions"})
        {
            if (const auto object = FindJsonObject(*snapshots, key))
            {
                selected = QuotaFromObject(key, *object);
                if (selected.has_value())
                {
                    break;
                }
            }
        }

        if (!selected.has_value())
        {
            if (const auto object = FirstNestedJsonObject(*snapshots))
            {
                selected = QuotaFromObject("quota", *object);
            }
        }
    }

    if (!selected.has_value())
    {
        const auto monthly = FindJsonObject(json, "monthly_quotas").value_or("{}");
        const auto limited = FindJsonObject(json, "limited_user_quotas").value_or("{}");
        selected = QuotaFromLimitedCounts(
            "completions",
            FindJsonDouble(monthly, "completions"),
            FindJsonDouble(limited, "completions"));
        if (!selected.has_value())
        {
            selected = QuotaFromLimitedCounts(
                "chat",
                FindJsonDouble(monthly, "chat"),
                FindJsonDouble(limited, "chat"));
        }
    }

    if (!selected.has_value())
    {
        error = L"GitHub Copilot internal response does not contain usable quota data.";
        return std::nullopt;
    }

    selected->plan = Trim(ToWide(plan));
    selected->reset_at = reset_at;
    return selected;
}

Quota CalculateQuota(double total_credits, double consumed_credits)
{
    if (!std::isfinite(total_credits) || total_credits < 0.0)
    {
        total_credits = 0.0;
    }
    if (!std::isfinite(consumed_credits) || consumed_credits < 0.0)
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

Quota CalculateQuotaFromRemaining(double total_credits, double remaining_credits, std::optional<double> remaining_percent)
{
    if (!std::isfinite(total_credits) || total_credits < 0.0)
    {
        total_credits = 0.0;
    }
    if (!std::isfinite(remaining_credits) || remaining_credits < 0.0)
    {
        remaining_credits = 0.0;
    }

    Quota quota;
    quota.total_credits = total_credits;
    quota.remaining_credits = std::min(remaining_credits, total_credits);
    quota.consumed_credits = std::max(0.0, total_credits - quota.remaining_credits);
    if (remaining_percent.has_value() && std::isfinite(*remaining_percent))
    {
        quota.remaining_percent = std::min(std::max(*remaining_percent, 0.0), 100.0);
    }
    else
    {
        quota.remaining_percent = total_credits <= 0.0 ? 0.0 : (quota.remaining_credits * 100.0 / total_credits);
    }
    return quota;
}

std::wstring FormatCreditCount(double credits)
{
    if (!std::isfinite(credits) || credits < 0.0)
    {
        credits = 0.0;
    }

    std::wostringstream stream;
    if (credits < 1000.0)
    {
        const auto rounded = static_cast<long long>(std::floor(credits + 0.5));
        stream << rounded << L"cr";
        return stream.str();
    }

    stream << std::fixed << std::setprecision(1) << (credits / 1000.0) << L"kcr";
    return stream.str();
}

std::wstring FormatPercent(double percent)
{
    if (!std::isfinite(percent) || percent < 0.0)
    {
        percent = 0.0;
    }
    const auto rounded = static_cast<long long>(std::floor(percent + 0.5));
    return std::to_wstring(rounded) + L"%";
}

std::wstring FormatResetCountdown(long long reset_at, long long now)
{
    if (reset_at <= now)
    {
        return L"now";
    }

    const auto total_minutes = (reset_at - now) / 60;
    const auto hours = total_minutes / 60;
    const auto minutes = total_minutes % 60;

    if (hours >= 24)
    {
        const auto days = hours / 24;
        const auto remaining_hours = hours % 24;
        if (remaining_hours == 0)
        {
            return std::to_wstring(days) + L"d";
        }
        return std::to_wstring(days) + L"d " + std::to_wstring(remaining_hours) + L"h";
    }

    if (hours > 0)
    {
        if (minutes == 0)
        {
            return std::to_wstring(hours) + L"h";
        }
        return std::to_wstring(hours) + L"h " + std::to_wstring(minutes) + L"m";
    }

    return std::to_wstring(minutes) + L"m";
}

std::wstring FormatResetTime(long long reset_at, long long now)
{
    const auto reset_time = static_cast<std::time_t>(reset_at);
    const auto now_time = static_cast<std::time_t>(now);

    std::tm reset_local{};
    std::tm now_local{};
    localtime_s(&reset_local, &reset_time);
    localtime_s(&now_local, &now_time);

    std::wostringstream stream;
    stream << std::setfill(L'0');
    if (reset_local.tm_year == now_local.tm_year && reset_local.tm_yday == now_local.tm_yday)
    {
        stream << std::setw(2) << reset_local.tm_hour << L":"
               << std::setw(2) << reset_local.tm_min;
        return stream.str();
    }

    stream << std::setw(2) << (reset_local.tm_mon + 1) << L"-"
           << std::setw(2) << reset_local.tm_mday << L" "
           << std::setw(2) << reset_local.tm_hour << L":"
           << std::setw(2) << reset_local.tm_min;
    return stream.str();
}

std::wstring FormatQuotaValue(const Quota& quota, long long reset_at, long long now)
{
    DisplayOptions options;
    return FormatQuotaValue(quota, reset_at, now, options);
}

std::wstring FormatQuotaValue(const Quota& quota, long long reset_at, long long now, const DisplayOptions& options)
{
    const auto percent = options.quota_display == QuotaDisplayMode::Used
        ? 100.0 - quota.remaining_percent
        : quota.remaining_percent;
    auto value = L" " + FormatPercent(percent);
    if (options.show_remaining_credits)
    {
        value += L" ";
        value += FormatCreditCount(quota.remaining_credits);
    }
    if (options.show_reset_info && reset_at > 0)
    {
        value += L" ";
        value += options.reset_display == ResetDisplayMode::Time
            ? FormatResetTime(reset_at, now)
            : FormatResetCountdown(reset_at, now);
    }
    return value;
}

float FormatResourceGraphValue(const Quota& quota, const DisplayOptions& options)
{
    (void)options;
    return ClampPercentForGraph(100.0 - quota.remaining_percent);
}
}
