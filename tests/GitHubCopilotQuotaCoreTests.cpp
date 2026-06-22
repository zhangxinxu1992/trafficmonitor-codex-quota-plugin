#include "GitHubCopilotQuotaCore.h"
#include "GitHubCopilotQuotaFetch.h"

#include <Windows.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void CheckNear(double actual, double expected, const char* message)
{
    Check(std::fabs(actual - expected) < 0.0001, message);
}

bool IsOnOrBefore(const githubcopilotquota::Date& actual, const githubcopilotquota::Date& expected)
{
    if (actual.year != expected.year)
    {
        return actual.year < expected.year;
    }
    if (actual.month != expected.month)
    {
        return actual.month < expected.month;
    }
    return actual.day <= expected.day;
}

struct FakeGitHubTransport
{
    std::vector<githubcopilotquota::GitHubHttpRequest> requests;
    std::vector<githubcopilotquota::GitHubHttpResponse> responses;
};

struct FakeDeviceLoginCallbacks
{
    std::vector<std::wstring> opened_urls;
    std::vector<std::wstring> user_codes;
    std::vector<int> sleeps;
    std::vector<std::pair<std::wstring, std::wstring>> stored_tokens;
};

bool FakeGitHubRequest(
    const githubcopilotquota::GitHubHttpRequest& request,
    githubcopilotquota::GitHubHttpResponse& response,
    std::wstring& error,
    void* context)
{
    auto* fake = static_cast<FakeGitHubTransport*>(context);
    fake->requests.push_back(request);

    const auto response_index = fake->requests.size() - 1;
    if (response_index >= fake->responses.size())
    {
        error = L"Unexpected GitHub request: " + request.path;
        return false;
    }

    response = fake->responses[response_index];
    return true;
}

bool FakeOpenBrowser(const std::wstring& url, std::wstring& error, void* context)
{
    (void)error;
    auto* fake = static_cast<FakeDeviceLoginCallbacks*>(context);
    fake->opened_urls.push_back(url);
    return true;
}

bool FakeShowDeviceCode(const githubcopilotquota::DeviceCodeResponse& device, std::wstring& error, void* context)
{
    (void)error;
    auto* fake = static_cast<FakeDeviceLoginCallbacks*>(context);
    fake->user_codes.push_back(device.user_code);
    return true;
}

bool FakeStoreToken(const std::wstring& token, const std::wstring& username, std::wstring& error, void* context)
{
    (void)error;
    auto* fake = static_cast<FakeDeviceLoginCallbacks*>(context);
    fake->stored_tokens.push_back({token, username});
    return true;
}

void FakeSleep(int seconds, void* context)
{
    auto* fake = static_cast<FakeDeviceLoginCallbacks*>(context);
    fake->sleeps.push_back(seconds);
}

githubcopilotquota::GitHubHttpResponse FakeUsageResponse(double credits)
{
    return githubcopilotquota::GitHubHttpResponse{
        200,
        R"({"user":"octocat","usageItems":[{"product":"Copilot AI Credits","sku":"AI Credit","netQuantity":")"
            + std::to_string(credits)
            + R"("}]})"};
}

std::vector<githubcopilotquota::GitHubHttpResponse> FakeUsageResponses(int count, double credits)
{
    std::vector<githubcopilotquota::GitHubHttpResponse> responses;
    for (auto index = 0; index < count; ++index)
    {
        responses.push_back(FakeUsageResponse(credits));
    }
    return responses;
}

githubcopilotquota::GitHubHttpResponse FakeCopilotInternalResponse()
{
    return githubcopilotquota::GitHubHttpResponse{
        200,
        R"({
            "copilot_plan": "pro",
            "quota_reset_date": "2026-07-01",
            "quota_snapshots": {
                "premium_interactions": {
                    "entitlement": 300,
                    "remaining": 240,
                    "percent_remaining": 80,
                    "quota_id": "premium_interactions"
                },
                "chat": {
                    "entitlement": 1000,
                    "remaining": 900,
                    "percent_remaining": 90,
                    "quota_id": "chat"
                }
            }
        })"};
}

void TestResolvesGitHubTokenPrecedence()
{
    githubcopilotquota::PluginConfig config;
    config.github_token = L" config-token ";

    std::wstring error;
    const auto env_choice = githubcopilotquota::ResolveGitHubToken(L" env-token ", L" stored-token ", config, error);
    Check(env_choice.has_value(), "env token choice should resolve");
    Check(env_choice->token == L"env-token", "env token should be trimmed and preferred");
    Check(env_choice->source == githubcopilotquota::GitHubTokenSource::Environment, "env token source should be reported");

    error.clear();
    const auto stored_choice = githubcopilotquota::ResolveGitHubToken(L"", L" stored-token ", config, error);
    Check(stored_choice.has_value(), "stored token choice should resolve");
    Check(stored_choice->token == L"stored-token", "stored token should be trimmed and preferred over config");
    Check(stored_choice->source == githubcopilotquota::GitHubTokenSource::StoredCredential, "stored token source should be reported");

    error.clear();
    const auto config_choice = githubcopilotquota::ResolveGitHubToken(L"", L"", config, error);
    Check(config_choice.has_value(), "config token choice should resolve");
    Check(config_choice->token == L"config-token", "config token should be trimmed");
    Check(config_choice->source == githubcopilotquota::GitHubTokenSource::Config, "config token source should be reported");

    config.github_token.clear();
    error.clear();
    const auto missing_choice = githubcopilotquota::ResolveGitHubToken(L" ", L" ", config, error);
    Check(!missing_choice.has_value(), "missing token choice should fail");
    Check(error == L"Missing GitHub token. Sign in from plugin options, set COPILOT_QUOTA_GITHUB_TOKEN, or set github_token in config.json.",
        "missing token error should mention options sign-in");
}

