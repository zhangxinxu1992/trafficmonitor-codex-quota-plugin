#include "GitHubCopilotQuotaFetch.h"

#include <Windows.h>
#include <Shellapi.h>
#include <wincred.h>
#include <winhttp.h>

#include <algorithm>
#include <ctime>
#include <optional>
#include <string>

namespace
{
constexpr const wchar_t* kUserAgent = L"TrafficMonitorGitHubCopilotQuota/1.0";
constexpr const wchar_t* kGitHubHost = L"api.github.com";
constexpr const wchar_t* kCopilotInternalUserAgent = L"GitHubCopilotChat/0.26.7";
constexpr const wchar_t* kCopilotEditorVersion = L"vscode/1.96.2";
constexpr const wchar_t* kCopilotEditorPluginVersion = L"copilot-chat/0.26.7";
constexpr const wchar_t* kGitHubOAuthCredentialTarget = L"TrafficMonitorGitHubCopilotQuota:GitHubOAuth";
constexpr const wchar_t* kGitHubWebHost = L"github.com";
constexpr const char* kGitHubOAuthClientId = "Iv1.b507a08c87ecfe98";

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

std::string NarrowAscii(const std::wstring& value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto ch : value)
    {
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

bool IsQueryUnreserved(wchar_t ch)
{
    return (ch >= L'A' && ch <= L'Z')
        || (ch >= L'a' && ch <= L'z')
        || (ch >= L'0' && ch <= L'9')
        || ch == L'-'
        || ch == L'.'
        || ch == L'_'
        || ch == L'~';
}

std::wstring UrlEncodeAscii(const std::wstring& value)
{
    constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring result;
    result.reserve(value.size());
    for (const auto ch : value)
    {
        if (IsQueryUnreserved(ch))
        {
            result.push_back(ch);
            continue;
        }

        const auto byte = static_cast<unsigned int>(ch) & 0xFF;
        result.push_back(L'%');
        result.push_back(kHex[(byte >> 4) & 0xF]);
        result.push_back(kHex[byte & 0xF]);
    }
    return result;
}

std::wstring BuildVerificationUrl(const githubcopilotquota::DeviceCodeResponse& device)
{
    if (!device.verification_uri_complete.empty())
    {
        return device.verification_uri_complete;
    }

    std::wstring url = device.verification_uri;
    url += url.find(L'?') == std::wstring::npos ? L"?" : L"&";
    url += L"user_code=";
    url += UrlEncodeAscii(device.user_code);
    return url;
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

bool ContainsHeaderControlCharacters(const std::wstring& value)
{
    return value.find_first_of(L"\r\n") != std::wstring::npos;
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

GitHubHttpRequest MakeCopilotInternalRequest(const std::wstring& token)
{
    GitHubHttpRequest request;
    request.path = L"/copilot_internal/user";
    request.accept = L"application/json";
    request.authorization = L"token " + token;
    request.api_version = L"2025-04-01";
    request.user_agent = kCopilotInternalUserAgent;
    request.editor_version = kCopilotEditorVersion;
    request.editor_plugin_version = kCopilotEditorPluginVersion;
    return request;
}

GitHubHttpRequest MakeGitHubUserRequest(const std::wstring& token)
{
    GitHubHttpRequest request;
    request.path = L"/user";
    request.accept = L"application/vnd.github+json";
    request.authorization = L"token " + token;
    request.api_version = L"2022-11-28";
    request.user_agent = kUserAgent;
    return request;
}

GitHubHttpRequest MakeDeviceCodeRequest()
{
    GitHubHttpRequest request;
    request.host = kGitHubWebHost;
    request.method = L"POST";
    request.path = L"/login/device/code";
    request.accept = L"application/json";
    request.user_agent = kUserAgent;
    request.content_type = L"application/x-www-form-urlencoded";
    request.body = std::string("client_id=") + kGitHubOAuthClientId + "&scope=read%3Auser";
    return request;
}

GitHubHttpRequest MakeAccessTokenRequest(const std::wstring& device_code)
{
    GitHubHttpRequest request;
    request.host = kGitHubWebHost;
    request.method = L"POST";
    request.path = L"/login/oauth/access_token";
    request.accept = L"application/json";
    request.user_agent = kUserAgent;
    request.content_type = L"application/x-www-form-urlencoded";
    request.body = std::string("client_id=") + kGitHubOAuthClientId
        + "&device_code=" + NarrowAscii(device_code)
        + "&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code";
    return request;
}

bool AddGitHubRequestHeaders(HINTERNET request, const GitHubHttpRequest& git_hub_request, std::wstring& error)
{
    std::wstring headers;
    if (!git_hub_request.accept.empty())
    {
        headers += L"Accept: " + git_hub_request.accept + L"\r\n";
    }
    if (!git_hub_request.authorization.empty())
    {
        headers += L"Authorization: " + git_hub_request.authorization + L"\r\n";
    }
    if (!git_hub_request.content_type.empty())
    {
        headers += L"Content-Type: " + git_hub_request.content_type + L"\r\n";
    }
    if (!git_hub_request.api_version.empty())
    {
        headers += L"X-Github-Api-Version: " + git_hub_request.api_version + L"\r\n";
    }
    if (!git_hub_request.editor_version.empty())
    {
        headers += L"Editor-Version: " + git_hub_request.editor_version + L"\r\n";
    }
    if (!git_hub_request.editor_plugin_version.empty())
    {
        headers += L"Editor-Plugin-Version: " + git_hub_request.editor_plugin_version + L"\r\n";
    }
    headers += L"User-Agent: " + (git_hub_request.user_agent.empty() ? std::wstring(kUserAgent) : git_hub_request.user_agent);
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
        git_hub_request.method.empty() ? L"GET" : git_hub_request.method.c_str(),
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

    const auto body_size = static_cast<DWORD>(git_hub_request.body.size());
    void* body = body_size == 0 ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(git_hub_request.body.data());
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body, body_size, body_size, 0))
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
    if (winhttp_context->session.value == nullptr)
    {
        winhttp_context->session.value = WinHttpOpen(
            kUserAgent,
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (winhttp_context->session.value == nullptr)
        {
            error = L"Failed to open WinHTTP session: " + WindowsErrorMessage(GetLastError());
            return false;
        }
    }

    const auto host = request.host.empty() ? std::wstring(kGitHubHost) : request.host;
    HttpHandle connect(WinHttpConnect(winhttp_context->session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connect.value == nullptr)
    {
        error = L"Failed to connect to " + host + L": " + WindowsErrorMessage(GetLastError());
        return false;
    }

    return FetchGitHubJson(connect, request, response, error);
}

bool RealOpenBrowser(const std::wstring& url, std::wstring& error, void* context)
{
    (void)context;
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32)
    {
        error = L"Failed to open GitHub authentication URL.";
        return false;
    }
    return true;
}

bool RealStoreToken(const std::wstring& token, const std::wstring& username, std::wstring& error, void* context)
{
    (void)context;
    return githubcopilotquota::WriteCredentialToken(githubcopilotquota::GetGitHubOAuthCredentialTarget(), token, username, error);
}

void RealSleepSeconds(int seconds, void* context)
{
    (void)context;
    Sleep(static_cast<DWORD>(seconds < 0 ? 0 : seconds) * 1000);
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
    return FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        config_json,
        env_token,
        L"",
        now,
        request_callback,
        request_context);
}

std::wstring GetGitHubOAuthCredentialTarget()
{
    return kGitHubOAuthCredentialTarget;
}

std::optional<std::wstring> ReadCredentialToken(const std::wstring& target, std::wstring& error)
{
    error.clear();

    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential))
    {
        const auto code = GetLastError();
        if (code == ERROR_NOT_FOUND)
        {
            return std::nullopt;
        }
        error = L"Failed to read GitHub token credential: " + WindowsErrorMessage(code);
        return std::nullopt;
    }

