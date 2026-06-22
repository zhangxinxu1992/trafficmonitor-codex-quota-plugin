#include "GitHubCopilotQuotaCore.h"

#include <algorithm>
#include <cmath>
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

bool IsJsonWhitespace(wchar_t ch)
{
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

bool IsJsonWhitespace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
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

std::optional<long long> FindJsonInt64(const std::wstring& json, const std::wstring& key)
{
    const auto text = FindJsonScalarText(json, key);
    if (!text.has_value())
    {
        return std::nullopt;
    }

    try
    {
        std::size_t parsed{};
        const auto value = std::stoll(*text, &parsed);
        if (!HasOnlyTrailingWhitespace(*text, parsed))
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

std::optional<std::string> FindJsonArray(const std::string& json, const std::string& key)
{
    const auto value_pos = FindJsonValueStart(json, key);
    if (!value_pos.has_value() || json[*value_pos] != '[')
    {
        return std::nullopt;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (auto index = *value_pos; index < json.size(); ++index)
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
        else if (ch == '[')
        {
            ++depth;
        }
        else if (ch == ']')
        {
            --depth;
            if (depth == 0)
            {
                return json.substr(*value_pos, index - *value_pos + 1);
            }
        }
    }

    return std::nullopt;
}

std::vector<std::string> ExtractJsonObjects(const std::string& array_json)
{
    std::vector<std::string> objects;
    bool in_string = false;
    bool escaped = false;
    int object_depth = 0;
    std::size_t object_start = std::string::npos;

    for (std::size_t index = 0; index < array_json.size(); ++index)
    {
        const auto ch = array_json[index];
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
            if (object_depth == 0)
            {
                object_start = index;
            }
            ++object_depth;
        }
        else if (ch == '}' && object_depth > 0)
        {
            --object_depth;
            if (object_depth == 0 && object_start != std::string::npos)
            {
                objects.push_back(array_json.substr(object_start, index - object_start + 1));
                object_start = std::string::npos;
            }
        }
    }

    return objects;
}

bool Contains(const std::string& value, const std::string& needle)
{
    return value.find(needle) != std::string::npos;
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

githubcopilotquota::Date AddMonths(const githubcopilotquota::Date& date, int months)
{
    auto year = date.year;
    auto month = date.month + months;
    while (month > 12)
    {
        month -= 12;
        ++year;
    }
    while (month < 1)
    {
        month += 12;
        --year;
    }

    const auto day = std::min(date.day, DaysInMonth(year, month));
    return githubcopilotquota::Date{year, month, day};
}

githubcopilotquota::Date BillingDate(int year, int month, int billing_day)
{
    const auto day = std::min(std::max(billing_day, 1), DaysInMonth(year, month));
    return githubcopilotquota::Date{year, month, day};
}

githubcopilotquota::Date DateFromTimestamp(long long timestamp)
{
    const auto time_value = static_cast<std::time_t>(timestamp);
    std::tm utc{};
    gmtime_s(&utc, &time_value);
    return githubcopilotquota::Date{utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday};
}

long long TimestampFromDate(const githubcopilotquota::Date& date)
{
    std::tm utc{};
    utc.tm_year = date.year - 1900;
    utc.tm_mon = date.month - 1;
    utc.tm_mday = date.day;
    utc.tm_isdst = 0;
    return static_cast<long long>(::_mkgmtime(&utc));
}

githubcopilotquota::Date AddDays(const githubcopilotquota::Date& date, int days)
{
    return DateFromTimestamp(TimestampFromDate(date) + static_cast<long long>(days) * 24 * 60 * 60);
}

std::vector<githubcopilotquota::Date> BuildDateRange(const githubcopilotquota::Date& start, const githubcopilotquota::Date& end)
{
    std::vector<githubcopilotquota::Date> dates;
    const auto start_time = TimestampFromDate(start);
    const auto end_time = TimestampFromDate(end);
    for (auto time_value = start_time; time_value < end_time; time_value += 24 * 60 * 60)
    {
        dates.push_back(DateFromTimestamp(time_value));
    }
    return dates;
}

}

namespace githubcopilotquota
{
std::optional<PluginConfig> ParseConfigJson(const std::wstring& json, std::wstring& error)
{
    error.clear();

    PluginConfig config;
    if (const auto token = FindJsonStringValue(json, L"github_token"))
    {
        config.github_token = Trim(*token);
    }
    if (const auto username = FindJsonStringValue(json, L"username"))
    {
        config.username = Trim(*username);
    }
    if (const auto plan = FindJsonStringValue(json, L"plan"))
    {
        config.plan = Trim(*plan);
    }
    if (FindJsonValueStart(json, L"total_credits").has_value())
    {
        const auto total_credits = FindJsonDouble(json, L"total_credits");
        if (!total_credits.has_value())
        {
            error = L"GitHub Copilot quota config total_credits must be a valid number.";
            return std::nullopt;
        }
        config.total_credits = *total_credits;
    }
    if (FindJsonValueStart(json, L"billing_day").has_value())
    {
        const auto billing_day = FindJsonInt64(json, L"billing_day");
        if (!billing_day.has_value())
        {
            error = L"GitHub Copilot quota config billing_day must be an integer from 1 to 31.";
            return std::nullopt;
        }
        if (*billing_day >= 1 && *billing_day <= 31)
        {
            config.billing_day = static_cast<int>(*billing_day);
            config.has_billing_day = true;
        }
        else
        {
            error = L"GitHub Copilot quota config billing_day must be an integer from 1 to 31.";
            return std::nullopt;
        }
    }

    return config;
}

std::optional<Allowance> ResolveAllowance(const PluginConfig& config, std::wstring& error)
{
    error.clear();
    if (config.total_credits > 0.0)
    {
        return Allowance{config.total_credits, L"total_credits"};
    }
    if (config.plan == L"pro")
    {
        return Allowance{1500.0, L"plan:pro"};
    }
    if (config.plan == L"pro_plus")
    {
        return Allowance{7000.0, L"plan:pro_plus"};
    }
    if (config.plan == L"max")
    {
        return Allowance{20000.0, L"plan:max"};
    }

    error = L"GitHub Copilot quota config requires total_credits or a known plan.";
    return std::nullopt;
}

std::optional<UsageReport> ParseUsageJson(const std::string& json, std::wstring& error)
{
    error.clear();

    const auto usage_items = FindJsonArray(json, "usageItems");
    if (!usage_items.has_value())
    {
        error = L"GitHub usage response does not contain usageItems.";
        return std::nullopt;
    }

    UsageReport report;
    if (const auto user = FindJsonStringValue(json, "user"))
    {
        report.user = ToWide(*user);
    }

    for (const auto& item : ExtractJsonObjects(*usage_items))
    {
        const auto product = FindJsonStringValue(item, "product").value_or("");
        const auto sku = FindJsonStringValue(item, "sku").value_or("");
        if (!Contains(product, "Copilot AI Credits") && !Contains(sku, "AI Credit"))
        {
            continue;
        }

        if (FindJsonValueStart(item, "netQuantity").has_value())
        {
            const auto net_quantity = FindJsonDouble(item, "netQuantity");
            if (!net_quantity.has_value())
            {
                error = L"GitHub usage response contains malformed netQuantity.";
                return std::nullopt;
            }
            report.consumed_credits += *net_quantity;
        }
    }

    return report;
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

UsagePeriod CalculateBillingPeriod(int billing_day, long long now)
{
    billing_day = std::min(std::max(billing_day, 1), 31);

    UsagePeriod period;
    const auto current = DateFromTimestamp(now);
    const auto candidate_start = BillingDate(current.year, current.month, billing_day);
    const auto candidate_start_time = TimestampFromDate(candidate_start);

    if (now >= candidate_start_time)
    {
        period.start = candidate_start;
        const auto next_month = AddMonths(Date{current.year, current.month, 1}, 1);
        period.end = BillingDate(next_month.year, next_month.month, billing_day);
    }
    else
    {
        period.end = candidate_start;
        const auto previous_month = AddMonths(Date{current.year, current.month, 1}, -1);
        period.start = BillingDate(previous_month.year, previous_month.month, billing_day);
    }

    period.reset_at = TimestampFromDate(period.end);
    auto usage_end = AddDays(current, 1);
    if (TimestampFromDate(usage_end) > TimestampFromDate(period.end))
    {
        usage_end = period.end;
    }
    period.usage_dates = BuildDateRange(period.start, usage_end);
    period.is_calendar_month_estimate = false;
    return period;
}

UsagePeriod CalculateCalendarMonthEstimate(long long now)
{
    UsagePeriod period;
    const auto current = DateFromTimestamp(now);
    period.start = Date{current.year, current.month, 1};
    const auto next_month = AddMonths(period.start, 1);
    period.end = Date{next_month.year, next_month.month, 1};
    period.reset_at = 0;
    period.is_calendar_month_estimate = true;
    return period;
}

std::wstring BuildUsagePath(const std::wstring& username, const Date& date)
{
    std::wostringstream stream;
    stream << L"/users/" << username << L"/settings/billing/ai_credit/usage?year=" << date.year
           << L"&month=" << date.month << L"&day=" << date.day;
    return stream.str();
}

std::wstring BuildMonthlyUsagePath(const std::wstring& username, int year, int month)
{
    std::wostringstream stream;
    stream << L"/users/" << username << L"/settings/billing/ai_credit/usage?year=" << year
           << L"&month=" << month;
    return stream.str();
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

std::wstring FormatQuotaValue(const Quota& quota, long long reset_at, long long now)
{
    auto value = L" " + FormatPercent(quota.remaining_percent) + L" " + FormatCreditCount(quota.remaining_credits);
    if (reset_at > 0)
    {
        value += L" ";
        value += FormatResetCountdown(reset_at, now);
    }
    return value;
}
}