void TestParsesDeviceCodeResponse()
{
    std::wstring error;
    const auto response = githubcopilotquota::ParseDeviceCodeJson(
        R"({
            "device_code": "device-code",
            "user_code": "ABCD-EFGH",
            "verification_uri": "https://github.com/login/device",
            "verification_uri_complete": "https://github.com/login/device?user_code=ABCD-EFGH",
            "expires_in": 900,
            "interval": 5
        })",
        error);

    Check(response.has_value(), "device code JSON should parse");
    Check(response->device_code == L"device-code", "device code should parse");
    Check(response->user_code == L"ABCD-EFGH", "user code should parse");
    Check(response->verification_uri == L"https://github.com/login/device", "verification URI should parse");
    Check(response->verification_uri_complete == L"https://github.com/login/device?user_code=ABCD-EFGH",
        "complete verification URI should parse");
    Check(response->expires_in == 900, "device code expiry should parse");
    Check(response->interval == 5, "device code interval should parse");
    Check(error.empty(), "successful device code parse should not set error");
}

void TestParsesAccessTokenResponse()
{
    std::wstring error;
    const auto response = githubcopilotquota::ParseAccessTokenJson(
        R"({"access_token":"oauth-token","token_type":"bearer","scope":"read:user"})",
        error);

    Check(response.status == githubcopilotquota::OAuthTokenStatus::Success, "access token JSON should parse as success");
    Check(response.access_token == L"oauth-token", "access token should parse");
    Check(response.token_type == L"bearer", "token type should parse");
    Check(response.scope == L"read:user", "scope should parse");
    Check(error.empty(), "successful access token parse should not set error");
}

void TestParsesAccessTokenPendingAndSlowDownErrors()
{
    std::wstring error;
    const auto pending = githubcopilotquota::ParseAccessTokenJson(
        R"({"error":"authorization_pending","error_description":"pending"})",
        error);
    Check(pending.status == githubcopilotquota::OAuthTokenStatus::AuthorizationPending,
        "authorization_pending should parse as pending status");
    Check(error.empty(), "pending access token parse should not set fatal error");

    const auto slow_down = githubcopilotquota::ParseAccessTokenJson(
        R"({"error":"slow_down","error_description":"slow down"})",
        error);
    Check(slow_down.status == githubcopilotquota::OAuthTokenStatus::SlowDown,
        "slow_down should parse as slow down status");
    Check(error.empty(), "slow down access token parse should not set fatal error");
}

void TestCredentialTokenRoundTripWithTestTarget()
{
    std::wstring error;
    const auto target = L"TrafficMonitorGitHubCopilotQuota:Test:" + std::to_wstring(GetCurrentProcessId());

    Check(githubcopilotquota::DeleteCredentialToken(target, error), "pre-test credential cleanup should succeed");
    error.clear();

    Check(githubcopilotquota::WriteCredentialToken(target, L"stored-oauth-token", L"octocat", error),
        "credential token write should succeed");
    Check(error.empty(), "successful credential token write should not set error");

    const auto token = githubcopilotquota::ReadCredentialToken(target, error);
    Check(token.has_value(), "credential token read should succeed");
    Check(token.value_or(L"") == L"stored-oauth-token", "credential token read should return original token");

    error.clear();
    Check(githubcopilotquota::DeleteCredentialToken(target, error), "credential token delete should succeed");
    Check(error.empty(), "successful credential token delete should not set error");

    const auto deleted_token = githubcopilotquota::ReadCredentialToken(target, error);
    Check(!deleted_token.has_value(), "deleted credential token should not be found");
}

void TestFetchUsesStoredTokenBeforeConfigToken()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        LR"({"github_token":"config-token"})",
        L"",
        L"stored-token",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed with stored token");
    Check(fake.requests.size() == 1, "stored-token fetch should issue one request");
    Check(!fake.requests.empty() && fake.requests.front().authorization == L"token stored-token",
        "stored token should be used before config token");
}

void TestFetchEnvTokenOverridesStoredToken()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        LR"({"github_token":"config-token"})",
        L"env-token",
        L"stored-token",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed with env token and stored token present");
    Check(fake.requests.size() == 1, "env-token fetch should issue one request");
    Check(!fake.requests.empty() && fake.requests.front().authorization == L"token env-token",
        "env token should override stored token");
}

