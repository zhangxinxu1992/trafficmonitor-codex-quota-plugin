#include "CodexQuotaCore.h"

#include <cmath>
#include <cwchar>
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
    auto text = FormatRemainingPercent(used_percent);
    if (reset_at > 0)
    {
        text += L" ";
        text += FormatResetCountdown(reset_at, now);
    }
    return text;
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

}