    std::wstring token;
    if (credential->CredentialBlobSize % sizeof(wchar_t) == 0)
    {
        token.assign(
            reinterpret_cast<const wchar_t*>(credential->CredentialBlob),
            credential->CredentialBlobSize / sizeof(wchar_t));
    }
    else
    {
        error = L"Stored GitHub token credential has invalid size.";
    }
    CredFree(credential);

    token = Trim(token);
    if (token.empty())
    {
        return std::nullopt;
    }
    return token;
}

bool WriteCredentialToken(
    const std::wstring& target,
    const std::wstring& token,
    const std::wstring& username,
    std::wstring& error)
{
    error.clear();

    const auto trimmed_token = Trim(token);
    if (trimmed_token.empty())
    {
        error = L"GitHub token is empty.";
        return false;
    }

    const auto blob_size = trimmed_token.size() * sizeof(wchar_t);
    if (blob_size > CRED_MAX_CREDENTIAL_BLOB_SIZE)
    {
        error = L"GitHub token is too large for Windows Credential Manager.";
        return false;
    }

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(blob_size);
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(trimmed_token.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = username.empty() ? nullptr : const_cast<LPWSTR>(username.c_str());

    if (!CredWriteW(&credential, 0))
    {
        error = L"Failed to write GitHub token credential: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    return true;
}

bool DeleteCredentialToken(const std::wstring& target, std::wstring& error)
{
    error.clear();
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0))
    {
        return true;
    }

    const auto code = GetLastError();
    if (code == ERROR_NOT_FOUND)
    {
        return true;
    }
    error = L"Failed to delete GitHub token credential: " + WindowsErrorMessage(code);
    return false;
}