void TestRunGitHubDeviceLoginStoresTokenAfterVerification()
{
    FakeGitHubTransport transport;
    transport.responses.push_back(githubcopilotquota::GitHubHttpResponse{
        200,
        R"({
            "device_code": "device-code",
            "user_code": "ABCD-EFGH",
            "verification_uri": "https://github.com/login/device",
            "verification_uri_complete": "https://github.com/login/device?user_code=ABCD-EFGH",
            "expires_in": 900,
            "interval": 5
        })"});
    transport.responses.push_back(githubcopilotquota::GitHubHttpResponse{
        200,
        R"({"error":"authorization_pending"})"});
    transport.responses.push_back(githubcopilotquota::GitHubHttpResponse{
        200,
        R"({"access_token":"oauth-token","token_type":"bearer","scope":"read:user"})"});
    transport.responses.push_back(githubcopilotquota::GitHubHttpResponse{
        200,
        R"({"login":"octocat"})"});
    transport.responses.push_back(FakeCopilotInternalResponse());

    FakeDeviceLoginCallbacks callbacks;
    const auto result = githubcopilotquota::RunGitHubDeviceLogin(
        FakeGitHubRequest,
        &transport,
        FakeShowDeviceCode,
        &callbacks,
        FakeOpenBrowser,
        &callbacks,
        FakeStoreToken,
        &callbacks,
        FakeSleep,
        &callbacks);

    Check(result.success, "device login should succeed after browser authorization");
    Check(result.username == L"octocat", "device login should return verified username");
    Check(callbacks.user_codes.size() == 1 && callbacks.user_codes.front() == L"ABCD-EFGH",
        "device login should show the user code before polling");
    Check(callbacks.opened_urls.size() == 1, "device login should open one browser URL");
    Check(!callbacks.opened_urls.empty()
            && callbacks.opened_urls.front() == L"https://github.com/login/device?user_code=ABCD-EFGH",
        "device login should prefer complete verification URL");
    Check(callbacks.sleeps.size() == 2, "device login should sleep before each token poll");
    Check(callbacks.sleeps.size() == 2 && callbacks.sleeps[0] == 5 && callbacks.sleeps[1] == 5,
        "device login should use GitHub polling interval");
    Check(callbacks.stored_tokens.size() == 1, "device login should store one token");
    Check(!callbacks.stored_tokens.empty() && callbacks.stored_tokens.front().first == L"oauth-token",
        "device login should store OAuth token");
    Check(!callbacks.stored_tokens.empty() && callbacks.stored_tokens.front().second == L"octocat",
        "device login should store verified username with token");
    Check(transport.requests.size() == 5, "device login should issue device, poll, poll, user, and quota requests");
    Check(transport.requests.size() == 5 && transport.requests[0].host == L"github.com",
        "device code request should target github.com");
    Check(transport.requests.size() == 5 && transport.requests[0].method == L"POST",
        "device code request should use POST");
    Check(transport.requests.size() == 5 && transport.requests[1].path == L"/login/oauth/access_token",
        "token poll request should use OAuth access token path");
    Check(transport.requests.size() == 5 && transport.requests[3].path == L"/user",
        "device login should verify identity before storing token");
    Check(transport.requests.size() == 5 && transport.requests[4].path == L"/copilot_internal/user",
        "device login should verify Copilot quota before storing token");
}

void TestRunGitHubDeviceLoginBuildsCompleteUrlWhenGitHubOmitsIt()
{
    FakeGitHubTransport transport;
    transport.responses.push_back(githubcopilotquota::GitHubHttpResponse{
        200,
        R"({
            "device_code": "device-code",
            "user_code": "WXYZ-1234",
            "verification_uri": "https://github.com/login/device",
            "expires_in": 900,
            "interval": 5
        })"});
    transport.responses.push_back(githubcopilotquota::GitHubHttpResponse{
        200,
        R"({"access_token":"oauth-token","token_type":"bearer","scope":"read:user"})"});
    transport.responses.push_back(githubcopilotquota::GitHubHttpResponse{
        200,
        R"({"login":"octocat"})"});
    transport.responses.push_back(FakeCopilotInternalResponse());

    FakeDeviceLoginCallbacks callbacks;
    const auto result = githubcopilotquota::RunGitHubDeviceLogin(
        FakeGitHubRequest,
        &transport,
        FakeShowDeviceCode,
        &callbacks,
        FakeOpenBrowser,
        &callbacks,
        FakeStoreToken,
        &callbacks,
        FakeSleep,
        &callbacks);

    Check(result.success, "device login should succeed when GitHub omits verification_uri_complete");
    Check(callbacks.user_codes.size() == 1 && callbacks.user_codes.front() == L"WXYZ-1234",
        "device login should expose the user code when GitHub omits complete URI");
    Check(callbacks.opened_urls.size() == 1
            && callbacks.opened_urls.front() == L"https://github.com/login/device?user_code=WXYZ-1234",
        "device login should build a complete verification URL from verification_uri and user_code");
}

void TestParsesConfigWithExplicitAllowance()
{
    std::wstring error;
    const auto config = githubcopilotquota::ParseConfigJson(
        LR"({
            "github_token": "config-token",
            "username": "octocat",
            "plan": "pro",
            "total_credits": 2345,
            "billing_day": 15
        })",
        error);

    Check(config.has_value(), "config with explicit allowance should parse");
    Check(config->github_token == L"config-token", "config token should parse");
    Check(config->username == L"octocat", "username should parse");
    Check(config->plan == L"pro", "plan should parse");
    CheckNear(config->total_credits, 2345.0, "total credits should parse");
    Check(config->has_billing_day, "billing day should be marked present");
    Check(config->billing_day == 15, "billing day should parse");
    Check(error.empty(), "successful config parse should not set error");
}

