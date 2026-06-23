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

std::wstring ReadEnvironmentVariable(const wchar_t* name)
{
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0)
    {
        return {};
    }

    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    value.resize(written);
    return value;
}

class EnvironmentVariableGuard
{
public:
    EnvironmentVariableGuard(const wchar_t* name, const wchar_t* value)
        : m_name(name),
          m_had_value(GetEnvironmentVariableW(name, nullptr, 0) != 0),
          m_original(ReadEnvironmentVariable(name))
    {
        SetEnvironmentVariableW(m_name.c_str(), value);
    }

    ~EnvironmentVariableGuard()
    {
        SetEnvironmentVariableW(m_name.c_str(), m_had_value ? m_original.c_str() : nullptr);
    }

private:
    std::wstring m_name;
    bool m_had_value{};
    std::wstring m_original;
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

    std::wstring error;
    const auto stored_choice = githubcopilotquota::ResolveGitHubToken(L" stored-token ", config, error);
    Check(stored_choice.has_value(), "stored token choice should resolve");
    Check(stored_choice->token == L"stored-token", "stored token should be trimmed");
    Check(stored_choice->source == githubcopilotquota::GitHubTokenSource::StoredCredential, "stored token source should be reported");

    error.clear();
    const auto missing_choice = githubcopilotquota::ResolveGitHubToken(L" ", config, error);
    Check(!missing_choice.has_value(), "missing token choice should fail");
    Check(error.find(L"TrafficMonitor plugin options") != std::wstring::npos,
        "missing token error should direct users to plugin options sign-in");
    Check(error.find(L"github_token") == std::wstring::npos,
        "missing token error should not mention legacy plaintext config tokens");
    Check(error.find(L"environment") == std::wstring::npos && error.find(L"override") == std::wstring::npos,
        "missing token error should only mention supported token sources");
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

    const auto username = githubcopilotquota::ReadCredentialUsername(target, error);
    Check(username.has_value(), "credential username read should succeed");
    Check(username.value_or(L"") == L"octocat", "credential username read should return original username");

    error.clear();
    Check(githubcopilotquota::DeleteCredentialToken(target, error), "credential token delete should succeed");
    Check(error.empty(), "successful credential token delete should not set error");

    const auto deleted_token = githubcopilotquota::ReadCredentialToken(target, error);
    Check(!deleted_token.has_value(), "deleted credential token should not be found");
}

void TestFetchUsesStoredTokenWhenLegacyConfigTokenIsPresent()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        LR"({"github_token":"config-token"})",
        L"stored-token",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed with stored token");
    Check(fake.requests.size() == 1, "stored-token fetch should issue one request");
    Check(!fake.requests.empty() && fake.requests.front().authorization == L"token stored-token",
        "stored token should be used when legacy config token is present");
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

void TestParsesCurrentConfigFields()
{
    std::wstring error;
    const auto config = githubcopilotquota::ParseConfigJson(
        LR"({
            "username": "octocat",
            "quota_display": "used",
            "reset_display": "time",
            "show_reset_info": false,
            "show_remaining_credits": false
        })",
        error);

    Check(config.has_value(), "current config fields should parse");
    Check(config->username == L"octocat", "username should parse");
    Check(config->display.quota_display == githubcopilotquota::QuotaDisplayMode::Used, "quota display should parse");
    Check(config->display.reset_display == githubcopilotquota::ResetDisplayMode::Time, "reset display should parse");
    Check(!config->display.show_reset_info, "reset info display flag should parse");
    Check(!config->display.show_remaining_credits, "remaining credits display flag should parse");
    Check(error.empty(), "successful config parse should not set error");

    error.clear();
    const auto default_config = githubcopilotquota::ParseConfigJson(L"{}", error);
    Check(default_config.has_value(), "empty GitHub Copilot display config should parse");
    Check(default_config->display.show_reset_info, "GitHub Copilot reset info should default to visible");
    Check(githubcopilotquota::SerializeConfigJson(*config).find(L"\"show_reset_info\": false") != std::wstring::npos,
        "GitHub Copilot config serialization should persist hidden reset info");
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

long long LocalTimestamp(int year, int month, int day, int hour, int minute)
{
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&local));
}

