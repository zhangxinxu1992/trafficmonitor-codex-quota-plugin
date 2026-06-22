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

std::wstring GetDefaultConfigPath();
FetchResult FetchQuotaSnapshotFromConfigJson(
    const std::wstring& config_json,
    const std::wstring& env_token,
    long long now,
    GitHubHttpRequestCallback request_callback,
    void* request_context);
FetchResult FetchQuotaSnapshot();
}