void TestRejectsMalformedConfigTotalCredits()
{
    std::wstring error;
    const auto config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": "1500abc"
        })",
        error);

    Check(!config.has_value(), "malformed total_credits should fail config parsing");
    Check(!error.empty(), "malformed total_credits should set error");
}

void TestRejectsNonFiniteConfigTotalCredits()
{
    std::wstring nan_error;
    const auto nan_config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": "nan"
        })",
        nan_error);

    Check(!nan_config.has_value(), "nan total_credits should fail config parsing");
    Check(!nan_error.empty(), "nan total_credits should set error");

    std::wstring inf_error;
    const auto inf_config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": "inf"
        })",
        inf_error);

    Check(!inf_config.has_value(), "inf total_credits should fail config parsing");
    Check(!inf_error.empty(), "inf total_credits should set error");
}

void TestRejectsUnquotedWhitespaceGarbageTotalCredits()
{
    std::wstring error;
    const auto config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": 1500 abc
        })",
        error);

    Check(!config.has_value(), "unquoted total_credits with whitespace garbage should fail config parsing");
    Check(!error.empty(), "unquoted total_credits with whitespace garbage should set error");
}

void TestRejectsFractionalBillingDay()
{
    std::wstring error;
    const auto config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": 1500,
            "billing_day": 15.9
        })",
        error);

    Check(!config.has_value(), "fractional billing_day should fail config parsing");
    Check(!error.empty(), "fractional billing_day should set error");
}

void TestRejectsUnquotedWhitespaceGarbageBillingDay()
{
    std::wstring error;
    const auto config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "plan": "pro",
            "total_credits": 1500,
            "billing_day": 15 abc
        })",
        error);

    Check(!config.has_value(), "unquoted billing_day with whitespace garbage should fail config parsing");
    Check(!error.empty(), "unquoted billing_day with whitespace garbage should set error");
}

void TestResolvesAllowanceFromPlan()
{
    githubcopilotquota::PluginConfig config;
    config.plan = L"pro_plus";

    std::wstring error;
    const auto allowance = githubcopilotquota::ResolveAllowance(config, error);

    Check(allowance.has_value(), "known plan should resolve allowance");
    CheckNear(allowance->total_credits, 7000.0, "pro_plus allowance should be 7000");
    Check(allowance->source == L"plan:pro_plus", "allowance source should name plan");
    Check(error.empty(), "successful allowance resolution should not set error");
}

void TestExplicitAllowanceOverridesPlan()
{
    githubcopilotquota::PluginConfig config;
    config.plan = L"pro_plus";
    config.total_credits = 2345.0;

    std::wstring error;
    const auto allowance = githubcopilotquota::ResolveAllowance(config, error);

    Check(allowance.has_value(), "explicit allowance should resolve");
    CheckNear(allowance->total_credits, 2345.0, "explicit total credits should override plan allowance");
    Check(allowance->source == L"total_credits", "explicit allowance source should be total_credits");
    Check(error.empty(), "successful explicit allowance resolution should not set error");
}

void TestRejectsMissingAllowance()
{
    githubcopilotquota::PluginConfig config;

    std::wstring error;
    const auto allowance = githubcopilotquota::ResolveAllowance(config, error);

    Check(!allowance.has_value(), "missing allowance should fail");
    Check(error.find(L"plan") != std::wstring::npos, "missing allowance error should mention plan");
    Check(error.find(L"total_credits") != std::wstring::npos, "missing allowance error should mention total_credits");
}

void TestParsesUsageReport()
{
    std::wstring error;
    const auto usage = githubcopilotquota::ParseUsageJson(
        R"({
            "timePeriod": { "year": 2026, "month": 6 },
            "user": "octocat",
            "usageItems": [
                { "product": "Copilot AI Credits", "sku": "AI Credit", "unitType": "ai-credits", "netQuantity": 12.5 },
                { "product": "Copilot AI Credits", "sku": "AI Credit", "unitType": "ai-credits", "netQuantity": "7.25" },
                { "product": "Other", "sku": "Ignored", "unitType": "units", "netQuantity": 1000 }
            ]
        })",
        error);

    Check(usage.has_value(), "usage JSON should parse");
    Check(usage->user == L"octocat", "usage user should parse");
    CheckNear(usage->consumed_credits, 19.75, "only Copilot AI credit netQuantity values should be summed");
    Check(error.empty(), "successful usage parse should not set error");
}

void TestRejectsMalformedUsageNetQuantity()
{
    std::wstring error;
    const auto usage = githubcopilotquota::ParseUsageJson(
        R"({
            "user": "octocat",
            "usageItems": [
                { "product": "Copilot AI Credits", "sku": "AI Credit", "netQuantity": "7.25cr" }
            ]
        })",
        error);

    Check(!usage.has_value(), "malformed netQuantity should fail usage parsing");
    Check(!error.empty(), "malformed netQuantity should set error");
}

