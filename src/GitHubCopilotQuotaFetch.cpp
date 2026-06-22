#include "GitHubCopilotQuotaFetch.h"

#include <Windows.h>
#include <winhttp.h>

#include <ctime>
#include <string>

namespace
{
constexpr const wchar_t* kUserAgent = L"TrafficMonitorGitHubCopilotQuota/1.0";
constexpr const wchar_t* kGitHubHost = L"api.github.com";

using githubcopilotquota::GitHubHttpRequest;
using githubcopilotquota::GitHubHttpResponse;

struct HttpHandle
{
    HINTERNET value{};

    explicit HttpHandle(HINTERNET handle = nullptr) : value(handle) {}
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;

    ~HttpHandle()
    {
        if (value != nullptr)
        {
            WinHttpCloseHandle(value);
        }
    }

    operator HINTERNET() const
    {
        return value;
    }
};

struct WinHttpGitHubContext
{
    HttpHandle session;
    HttpHandle connect;
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

std::wstring WindowsErrorMessage(DWORD error_code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    if (length == 0 || buffer == nullptr)
    {
        return L"Windows error " + std::to_wstring(error_code);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L'.'))
    {
        message.pop_back();
    }
    return message;
}

std::wstring GetEnvVar(const wchar_t* name)
{
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0)
    {
        return {};
    }

    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    if (written == 0)
    {
        return {};
    }
    value.resize(written);
    return value;
}

std::wstring JoinPath(std::wstring base, const wchar_t* child)
{
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
    {
        base.push_back(L'\\');
    }
    base += child;
    return base;
}

bool ReadFileUtf8AsWide(const std::wstring& path, std::wstring& content, std::wstring& error)
{
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        {
            error = L"GitHub Copilot quota config not found: " + path;
        }
        else
        {
            error = L"Failed to open GitHub Copilot quota config " + path + L": " + WindowsErrorMessage(code);
        }
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
    {
        error = L"Invalid GitHub Copilot quota config size.";
        CloseHandle(file);
        return false;
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD bytes_read = 0;
    const BOOL ok = bytes.empty()
        || ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &bytes_read, nullptr);
    CloseHandle(file);

    if (!ok)
    {
        error = L"Failed to read GitHub Copilot quota config " + path + L": " + WindowsErrorMessage(GetLastError());
        return false;
    }
    bytes.resize(bytes_read);

    if (bytes.empty())
    {
        content.clear();
        return true;
    }

    const int wide_length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        bytes.data(),
        static_cast<int>(bytes.size()),
        nullptr,
        0);
    if (wide_length <= 0)
    {
        error = L"Failed to decode GitHub Copilot quota config as UTF-8.";
        return false;
    }

    content.assign(static_cast<size_t>(wide_length), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        bytes.data(),
        static_cast<int>(bytes.size()),
        content.data(),
        wide_length);
    return true;
}