DeviceLoginResult RunGitHubDeviceLogin(
    GitHubHttpRequestCallback request_callback,
    void* request_context,
    DeviceCodeCallback device_code_callback,
    void* device_code_context,
    BrowserOpenCallback open_callback,
    void* open_context,
    CredentialStoreCallback store_callback,
    void* store_context,
    SleepCallback sleep_callback,
    void* sleep_context)
{
    DeviceLoginResult result;
    if (request_callback == nullptr || open_callback == nullptr || store_callback == nullptr)
    {
        result.error = L"GitHub device login is not initialized.";
        return result;
    }

    GitHubHttpResponse device_response;
    std::wstring request_error;
    if (!request_callback(MakeDeviceCodeRequest(), device_response, request_error, request_context))
    {
        result.error = request_error.empty() ? L"GitHub device code request failed." : request_error;
        return result;
    }
    result.http_status = device_response.http_status;
    if (device_response.http_status < 200 || device_response.http_status >= 300)
    {
        result.error = L"GitHub device code request returned HTTP " + std::to_wstring(device_response.http_status) + L".";
        return result;
    }

    const auto device = ParseDeviceCodeJson(device_response.body, result.error);
    if (!device.has_value())
    {
        return result;
    }

    if (device_code_callback != nullptr
        && !device_code_callback(*device, result.error, device_code_context))
    {
        if (result.error.empty())
        {
            result.error = L"GitHub device code could not be shown.";
        }
        return result;
    }

    const auto verification_url = BuildVerificationUrl(*device);
    if (!open_callback(verification_url, result.error, open_context))
    {
        if (result.error.empty())
        {
            result.error = L"Failed to open GitHub authentication URL.";
        }
        return result;
    }

    auto interval = device->interval < 1 ? 1 : device->interval;
    int elapsed = 0;
    std::wstring token;
    while (elapsed < device->expires_in)
    {
        if (sleep_callback != nullptr)
        {
            sleep_callback(interval, sleep_context);
        }
        else
        {
            RealSleepSeconds(interval, nullptr);
        }
        elapsed += interval;

        GitHubHttpResponse token_response;
        if (!request_callback(MakeAccessTokenRequest(device->device_code), token_response, request_error, request_context))
        {
            result.error = request_error.empty() ? L"GitHub OAuth token request failed." : request_error;
            return result;
        }
        result.http_status = token_response.http_status;
        if (token_response.http_status < 200 || token_response.http_status >= 300)
        {
            result.error = L"GitHub OAuth token request returned HTTP " + std::to_wstring(token_response.http_status) + L".";
            return result;
        }

        std::wstring parse_error;
        const auto parsed_token = ParseAccessTokenJson(token_response.body, parse_error);
        if (parsed_token.status == OAuthTokenStatus::Success)
        {
            token = parsed_token.access_token;
            break;
        }
        if (parsed_token.status == OAuthTokenStatus::AuthorizationPending)
        {
            continue;
        }
        if (parsed_token.status == OAuthTokenStatus::SlowDown)
        {
            interval += 5;
            continue;
        }
        if (parsed_token.status == OAuthTokenStatus::ExpiredToken)
        {
            result.error = L"GitHub device login code expired.";
            return result;
        }
        if (parsed_token.status == OAuthTokenStatus::AccessDenied)
        {
            result.error = L"GitHub device login was denied.";
            return result;
        }
        result.error = parse_error.empty() ? parsed_token.error : parse_error;
        if (result.error.empty())
        {
            result.error = L"GitHub OAuth token response could not be parsed.";
        }
        return result;
    }

    if (token.empty())
    {
        result.error = L"GitHub device login code expired.";
        return result;
    }
    if (ContainsHeaderControlCharacters(token))
    {
        result.error = L"GitHub token contains invalid characters.";
        return result;
    }

    GitHubHttpResponse user_response;
    if (!request_callback(MakeGitHubUserRequest(token), user_response, request_error, request_context))
    {
        result.error = request_error.empty() ? L"GitHub user verification request failed." : request_error;
        return result;
    }
    result.http_status = user_response.http_status;
    if (user_response.http_status == 401 || user_response.http_status == 403)
    {
        result.error = L"GitHub authentication failed.";
        return result;
    }
    if (user_response.http_status < 200 || user_response.http_status >= 300)
    {
        result.error = L"GitHub user verification returned HTTP " + std::to_wstring(user_response.http_status) + L".";
        return result;
    }

    const auto username = ParseAuthenticatedUserJson(user_response.body, result.error);
    if (!username.has_value())
    {
        return result;
    }

    GitHubHttpResponse quota_response;
    if (!request_callback(MakeCopilotInternalRequest(token), quota_response, request_error, request_context))
    {
        result.error = request_error.empty() ? L"GitHub Copilot verification request failed." : request_error;
        return result;
    }
    result.http_status = quota_response.http_status;
    if (quota_response.http_status == 401 || quota_response.http_status == 403)
    {
        result.error = L"GitHub Copilot authentication failed.";
        return result;
    }
    if (quota_response.http_status < 200 || quota_response.http_status >= 300)
    {
        result.error = L"GitHub Copilot verification returned HTTP " + std::to_wstring(quota_response.http_status) + L".";
        return result;
    }

    std::wstring quota_error;
    const auto quota = ParseCopilotInternalUserJson(quota_response.body, quota_error);
    if (!quota.has_value())
    {
        result.error = quota_error;
        return result;
    }

    if (!store_callback(token, *username, result.error, store_context))
    {
        if (result.error.empty())
        {
            result.error = L"Failed to store GitHub token.";
        }
        return result;
    }

    result.username = *username;
    result.success = true;
    result.error.clear();
    return result;
}