void TestRejectsNonFiniteUsageNetQuantity()
{
    std::wstring nan_error;
    const auto nan_usage = githubcopilotquota::ParseUsageJson(
        R"({
            "user": "octocat",
            "usageItems": [
                { "product": "Copilot AI Credits", "sku": "AI Credit", "netQuantity": "nan" }
            ]
        })",
        nan_error);

    Check(!nan_usage.has_value(), "nan netQuantity should fail usage parsing");
    Check(!nan_error.empty(), "nan netQuantity should set error");

    std::wstring inf_error;
    const auto inf_usage = githubcopilotquota::ParseUsageJson(
        R"({
            "user": "octocat",
            "usageItems": [
                { "product": "Copilot AI Credits", "sku": "AI Credit", "netQuantity": "inf" }
            ]
        })",
        inf_error);

    Check(!inf_usage.has_value(), "inf netQuantity should fail usage parsing");
    Check(!inf_error.empty(), "inf netQuantity should set error");
}

void TestRejectsUnquotedWhitespaceGarbageUsageNetQuantity()
{
    std::wstring error;
    const auto usage = githubcopilotquota::ParseUsageJson(
        R"({
            "user": "octocat",
            "usageItems": [
                { "product": "Copilot AI Credits", "sku": "AI Credit", "netQuantity": 7.25 cr }
            ]
        })",
        error);

    Check(!usage.has_value(), "unquoted netQuantity with whitespace garbage should fail usage parsing");
    Check(!error.empty(), "unquoted netQuantity with whitespace garbage should set error");
}

void TestParsesUserLogin()
{
    std::wstring error;
    const auto login = githubcopilotquota::ParseAuthenticatedUserJson(R"({"login":"octocat"})", error);

    Check(login.has_value(), "authenticated user JSON should parse");
    Check(*login == L"octocat", "login should parse");
}

void TestParsesCopilotInternalPremiumQuota()
{
    std::wstring error;
    const auto snapshot = githubcopilotquota::ParseCopilotInternalUserJson(FakeCopilotInternalResponse().body, error);

    Check(snapshot.has_value(), "Copilot internal response should parse");
    Check(snapshot->plan == L"pro", "Copilot internal plan should parse");
    Check(snapshot->quota_id == L"premium_interactions", "premium quota should be selected");
    CheckNear(snapshot->total_credits, 300.0, "premium entitlement should become total quota");
    CheckNear(snapshot->remaining_credits, 240.0, "premium remaining should parse");
    CheckNear(snapshot->remaining_percent, 80.0, "premium percent_remaining should parse");
    Check(snapshot->reset_at == 1782864000, "quota_reset_date should parse as UTC reset timestamp");
    Check(error.empty(), "successful Copilot internal parse should not set error");
}

void TestParsesCopilotInternalLimitedQuotaFallback()
{
    std::wstring error;
    const auto snapshot = githubcopilotquota::ParseCopilotInternalUserJson(
        R"({
            "copilot_plan": "free",
            "monthly_quotas": { "completions": 2000, "chat": 50 },
            "limited_user_quotas": { "completions": 1000, "chat": 10 }
        })",
        error);

    Check(snapshot.has_value(), "limited Copilot internal response should parse");
    Check(snapshot->quota_id == L"completions", "limited completions quota should be selected");
    CheckNear(snapshot->total_credits, 2000.0, "limited monthly quota should become total quota");
    CheckNear(snapshot->remaining_credits, 1000.0, "limited remaining quota should parse");
    CheckNear(snapshot->remaining_percent, 50.0, "limited remaining percent should calculate");
    Check(error.empty(), "successful limited Copilot internal parse should not set error");
}

void TestDefaultConfigPathUsesAppDataConfigFile()
{
    const auto path = githubcopilotquota::GetDefaultConfigPath();
    const std::wstring suffix = L"TrafficMonitorGitHubCopilotQuota\\config.json";

    Check(path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0,
        "default GitHub Copilot quota config path should end with TrafficMonitorGitHubCopilotQuota\\config.json");

    const DWORD appdata_length = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (appdata_length > 0)
    {
        std::wstring appdata(appdata_length, L'\0');
        const DWORD written = GetEnvironmentVariableW(L"APPDATA", appdata.data(), appdata_length);
        appdata.resize(written);
        Check(path.rfind(appdata, 0) == 0, "default GitHub Copilot quota config path should be under APPDATA");
    }
}

void TestFormatsCreditCounts()
{
    Check(githubcopilotquota::FormatCreditCount(0) == L"0cr", "zero credits should format");
    Check(githubcopilotquota::FormatCreditCount(950) == L"950cr", "under 1000 credits should use cr");
    Check(githubcopilotquota::FormatCreditCount(1200) == L"1.2kcr", "1000+ credits should use kcr");
    Check(githubcopilotquota::FormatCreditCount(20000) == L"20.0kcr", "large credits should keep one decimal");
}

