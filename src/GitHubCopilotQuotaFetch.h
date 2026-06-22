#pragma once

#include "GitHubCopilotQuotaCore.h"

#include <string>

namespace githubcopilotquota
{
struct QuotaSnapshot
{
    PluginConfig config;
    Allowance allowance;
    UsagePeriod period;
    UsageReport usage;
    Quota quota;
    std::wstring username;
};

struct FetchResult
{
    bool success{};
    QuotaSnapshot snapshot;
    std::wstring error;
    int http_status{};
};

struct GitHubHttpRequest
{
    std::wstring path;
    std::wstring accept;
    std::wstring authorization;
    std::wstring api_version;
    std::wstring user_agent;
    std::wstring editor_version;
    std::wstring editor_plugin_version;
    std::wstring host;
    std::wstring method;
    std::wstring content_type;
    std::string body;
};

struct GitHubHttpResponse
{
    int http_status{};
    std::string body;
};

using GitHubHttpRequestCallback = bool (*)(
    const GitHubHttpRequest& request,
    GitHubHttpResponse& response,
    std::wstring& error,
    void* context);

using BrowserOpenCallback = bool (*)(const std::wstring& url, std::wstring& error, void* context);
using DeviceCodeCallback = bool (*)(const DeviceCodeResponse& device, std::wstring& error, void* context);
using CredentialStoreCallback = bool (*)(
    const std::wstring& token,
    const std::wstring& username,
    std::wstring& error,
    void* context);
using SleepCallback = void (*)(int seconds, void* context);

struct DeviceLoginResult
{
    bool success{};
    std::wstring username;
    std::wstring error;
    int http_status{};
};

std::wstring GetDefaultConfigPath();
std::wstring GetGitHubOAuthCredentialTarget();
std::optional<std::wstring> ReadCredentialToken(const std::wstring& target, std::wstring& error);
bool WriteCredentialToken(
    const std::wstring& target,
    const std::wstring& token,
    const std::wstring& username,
    std::wstring& error);
bool DeleteCredentialToken(const std::wstring& target, std::wstring& error);
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
    void* sleep_context);
DeviceLoginResult RunGitHubDeviceLogin(DeviceCodeCallback device_code_callback, void* device_code_context);
DeviceLoginResult RunGitHubDeviceLogin();
FetchResult FetchQuotaSnapshotFromConfigJson(
    const std::wstring& config_json,
    const std::wstring& env_token,
    long long now,
    GitHubHttpRequestCallback request_callback,
    void* request_context);
FetchResult FetchQuotaSnapshotFromConfigJsonWithStoredToken(
    const std::wstring& config_json,
    const std::wstring& env_token,
    const std::wstring& stored_token,
    long long now,
    GitHubHttpRequestCallback request_callback,
    void* request_context);
FetchResult FetchQuotaSnapshot();
}
