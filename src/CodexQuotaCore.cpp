#include "CodexQuotaCore.h"

#include <cmath>
#include <cwchar>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace
{
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

std::wstring ToWide(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
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

std::optional<std::wstring> FindJsonStringValue(const std::wstring& json, const std::wstring& key)
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

    auto value_pos = json.find_first_not_of(L" \t\r\n", colon_pos + 1);
    if (value_pos == std::wstring::npos || json[value_pos] != L'"')
    {
        return std::nullopt;
    }
    ++value_pos;

    std::wstring value;
    bool escaped = false;
    for (auto index = value_pos; index < json.size(); ++index)
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

std::optional<std::wstring> FindJsonScalarText(const std::wstring& json, const std::wstring& key)
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

    auto value_pos = json.find_first_not_of(L" \t\r\n", colon_pos + 1);
    if (value_pos == std::wstring::npos)
    {
        return std::nullopt;
    }

    if (json[value_pos] == L'"')
    {
        return FindJsonStringValue(json, key);
    }

    const auto value_end = json.find_first_of(L",}]", value_pos);
    return Trim(json.substr(value_pos, value_end == std::wstring::npos ? std::wstring::npos : value_end - value_pos));
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

std::optional<std::string> FindJsonStringValue(const std::string& json, const std::string& key)
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

    auto value_pos = json.find_first_not_of(" \t\r\n", colon_pos + 1);
    if (value_pos == std::string::npos || json[value_pos] != '"')
    {
        return std::nullopt;
    }
    ++value_pos;

    std::string value;
    bool escaped = false;
    for (auto index = value_pos; index < json.size(); ++index)
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

std::optional<std::string> FindJsonObject(const std::string& json, const std::string& key)
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

    const auto object_start = json.find('{', colon_pos + 1);
    if (object_start == std::string::npos)
    {
        return std::nullopt;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (auto index = object_start; index < json.size(); ++index)
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
                return json.substr(object_start, index - object_start + 1);
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> FindJsonScalarText(const std::string& json, const std::string& key)
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

    auto value_pos = json.find_first_not_of(" \t\r\n", colon_pos + 1);
    if (value_pos == std::string::npos)
    {
        return std::nullopt;
    }

    if (json[value_pos] == '"')
    {
        const auto string_value = FindJsonStringValue(json.substr(value_pos - 1), key);
        if (string_value.has_value())
        {
            return *string_value;
        }
        ++value_pos;
        const auto value_end = json.find('"', value_pos);
        if (value_end == std::string::npos)
        {
            return std::nullopt;
        }
        return json.substr(value_pos, value_end - value_pos);
    }

    const auto value_end = json.find_first_of(",}\r\n\t ", value_pos);
    return Trim(json.substr(value_pos, value_end == std::string::npos ? std::string::npos : value_end - value_pos));
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
        return std::stod(*text);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<long long> FindJsonInt64(const std::string& json, const std::string& key)
{
    const auto text = FindJsonScalarText(json, key);
    if (!text.has_value())
    {
        return std::nullopt;
    }

    try
    {
        return std::stoll(*text);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<codexquota::RateWindow> ParseRateWindow(const std::string& object_json, bool required, std::wstring& error)
{
    codexquota::RateWindow window;
    window.present = true;

    const auto used_percent = FindJsonDouble(object_json, "used_percent");
    if (!used_percent.has_value())
    {
        if (required)
        {
            error = L"Missing used_percent in Codex usage response.";
        }
        return std::nullopt;
    }
    window.used_percent = *used_percent;

    if (const auto seconds = FindJsonInt64(object_json, "limit_window_seconds"))
    {
        window.limit_window_seconds = static_cast<int>(*seconds);
    }
    if (const auto reset_at = FindJsonInt64(object_json, "reset_at"))
    {
        window.reset_at = *reset_at;
    }

    return window;
}

std::wstring QuotaDisplayModeText(codexquota::QuotaDisplayMode mode)
{
    return mode == codexquota::QuotaDisplayMode::Used ? L"used" : L"remaining";
}

std::wstring ResetDisplayModeText(codexquota::ResetDisplayMode mode)
{
    return mode == codexquota::ResetDisplayMode::Time ? L"time" : L"countdown";
}

std::optional<codexquota::QuotaDisplayMode> ParseQuotaDisplayMode(const std::wstring& text)
{
    const auto value = Trim(text);
    if (value.empty() || value == L"remaining")
    {
        return codexquota::QuotaDisplayMode::Remaining;
    }
    if (value == L"used")
    {
        return codexquota::QuotaDisplayMode::Used;
    }
    return std::nullopt;
}

std::optional<codexquota::ResetDisplayMode> ParseResetDisplayMode(const std::wstring& text)
{
    const auto value = Trim(text);
    if (value.empty() || value == L"countdown")
    {
        return codexquota::ResetDisplayMode::Countdown;
    }
    if (value == L"time")
    {
        return codexquota::ResetDisplayMode::Time;
    }
    return std::nullopt;
}

}

namespace codexquota
{
std::optional<Credentials> ParseCredentialsJson(const std::wstring& json, std::wstring& error)
{
    error.clear();

    if (const auto api_key = FindJsonStringValue(json, L"OPENAI_API_KEY"))
    {
        const auto trimmed = Trim(*api_key);
        if (!trimmed.empty())
        {
            return Credentials{trimmed, L""};
        }
    }

    const auto access_token = FindJsonStringValue(json, L"access_token");
    if (!access_token.has_value() || Trim(*access_token).empty())
    {
        error = L"Codex auth.json does not contain an access token. Run codex login.";
        return std::nullopt;
    }

    Credentials credentials;
    credentials.access_token = Trim(*access_token);
    if (const auto account_id = FindJsonStringValue(json, L"account_id"))
    {
        credentials.account_id = Trim(*account_id);
    }
    return credentials;
}

std::optional<UsageSnapshot> ParseUsageJson(const std::string& json, std::wstring& error)
{
    error.clear();

    UsageSnapshot snapshot;
    if (const auto plan = FindJsonStringValue(json, "plan_type"))
    {
        snapshot.plan_type = ToWide(*plan);
    }

    const auto rate_limit = FindJsonObject(json, "rate_limit");
    if (!rate_limit.has_value())
    {
        error = L"Codex usage response does not contain rate_limit.";
        return std::nullopt;
    }

    const auto primary_object = FindJsonObject(*rate_limit, "primary_window");
    if (!primary_object.has_value())
    {
        error = L"Codex usage response does not contain primary_window.";
        return std::nullopt;
    }

    const auto primary = ParseRateWindow(*primary_object, true, error);
    if (!primary.has_value())
    {
        return std::nullopt;
    }
    snapshot.primary = *primary;

    if (const auto secondary_object = FindJsonObject(*rate_limit, "secondary_window"))
    {
        const auto secondary = ParseRateWindow(*secondary_object, false, error);
        if (secondary.has_value())
        {
            snapshot.secondary = *secondary;
        }
    }

    return snapshot;
}

std::optional<PluginConfig> ParseConfigJson(const std::wstring& json, std::wstring& error)
{
    error.clear();

    PluginConfig config;
    if (const auto quota_display = FindJsonStringValue(json, L"quota_display"))
    {
        const auto mode = ParseQuotaDisplayMode(*quota_display);
        if (!mode.has_value())
        {
            error = L"TrafficMonitor Codex quota config quota_display must be remaining or used.";
            return std::nullopt;
        }
        config.display.quota_display = *mode;
    }
    if (const auto reset_display = FindJsonStringValue(json, L"reset_display"))
    {
        const auto mode = ParseResetDisplayMode(*reset_display);
        if (!mode.has_value())
        {
            error = L"TrafficMonitor Codex quota config reset_display must be countdown or time.";
            return std::nullopt;
        }
        config.display.reset_display = *mode;
    }
    if (FindJsonScalarText(json, L"show_reset_info").has_value())
    {
        const auto show_reset_info = FindJsonBool(json, L"show_reset_info");
        if (!show_reset_info.has_value())
        {
            error = L"TrafficMonitor Codex quota config show_reset_info must be true or false.";
            return std::nullopt;
        }
        config.display.show_reset_info = *show_reset_info;
    }

    return config;
}

std::wstring SerializeConfigJson(const PluginConfig& config)
{
    std::wostringstream stream;
    stream << L"{\n"
           << L"  \"quota_display\": \"" << QuotaDisplayModeText(config.display.quota_display) << L"\",\n"
           << L"  \"reset_display\": \"" << ResetDisplayModeText(config.display.reset_display) << L"\",\n"
           << L"  \"show_reset_info\": " << (config.display.show_reset_info ? L"true" : L"false") << L"\n"
           << L"}\n";
    return stream.str();
}

std::wstring FormatPercent(double used_percent)
{
    if (std::isnan(used_percent) || used_percent < 0.0)
    {
        used_percent = 0.0;
    }
    const auto rounded = static_cast<long long>(std::floor(used_percent + 0.5));
    return std::to_wstring(rounded) + L"%";
}

std::wstring FormatRemainingPercent(double used_percent)
{
    if (std::isnan(used_percent) || used_percent < 0.0)
    {
        used_percent = 0.0;
    }

    auto remaining_percent = 100.0 - used_percent;
    if (remaining_percent < 0.0)
    {
        remaining_percent = 0.0;
    }
    return FormatPercent(remaining_percent);
}

std::wstring FormatRemainingWindowText(double used_percent, long long reset_at, long long now)
{
    DisplayOptions options;
    return FormatWindowText(used_percent, reset_at, now, options);
}

std::wstring FormatWindowText(double used_percent, long long reset_at, long long now, const DisplayOptions& options)
{
    auto text = options.quota_display == QuotaDisplayMode::Used
        ? FormatPercent(used_percent)
        : FormatRemainingPercent(used_percent);
    if (options.show_reset_info && reset_at > 0)
    {
        text += L" ";
        text += options.reset_display == ResetDisplayMode::Time
            ? FormatResetTime(reset_at, now)
            : FormatResetCountdown(reset_at, now);
    }
    return text;
}

float FormatResourceGraphValue(double used_percent, const DisplayOptions& options)
{
    const auto graph_percent = options.quota_display == QuotaDisplayMode::Used
        ? used_percent
        : 100.0 - used_percent;
    return ClampPercentForGraph(graph_percent);
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
        if (days >= 7)
        {
            const auto weeks = days / 7;
            const auto remaining_days = days % 7;
            if (remaining_days == 0)
            {
                return std::to_wstring(weeks) + L"w";
            }
            return std::to_wstring(weeks) + L"w " + std::to_wstring(remaining_days) + L"d";
        }
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

}