void TestHandlesNonFiniteFormattingAndQuotaMath()
{
    const auto infinity = std::numeric_limits<double>::infinity();

    Check(githubcopilotquota::FormatCreditCount(infinity) == L"0cr", "infinite credits should format as zero credits");
    Check(githubcopilotquota::FormatCreditCount(-infinity) == L"0cr", "negative infinite credits should format as zero credits");
    Check(githubcopilotquota::FormatPercent(infinity) == L"0%", "infinite percent should format as zero percent");
    Check(githubcopilotquota::FormatPercent(-infinity) == L"0%", "negative infinite percent should format as zero percent");

    const auto quota_with_infinite_total = githubcopilotquota::CalculateQuota(infinity, 1.0);
    CheckNear(quota_with_infinite_total.total_credits, 0.0, "infinite total credits should clamp to zero");
    Check(std::isfinite(quota_with_infinite_total.remaining_credits), "infinite total should not produce non-finite remaining credits");
    Check(std::isfinite(quota_with_infinite_total.remaining_percent), "infinite total should not produce non-finite remaining percent");

    const auto quota_with_infinite_consumed = githubcopilotquota::CalculateQuota(1500.0, infinity);
    CheckNear(quota_with_infinite_consumed.consumed_credits, 0.0, "infinite consumed credits should clamp to zero");
    Check(std::isfinite(quota_with_infinite_consumed.remaining_credits), "infinite consumed should not produce non-finite remaining credits");
    Check(std::isfinite(quota_with_infinite_consumed.remaining_percent), "infinite consumed should not produce non-finite remaining percent");
}

void TestCalculatesRemainingQuota()
{
    const auto quota = githubcopilotquota::CalculateQuota(1500.0, 270.0);

    CheckNear(quota.total_credits, 1500.0, "quota should keep total");
    CheckNear(quota.consumed_credits, 270.0, "quota should keep consumed");
    CheckNear(quota.remaining_credits, 1230.0, "quota should subtract consumed from total");
    CheckNear(quota.remaining_percent, 82.0, "quota remaining percent should calculate");
}

void TestClampsOverage()
{
    const auto quota = githubcopilotquota::CalculateQuota(1500.0, 1700.0);

    CheckNear(quota.remaining_credits, 0.0, "overage remaining credits should clamp to zero");
    CheckNear(quota.remaining_percent, 0.0, "overage remaining percent should clamp to zero");
}

void TestFormatsQuotaValue()
{
    const long long now = 1782086400;
    const long long reset = 1783123200;
    const auto quota = githubcopilotquota::CalculateQuota(1500.0, 270.0);

    Check(githubcopilotquota::FormatQuotaValue(quota, reset, now) == L" 82% 1.2kcr 12d", "quota value should include leading space and reset");
    Check(githubcopilotquota::FormatQuotaValue(quota, 0, now) == L" 82% 1.2kcr", "quota value should omit missing reset");
}

void TestFormatsMonthlyResetCountdownInDays()
{
    const long long now = 1782086400;

    Check(githubcopilotquota::FormatResetCountdown(now + 7 * 24 * 60 * 60, now) == L"7d", "7-day monthly reset should stay day-based");
    Check(githubcopilotquota::FormatResetCountdown(now + 12 * 24 * 60 * 60, now) == L"12d", "12-day monthly reset should stay day-based");
    Check(githubcopilotquota::FormatResetCountdown(now + 31 * 24 * 60 * 60, now) == L"31d", "31-day monthly reset should stay day-based");
}

void TestCalculatesBillingPeriod()
{
    const long long now = 1782086400;
    const auto period = githubcopilotquota::CalculateBillingPeriod(15, now);

    Check(!period.is_calendar_month_estimate, "configured billing day should be exact");
    Check(period.start.year == 2026 && period.start.month == 6 && period.start.day == 15, "billing period start should be current cycle start");
    Check(period.end.year == 2026 && period.end.month == 7 && period.end.day == 15, "billing period end should be next cycle start");
    Check(period.reset_at == 1784073600, "reset timestamp should be next billing day UTC");
    Check(!period.usage_dates.empty(), "billing period should include usage dates");
    Check(period.usage_dates.front().day == 15, "first usage date should be start day");
}

void TestBillingPeriodUsageDatesStopAtToday()
{
    const long long now = 1782086400;
    const auto period = githubcopilotquota::CalculateBillingPeriod(15, now);

    Check(period.end.year == 2026 && period.end.month == 7 && period.end.day == 15, "billing period end should remain next reset");
    Check(period.reset_at == 1784073600, "billing reset timestamp should remain next billing day UTC");
    Check(!period.usage_dates.empty(), "billing usage dates should include elapsed cycle dates");
    const auto last = period.usage_dates.back();
    Check(last.year == 2026 && last.month == 6 && last.day == 22, "billing usage dates should stop at today");
    for (const auto& date : period.usage_dates)
    {
        Check(IsOnOrBefore(date, githubcopilotquota::Date{2026, 6, 22}), "billing usage dates should not include future dates");
    }
}

void TestCalculatesBillingPeriodBeforeBillingDay()
{
    const long long now = 1780704000;
    const auto period = githubcopilotquota::CalculateBillingPeriod(15, now);

    Check(!period.is_calendar_month_estimate, "configured billing day before current cycle should be exact");
    Check(period.start.year == 2026 && period.start.month == 5 && period.start.day == 15, "pre-billing-day period start should be previous cycle start");
    Check(period.end.year == 2026 && period.end.month == 6 && period.end.day == 15, "pre-billing-day period end should be current cycle end");
    Check(period.reset_at == 1781481600, "pre-billing-day reset timestamp should be current billing day UTC");
}

