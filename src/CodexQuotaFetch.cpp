#include "CodexQuotaFetch.h"

#include <Windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

namespace
{
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
        error = L"Failed to open " + path + L": " + WindowsErrorMessage(GetLastError());
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
    {
        error = L"Invalid auth.json size.";
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
        error = L"Failed to read " + path + L": " + WindowsErrorMessage(GetLastError());
        return false;
    }
    bytes.resize(bytes_read);

    if (bytes.empty())
    {
        content.clear();
        return true;
    }

    UINT code_page = CP_UTF8;
    int wide_length = MultiByteToWideChar(code_page, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wide_length <= 0)
    {
        code_page = CP_ACP;
        wide_length = MultiByteToWideChar(code_page, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (wide_length <= 0)
    {
        error = L"Failed to decode auth.json.";
        return false;
    }

    content.assign(static_cast<size_t>(wide_length), L'\0');
    MultiByteToWideChar(code_page, 0, bytes.data(), static_cast<int>(bytes.size()), content.data(), wide_length);
    return true;
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

bool AddRequestHeaders(HINTERNET request, const codexquota::Credentials& credentials, std::wstring& error)
{
    std::wstring headers = L"Authorization: Bearer " + credentials.access_token + L"\r\n";
    headers += L"Accept: application/json\r\n";
    if (!credentials.account_id.empty())
    {
        headers += L"ChatGPT-Account-Id: " + credentials.account_id + L"\r\n";
    }

    if (!WinHttpAddRequestHeaders(
            request,
            headers.c_str(),
            static_cast<DWORD>(headers.size()),
            WINHTTP_ADDREQ_FLAG_ADD))
    {
        error = L"Failed to add request headers: " + WindowsErrorMessage(GetLastError());
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
            error = L"Failed to query response data: " + WindowsErrorMessage(GetLastError());
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
            error = L"Failed to read response data: " + WindowsErrorMessage(GetLastError());
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
        error = L"Failed to read HTTP status: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    status_code = static_cast<int>(status);
    return true;
}
}

namespace codexquota
{
std::wstring GetDefaultAuthPath()
{
    const auto codex_home = GetEnvVar(L"CODEX_HOME");
    if (!codex_home.empty())
    {
        return JoinPath(codex_home, L"auth.json");
    }

    const auto user_profile = GetEnvVar(L"USERPROFILE");
    if (!user_profile.empty())
    {
        return JoinPath(JoinPath(user_profile, L".codex"), L"auth.json");
    }

    return L".codex\\auth.json";
}

FetchResult FetchUsageSnapshot()
{
    FetchResult result;

    std::wstring auth_json;
    std::wstring error;
    const auto auth_path = GetDefaultAuthPath();
    if (!ReadFileUtf8AsWide(auth_path, auth_json, error))
    {
        result.error = error;
        return result;
    }

    const auto credentials = ParseCredentialsJson(auth_json, error);
    if (!credentials.has_value())
    {
        result.error = error;
        return result;
    }

    HttpHandle session(WinHttpOpen(
        L"TrafficMonitorCodexQuota/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (session.value == nullptr)
    {
        result.error = L"Failed to open WinHTTP session: " + WindowsErrorMessage(GetLastError());
        return result;
    }

    HttpHandle connect(WinHttpConnect(session, L"chatgpt.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connect.value == nullptr)
    {
        result.error = L"Failed to connect to chatgpt.com: " + WindowsErrorMessage(GetLastError());
        return result;
    }

    HttpHandle request(WinHttpOpenRequest(
        connect,
        L"GET",
        L"/backend-api/wham/usage",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (request.value == nullptr)
    {
        result.error = L"Failed to create usage request: " + WindowsErrorMessage(GetLastError());
        return result;
    }

    WinHttpSetTimeouts(request, 10000, 10000, 10000, 30000);

    if (!AddRequestHeaders(request, *credentials, result.error))
    {
        return result;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        result.error = L"Failed to send usage request: " + WindowsErrorMessage(GetLastError());
        return result;
    }

    if (!WinHttpReceiveResponse(request, nullptr))
    {
        result.error = L"Failed to receive usage response: " + WindowsErrorMessage(GetLastError());
        return result;
    }

    if (!QueryStatusCode(request, result.http_status, result.error))
    {
        return result;
    }

    if (result.http_status == 401 || result.http_status == 403)
    {
        result.error = L"Codex authentication failed. Run codex login.";
        return result;
    }
    if (result.http_status < 200 || result.http_status >= 300)
    {
        result.error = L"Codex usage API returned HTTP " + std::to_wstring(result.http_status) + L".";
        return result;
    }

    std::string body;
    if (!ReadResponseBody(request, body, result.error))
    {
        return result;
    }

    const auto usage = ParseUsageJson(body, result.error);
    if (!usage.has_value())
    {
        return result;
    }

    result.usage = *usage;
    result.success = true;
    return result;
}
}