DeviceLoginResult RunGitHubDeviceLogin(DeviceCodeCallback device_code_callback, void* device_code_context)
{
    WinHttpGitHubContext context;
    return RunGitHubDeviceLogin(
        RealGitHubRequest,
        &context,
        device_code_callback,
        device_code_context,
        RealOpenBrowser,
        nullptr,
        RealStoreToken,
        nullptr,
        RealSleepSeconds,
        nullptr);
}

DeviceLoginResult RunGitHubDeviceLogin()
{
    return RunGitHubDeviceLogin(nullptr, nullptr);
}

FetchResult FetchQuotaSnapshotFromConfigJsonWithStoredToken(
    const std::wstring& config_json,
    const std::wstring& env_token,
    const std::wstring& stored_token,
    long long now,
    GitHubHttpRequestCallback request_callback,
    void* request_context)
{
    (void)now;

    FetchResult result;

    auto config = ParseConfigJson(config_json, result.error);
    if (!config.has_value())
    {
        return result;
    }

    const auto token_choice = ResolveGitHubToken(env_token, stored_token, *config, result.error);
    if (!token_choice.has_value())
    {
        return result;
    }
    const auto token = token_choice->token;
    if (ContainsHeaderControlCharacters(token))
    {
        result.error = L"GitHub token contains invalid characters.";
        return result;
    }

    if (request_callback == nullptr)
    {
        result.error = L"GitHub request transport is not initialized.";
        return result;
    }

    auto execute_request = [&](GitHubHttpResponse& response) {
        std::wstring request_error;
        if (!request_callback(MakeCopilotInternalRequest(token), response, request_error, request_context))
        {
            result.error = request_error.empty() ? L"GitHub request failed." : request_error;
            return false;
        }

        result.http_status = response.http_status;
        if (response.http_status == 401 || response.http_status == 403)
        {
            result.error = L"GitHub Copilot authentication failed.";
            return false;
        }
        if (response.http_status < 200 || response.http_status >= 300)
        {
            result.error = L"GitHub Copilot API returned HTTP " + std::to_wstring(response.http_status) + L".";
            return false;
        }
        return true;
    };

    GitHubHttpResponse usage_response;
    if (!execute_request(usage_response))
    {
        return result;
    }

    const auto internal_snapshot = ParseCopilotInternalUserJson(usage_response.body, result.error);
    if (!internal_snapshot.has_value())
    {
        return result;
    }

    auto quota = CalculateQuotaFromRemaining(
        internal_snapshot->total_credits,
        internal_snapshot->remaining_credits,
        internal_snapshot->remaining_percent);

    UsagePeriod period;
    period.reset_at = internal_snapshot->reset_at;
    period.is_copilot_internal = true;

    UsageReport usage;
    usage.user = config->username;
    usage.consumed_credits = quota.consumed_credits;

    config->github_token.clear();
    if (config->plan.empty())
    {
        config->plan = internal_snapshot->plan;
    }
    result.snapshot.config = *config;
    result.snapshot.allowance = Allowance{
        internal_snapshot->total_credits,
        internal_snapshot->quota_id.empty() ? L"copilot_internal" : L"copilot_internal:" + internal_snapshot->quota_id};
    result.snapshot.period = period;
    result.snapshot.usage = usage;
    result.snapshot.quota = quota;
    result.snapshot.username = config->username;
    result.success = true;
    return result;
}

FetchResult FetchQuotaSnapshot()
{
    FetchResult result;

    std::wstring config_json;
    const auto env_token = GetEnvVar(L"COPILOT_QUOTA_GITHUB_TOKEN");
    std::wstring credential_error;
    const auto skip_stored_credential = !GetEnvVar(L"GITHUB_COPILOT_QUOTA_SKIP_STORED_CREDENTIAL").empty();
    const auto stored_token = skip_stored_credential
        ? std::wstring()
        : ReadCredentialToken(GetGitHubOAuthCredentialTarget(), credential_error).value_or(L"");
    const auto config_path = GetDefaultConfigPath();
    if (!ReadFileUtf8AsWide(config_path, config_json, result.error))
    {
        if ((Trim(env_token).empty() && Trim(stored_token).empty()) || result.error.find(L"config not found") == std::wstring::npos)
        {
            return result;
        }
        config_json.clear();
    }

    WinHttpGitHubContext context;
    return FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        config_json,
        env_token,
        stored_token,
        static_cast<long long>(std::time(nullptr)),
        RealGitHubRequest,
        &context);
}
}