void TestCalculatesBillingPeriodWithShorterMonth()
{
    const long long now = 1776816000;
    const auto period = githubcopilotquota::CalculateBillingPeriod(31, now);

    Check(!period.is_calendar_month_estimate, "configured billing day 31 should be exact");
    Check(period.start.year == 2026 && period.start.month == 3 && period.start.day == 31, "short-month period start should use prior month billing day");
    Check(period.end.year == 2026 && period.end.month == 4 && period.end.day == 30, "short-month period end should clamp to last day");
    Check(period.reset_at == 1777507200, "short-month reset timestamp should be clamped billing day UTC");
}

void TestCalculatesCalendarMonthEstimate()
{
    const long long now = 1782086400;
    const auto period = githubcopilotquota::CalculateCalendarMonthEstimate(now);

    Check(period.is_calendar_month_estimate, "missing billing day should be estimate");
    Check(period.start.year == 2026 && period.start.month == 6 && period.start.day == 1, "calendar estimate should start on first day");
    Check(period.reset_at == 0, "calendar estimate should not claim a reset timestamp");
}

void TestBuildsUsagePaths()
{
    const githubcopilotquota::Date date{2026, 6, 22};

    Check(githubcopilotquota::BuildUsagePath(L"octocat", date) == L"/users/octocat/settings/billing/ai_credit/usage?year=2026&month=6&day=22", "daily usage path should include year month day");
    Check(githubcopilotquota::BuildMonthlyUsagePath(L"octocat", 2026, 6) == L"/users/octocat/settings/billing/ai_credit/usage?year=2026&month=6", "monthly usage path should include year month");
}

void TestFetchUsesConfigTokenAndClearsSnapshotToken()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token"})",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed with config token fallback");
    Check(fake.requests.size() == 1, "Copilot internal fetch should issue one request");
    Check(!fake.requests.empty() && fake.requests.front().path == L"/copilot_internal/user", "Copilot internal fetch should use the internal user endpoint");
    Check(!fake.requests.empty() && fake.requests.front().authorization == L"token config-token", "config token should be used for Copilot token Authorization header");
    Check(result.snapshot.config.github_token.empty(), "snapshot config should not retain GitHub token");
    CheckNear(result.snapshot.quota.total_credits, 300.0, "internal entitlement should become quota total");
    CheckNear(result.snapshot.quota.remaining_credits, 240.0, "internal remaining should become quota remaining");
    CheckNear(result.snapshot.quota.remaining_percent, 80.0, "internal percent_remaining should become quota remaining percent");
    Check(result.snapshot.period.reset_at == 1782864000, "internal quota_reset_date should become reset timestamp");
}

void TestFetchRequestHeadersUseCopilotInternalContract()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token"})",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed when checking header contract");
    Check(!fake.requests.empty(), "fetch helper should capture a request for header contract");
    Check(!fake.requests.empty() && fake.requests.front().accept == L"application/json", "Copilot internal request Accept header should match VS Code contract");
    Check(!fake.requests.empty() && fake.requests.front().api_version == L"2025-04-01", "Copilot internal request API version header should match contract");
    Check(!fake.requests.empty() && fake.requests.front().user_agent == L"GitHubCopilotChat/0.26.7", "Copilot internal request User-Agent header should match contract");
    Check(!fake.requests.empty() && fake.requests.front().editor_version == L"vscode/1.96.2", "Copilot internal request Editor-Version header should match contract");
    Check(!fake.requests.empty() && fake.requests.front().editor_plugin_version == L"copilot-chat/0.26.7", "Copilot internal request Editor-Plugin-Version header should match contract");
}

void TestFetchEnvTokenOverridesConfigToken()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token"})",
        L"env-token",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed with env token");
    Check(fake.requests.size() == 1, "Copilot internal mode should issue one request");
    Check(!fake.requests.empty() && fake.requests.front().authorization == L"token env-token", "env token should override config token");
}

void TestFetchEnvTokenWorksWithoutConfigAllowance()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        L"",
        L"env-token",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "env token should fetch without plan, total_credits, billing_day, or username config");
    Check(fake.requests.size() == 1, "config-free env token fetch should issue one Copilot internal request");
    CheckNear(result.snapshot.quota.total_credits, 300.0, "config-free fetch should use internal entitlement as total");
    CheckNear(result.snapshot.quota.remaining_percent, 80.0, "config-free fetch should use internal percent remaining");
}

void TestFetchRejectsEnvTokenWithCrLf()
{
    FakeGitHubTransport fake;

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token","username":"octocat","total_credits":100})",
        L"env-token\r\nX-Injected: value",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(!result.success, "env token with CR/LF should fail fetch helper");
    Check(result.error == L"GitHub token contains invalid characters.", "env token CR/LF error should be stable and non-secret");
    Check(result.error.find(L"env-token") == std::wstring::npos, "env token CR/LF error should not echo token");
    Check(fake.requests.empty(), "env token with CR/LF should be rejected before transport request");
}

void TestFetchRejectsConfigTokenWithCrLf()
{
    FakeGitHubTransport fake;

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        L"{\"github_token\":\"config-token\nX-Injected: value\",\"username\":\"octocat\",\"total_credits\":100}",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(!result.success, "config token with CR/LF should fail fetch helper");
    Check(result.error == L"GitHub token contains invalid characters.", "config token CR/LF error should be stable and non-secret");
    Check(result.error.find(L"config-token") == std::wstring::npos, "config token CR/LF error should not echo token");
    Check(fake.requests.empty(), "config token with CR/LF should be rejected before transport request");
}