bool EnsureWinHttpConnected(WinHttpGitHubContext& context, std::wstring& error)
{
    if (context.connect.value != nullptr)
    {
        return true;
    }

    if (context.session.value == nullptr)
    {
        context.session.value = WinHttpOpen(
            kUserAgent,
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (context.session.value == nullptr)
        {
            error = L"Failed to open WinHTTP session: " + WindowsErrorMessage(GetLastError());
            return false;
        }
    }

    context.connect.value = WinHttpConnect(context.session, kGitHubHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (context.connect.value == nullptr)
    {
        error = L"Failed to connect to api.github.com: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    return true;
}

GitHubHttpRequest MakeGitHubRequest(const std::wstring& path, const std::wstring& token)
{
    return GitHubHttpRequest{
        path,
        L"application/vnd.github+json",
        L"Bearer " + token,
        L"2026-03-10",
        kUserAgent};
}

bool AddGitHubRequestHeaders(HINTERNET request, const GitHubHttpRequest& git_hub_request, std::wstring& error)
{
    std::wstring headers = L"Accept: " + git_hub_request.accept + L"\r\n";
    headers += L"Authorization: " + git_hub_request.authorization + L"\r\n";
    headers += L"X-GitHub-Api-Version: " + git_hub_request.api_version + L"\r\n";
    headers += L"User-Agent: " + git_hub_request.user_agent;
    headers += L"\r\n";

    if (!WinHttpAddRequestHeaders(
            request,
            headers.c_str(),
            static_cast<DWORD>(headers.size()),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
    {
        error = L"Failed to add GitHub request headers: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    return true;
}

bool ReadResponseBody(HINTERNET request, std::string& body, std::wstring& error)
{
    body.clear();
    while (true)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            error = L"Failed to query GitHub response data: " + WindowsErrorMessage(GetLastError());
            return false;
        }
        if (available == 0)
        {
            return true;
        }

        std::string buffer(available, '\0');
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &bytes_read))
        {
            error = L"Failed to read GitHub response data: " + WindowsErrorMessage(GetLastError());
            return false;
        }
        body.append(buffer.data(), bytes_read);
    }
}

bool QueryStatusCode(HINTERNET request, int& status_code, std::wstring& error)
{
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &status_size,
            WINHTTP_NO_HEADER_INDEX))
    {
        error = L"Failed to read GitHub HTTP status: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    status_code = static_cast<int>(status);
    return true;
}

bool FetchGitHubJson(
    HINTERNET connect,
    const GitHubHttpRequest& git_hub_request,
    GitHubHttpResponse& git_hub_response,
    std::wstring& error)
{
    HttpHandle request(WinHttpOpenRequest(
        connect,
        L"GET",
        git_hub_request.path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (request.value == nullptr)
    {
        error = L"Failed to create GitHub request: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    WinHttpSetTimeouts(request, 10000, 10000, 10000, 30000);

    if (!AddGitHubRequestHeaders(request, git_hub_request, error))
    {
        return false;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        error = L"Failed to send GitHub request: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr))
    {
        error = L"Failed to receive GitHub response: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    if (!QueryStatusCode(request, git_hub_response.http_status, error))
    {
        return false;
    }

    if (git_hub_response.http_status < 200 || git_hub_response.http_status >= 300)
    {
        return true;
    }

    return ReadResponseBody(request, git_hub_response.body, error);
}

bool RealGitHubRequest(
    const GitHubHttpRequest& request,
    GitHubHttpResponse& response,
    std::wstring& error,
    void* context)
{
    auto* winhttp_context = static_cast<WinHttpGitHubContext*>(context);
    if (winhttp_context == nullptr)
    {
        error = L"GitHub request transport is not initialized.";
        return false;
    }
    if (!EnsureWinHttpConnected(*winhttp_context, error))
    {
        return false;
    }

    return FetchGitHubJson(winhttp_context->connect, request, response, error);
}
}

namespace githubcopilotquota
{
std::wstring GetDefaultConfigPath()
{
    const auto appdata = GetEnvVar(L"APPDATA");
    if (!appdata.empty())
    {
        return JoinPath(JoinPath(appdata, L"TrafficMonitorGitHubCopilotQuota"), L"config.json");
    }

    return L"TrafficMonitorGitHubCopilotQuota\\config.json";
}

FetchResult FetchQuotaSnapshotFromConfigJson(
    const std::wstring& config_json,
    const std::wstring& env_token,
    long long now,
    GitHubHttpRequestCallback request_callback,
    void* request_context)
{
    FetchResult result;

    auto config = ParseConfigJson(config_json, result.error);
    if (!config.has_value())
    {
        return result;
    }

    auto token = Trim(env_token);
    if (token.empty())
    {
        token = config->github_token;
    }
    if (token.empty())
    {
        result.error = L"Missing GitHub token. Set COPILOT_QUOTA_GITHUB_TOKEN or github_token in config.json.";
        return result;
    }

    auto allowance = ResolveAllowance(*config, result.error);
    if (!allowance.has_value())
    {
        return result;
    }

    if (request_callback == nullptr)
    {
        result.error = L"GitHub request transport is not initialized.";
        return result;
    }

    auto execute_request = [&](const std::wstring& path, GitHubHttpResponse& response) {
        std::wstring request_error;
        if (!request_callback(MakeGitHubRequest(path, token), response, request_error, request_context))
        {
            result.error = request_error.empty() ? L"GitHub request failed." : request_error;
            return false;
        }

        result.http_status = response.http_status;
        if (response.http_status == 401 || response.http_status == 403)
        {
            result.error = L"GitHub authentication failed or token lacks Plan read permission.";
            return false;
        }
        if (response.http_status < 200 || response.http_status >= 300)
        {
            result.error = L"GitHub API returned HTTP " + std::to_wstring(response.http_status) + L".";
            return false;
        }
        return true;
    };

    auto username = config->username;
    if (username.empty())
    {
        GitHubHttpResponse user_response;
        if (!execute_request(L"/user", user_response))
        {
            return result;
        }

        auto parsed_username = ParseAuthenticatedUserJson(user_response.body, result.error);
        if (!parsed_username.has_value())
        {
            return result;
        }
        username = *parsed_username;
    }

    const auto period = config->has_billing_day
        ? CalculateBillingPeriod(config->billing_day, now)
        : CalculateCalendarMonthEstimate(now);

    UsageReport usage;
    usage.user = username;

    if (config->has_billing_day)
    {
        for (const auto& date : period.usage_dates)
        {
            GitHubHttpResponse usage_response;
            if (!execute_request(BuildUsagePath(username, date), usage_response))
            {
                return result;
            }

            auto usage_report = ParseUsageJson(usage_response.body, result.error);
            if (!usage_report.has_value())
            {
                return result;
            }
            usage.consumed_credits += usage_report->consumed_credits;
        }
    }
    else
    {
        GitHubHttpResponse usage_response;
        if (!execute_request(BuildMonthlyUsagePath(username, period.start.year, period.start.month), usage_response))
        {
            return result;
        }

        auto usage_report = ParseUsageJson(usage_response.body, result.error);
        if (!usage_report.has_value())
        {
            return result;
        }
        usage.consumed_credits = usage_report->consumed_credits;
    }

    config->github_token.clear();
    result.snapshot.config = *config;
    result.snapshot.allowance = *allowance;
    result.snapshot.period = period;
    result.snapshot.usage = usage;
    result.snapshot.quota = CalculateQuota(allowance->total_credits, usage.consumed_credits);
    result.snapshot.username = username;
    result.success = true;
    return result;
}

FetchResult FetchQuotaSnapshot()
{
    FetchResult result;

    std::wstring config_json;
    const auto config_path = GetDefaultConfigPath();
    if (!ReadFileUtf8AsWide(config_path, config_json, result.error))
    {
        return result;
    }

    WinHttpGitHubContext context;
    return FetchQuotaSnapshotFromConfigJson(
        config_json,
        GetEnvVar(L"COPILOT_QUOTA_GITHUB_TOKEN"),
        static_cast<long long>(std::time(nullptr)),
        RealGitHubRequest,
        &context);
}
}