void TestFormatsQuotaValueWithDisplayOptions()
{
    const auto quota = githubcopilotquota::CalculateQuota(1500.0, 270.0);
    const auto now = LocalTimestamp(2026, 6, 22, 10, 0);
    const auto reset = LocalTimestamp(2026, 6, 22, 18, 30);

    githubcopilotquota::DisplayOptions options;
    options.quota_display = githubcopilotquota::QuotaDisplayMode::Used;
    options.reset_display = githubcopilotquota::ResetDisplayMode::Time;
    options.show_remaining_credits = false;

    Check(githubcopilotquota::FormatQuotaValue(quota, reset, now, options) == L" 18% 18:30",
        "used GitHub value should omit credits when configured and show reset time");

    options.show_remaining_credits = true;
    Check(githubcopilotquota::FormatQuotaValue(quota, reset, now, options) == L" 18% 1.2kcr 18:30",
        "GitHub remaining credits should remain remaining credits even with used percent");

    options.show_reset_info = false;
    Check(githubcopilotquota::FormatQuotaValue(quota, reset, now, options) == L" 18% 1.2kcr",
        "hidden GitHub Copilot reset info should keep percent and credits only");

    options.show_remaining_credits = false;
    Check(githubcopilotquota::FormatQuotaValue(quota, reset, now, options) == L" 18%",
        "hidden GitHub Copilot reset info and credits should leave only the quota percent");
}

void TestFormatsResourceGraphValueWithDisplayOptions()
{
    const auto quota = githubcopilotquota::CalculateQuota(1500.0, 270.0);

    githubcopilotquota::DisplayOptions options;
    CheckNear(githubcopilotquota::FormatResourceGraphValue(quota, options), 0.18,
        "remaining display should graph used GitHub Copilot quota");

    const auto almost_empty_quota = githubcopilotquota::CalculateQuotaFromRemaining(100.0, 4.0, 4.0);
    CheckNear(githubcopilotquota::FormatResourceGraphValue(almost_empty_quota, options), 0.96,
        "4 percent remaining GitHub Copilot quota should graph 96 percent used");

    options.quota_display = githubcopilotquota::QuotaDisplayMode::Used;
    CheckNear(githubcopilotquota::FormatResourceGraphValue(quota, options), 0.18,
        "used display should graph used GitHub Copilot quota");

    const auto over_remaining = githubcopilotquota::CalculateQuotaFromRemaining(100.0, 80.0, 125.0);
    CheckNear(githubcopilotquota::FormatResourceGraphValue(over_remaining, options), 0.0,
        "negative used GitHub Copilot graph values should clamp to zero");
}

void TestFormatsMonthlyResetCountdownInDays()
{
    const long long now = 1782086400;

    Check(githubcopilotquota::FormatResetCountdown(now + 7 * 24 * 60 * 60, now) == L"7d", "7-day monthly reset should stay day-based");
    Check(githubcopilotquota::FormatResetCountdown(now + 12 * 24 * 60 * 60, now) == L"12d", "12-day monthly reset should stay day-based");
    Check(githubcopilotquota::FormatResetCountdown(now + 31 * 24 * 60 * 60, now) == L"31d", "31-day monthly reset should stay day-based");
}

void TestFetchRejectsLegacyConfigToken()
{
    FakeGitHubTransport fake;

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJson(
        LR"({"github_token":"config-token"})",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(!result.success, "fetch helper should reject legacy config token fallback");
    Check(fake.requests.empty(), "legacy config token should not authorize a request");
    Check(result.error.find(L"github_token") == std::wstring::npos, "missing-token error should not mention legacy github_token");
}

void TestFetchUsesStoredTokenAndBuildsSnapshot()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        LR"({"username":"octocat"})",
        L"stored-token",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "fetch helper should succeed with stored token");
    Check(fake.requests.size() == 1, "Copilot internal fetch should issue one request");
    Check(!fake.requests.empty() && fake.requests.front().path == L"/copilot_internal/user", "Copilot internal fetch should use the internal user endpoint");
    Check(!fake.requests.empty() && fake.requests.front().authorization == L"token stored-token", "stored token should be used for Copilot token Authorization header");
    Check(result.snapshot.plan == L"pro", "internal plan should be stored on the snapshot");
    CheckNear(result.snapshot.quota.total_credits, 300.0, "internal entitlement should become quota total");
    CheckNear(result.snapshot.quota.remaining_credits, 240.0, "internal remaining should become quota remaining");
    CheckNear(result.snapshot.quota.remaining_percent, 80.0, "internal percent_remaining should become quota remaining percent");
    Check(result.snapshot.period.reset_at == 1782864000, "internal quota_reset_date should become reset timestamp");
}