void TestFetchMissingUsernameCallsUserEndpoint()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token"})",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should not require username for Copilot internal usage");
    Check(fake.requests.size() == 1, "missing username should not add user request");
    Check(!fake.requests.empty() && fake.requests[0].path != L"/user", "Copilot internal usage should not call /user");
}

void TestFetchConfiguredUsernameSkipsUserEndpoint()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token","username":"octocat"})",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed with configured username ignored by internal endpoint");
    Check(fake.requests.size() == 1, "configured username should only issue Copilot internal request");
    Check(!fake.requests.empty() && fake.requests.front().path != L"/user", "configured username should skip /user");
}

void TestFetchAuthErrorsUseStableMessage()
{
    for (const auto status : {401, 403})
    {
        FakeGitHubTransport fake;
        fake.responses.push_back(githubcopilotquota::GitHubHttpResponse{status, "{}"});

        const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
            LR"({"github_token":"config-token"})",
            L"",
            1782086400,
            FakeGitHubRequest,
            &fake);

        Check(!result.success, "auth HTTP status should fail fetch helper");
        Check(result.http_status == status, "auth HTTP status should be reported");
        Check(result.error == L"GitHub Copilot authentication failed.", "auth error should use stable message");
    }
}

void TestFetchNonSuccessHttpStatusIncludesStatus()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(githubcopilotquota::GitHubHttpResponse{500, "{}"});

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token"})",
        L"",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(!result.success, "non-2xx HTTP status should fail fetch helper");
    Check(result.http_status == 500, "non-2xx HTTP status should be reported");
    Check(result.error == L"GitHub Copilot API returned HTTP 500.", "non-2xx error should include HTTP status");
}

void TestLiveFetchWhenRequested()
{
    wchar_t flag[8]{};
    if (GetEnvironmentVariableW(L"GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST", flag, 8) == 0)
    {
        return;
    }

    const auto result = githubcopilotquota::FetchQuotaSnapshot();
    if (!result.success)
    {
        std::wcerr << L"LIVE ERROR: " << result.error << L" status=" << result.http_status << L'\n';
    }
    Check(result.success, "live GitHub Copilot quota fetch should succeed when requested");
    Check(result.snapshot.allowance.total_credits > 0.0, "live snapshot should include allowance");
    Check(result.snapshot.quota.total_credits > 0.0, "live snapshot should include quota total");
}
}

int main()
{
    TestParsesConfigWithExplicitAllowance();
    TestResolvesGitHubTokenPrecedence();
    TestParsesDeviceCodeResponse();
    TestParsesAccessTokenResponse();
    TestParsesAccessTokenPendingAndSlowDownErrors();
    TestCredentialTokenRoundTripWithTestTarget();
    TestRejectsMalformedConfigTotalCredits();
    TestRejectsNonFiniteConfigTotalCredits();
    TestRejectsUnquotedWhitespaceGarbageTotalCredits();
    TestRejectsFractionalBillingDay();
    TestRejectsUnquotedWhitespaceGarbageBillingDay();
    TestResolvesAllowanceFromPlan();
    TestExplicitAllowanceOverridesPlan();
    TestRejectsMissingAllowance();
    TestParsesUsageReport();
    TestRejectsMalformedUsageNetQuantity();
    TestRejectsNonFiniteUsageNetQuantity();
    TestRejectsUnquotedWhitespaceGarbageUsageNetQuantity();
    TestParsesUserLogin();
    TestParsesCopilotInternalPremiumQuota();
    TestParsesCopilotInternalLimitedQuotaFallback();
    TestDefaultConfigPathUsesAppDataConfigFile();
    TestFormatsCreditCounts();
    TestHandlesNonFiniteFormattingAndQuotaMath();
    TestCalculatesRemainingQuota();
    TestClampsOverage();
    TestFormatsQuotaValue();
    TestFormatsMonthlyResetCountdownInDays();
    TestCalculatesBillingPeriod();
    TestBillingPeriodUsageDatesStopAtToday();
    TestCalculatesBillingPeriodBeforeBillingDay();
    TestCalculatesBillingPeriodWithShorterMonth();
    TestCalculatesCalendarMonthEstimate();
    TestBuildsUsagePaths();
    TestFetchUsesConfigTokenAndClearsSnapshotToken();
    TestFetchRequestHeadersUseCopilotInternalContract();
    TestFetchEnvTokenOverridesConfigToken();
    TestFetchUsesStoredTokenBeforeConfigToken();
    TestFetchEnvTokenOverridesStoredToken();
    TestRunGitHubDeviceLoginStoresTokenAfterVerification();
    TestRunGitHubDeviceLoginBuildsCompleteUrlWhenGitHubOmitsIt();
    TestFetchEnvTokenWorksWithoutConfigAllowance();
    TestFetchRejectsEnvTokenWithCrLf();
    TestFetchRejectsConfigTokenWithCrLf();
    TestFetchMissingUsernameCallsUserEndpoint();
    TestFetchConfiguredUsernameSkipsUserEndpoint();
    TestFetchAuthErrorsUseStableMessage();
    TestFetchNonSuccessHttpStatusIncludesStatus();
    TestLiveFetchWhenRequested();

    if (failures != 0)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All GitHub Copilot quota tests passed\n";
    return 0;
}