void TestFetchRequestHeadersUseCopilotInternalContract()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        L"",
        L"stored-token",
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

void TestFetchStoredTokenWorksWithoutConfigFile()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        L"",
        L"stored-token",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(result.success, "stored credential token should fetch without a config file");
    Check(fake.requests.size() == 1, "config-free stored credential fetch should issue one Copilot internal request");
    CheckNear(result.snapshot.quota.total_credits, 300.0, "config-free fetch should use internal entitlement as total");
    CheckNear(result.snapshot.quota.remaining_percent, 80.0, "config-free fetch should use internal percent remaining");
}

void TestFetchRejectsStoredTokenWithCrLf()
{
    FakeGitHubTransport fake;

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        LR"({"username":"octocat"})",
        L"stored-token\r\nX-Injected: value",
        1782086400,
        FakeGitHubRequest,
        &fake);

    Check(!result.success, "stored token with CR/LF should fail fetch helper");
    Check(result.error == L"GitHub token contains invalid characters.", "stored token CR/LF error should be stable and non-secret");
    Check(result.error.find(L"stored-token") == std::wstring::npos, "stored token CR/LF error should not echo token");
    Check(fake.requests.empty(), "stored token with CR/LF should be rejected before transport request");
}

void TestFetchMissingUsernameCallsUserEndpoint()
{
    FakeGitHubTransport fake;
    fake.responses.push_back(FakeCopilotInternalResponse());

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        L"",
        L"stored-token",
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

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        LR"({"username":"octocat"})",
        L"stored-token",
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

        const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJsonWithStoredToken(
            L"",
            L"stored-token",
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

    const auto result = githubcopilotquota::FetchQuotaSnapshotFromConfigJsonWithStoredToken(
        L"",
        L"stored-token",
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
    if (GetEnvironmentVariableW(L"TRAFFICMONITOR_GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST", flag, 8) == 0
        && GetEnvironmentVariableW(L"GITHUB_COPILOT_QUOTA_RUN_LIVE_TEST", flag, 8) == 0)
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
    TestParsesCurrentConfigFields();
    TestResolvesGitHubTokenPrecedence();
    TestParsesDeviceCodeResponse();
    TestParsesAccessTokenResponse();
    TestParsesAccessTokenPendingAndSlowDownErrors();
    TestCredentialTokenRoundTripWithTestTarget();
    TestParsesUserLogin();
    TestParsesCopilotInternalPremiumQuota();
    TestParsesCopilotInternalLimitedQuotaFallback();
    TestDefaultConfigPathUsesAppDataConfigFile();
    TestFormatsCreditCounts();
    TestHandlesNonFiniteFormattingAndQuotaMath();
    TestCalculatesRemainingQuota();
    TestClampsOverage();
    TestFormatsQuotaValue();
    TestFormatsQuotaValueWithDisplayOptions();
    TestFormatsResourceGraphValueWithDisplayOptions();
    TestFormatsMonthlyResetCountdownInDays();
    TestFetchRejectsLegacyConfigToken();
    TestFetchUsesStoredTokenAndBuildsSnapshot();
    TestFetchRequestHeadersUseCopilotInternalContract();
    TestFetchUsesStoredTokenWhenLegacyConfigTokenIsPresent();
    TestRunGitHubDeviceLoginStoresTokenAfterVerification();
    TestRunGitHubDeviceLoginBuildsCompleteUrlWhenGitHubOmitsIt();
    TestFetchStoredTokenWorksWithoutConfigFile();
    TestFetchRejectsStoredTokenWithCrLf();
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
