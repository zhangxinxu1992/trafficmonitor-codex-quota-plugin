#include "PluginInterface.h"
#include "GitHubCopilotQuotaCore.h"
#include "GitHubCopilotQuotaFetch.h"

#include <Windows.h>
#include <commctrl.h>

#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

namespace
{
constexpr int kSignInButton = 1001;
constexpr int kSignOutButton = 1002;

class GitHubCopilotQuotaPlugin;

bool IsEnvFlagSet(const wchar_t* name)
{
    wchar_t value[8]{};
    return GetEnvironmentVariableW(name, value, 8) != 0;
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

std::wstring AuthStatusText()
{
    if (!GetEnvVar(L"COPILOT_QUOTA_GITHUB_TOKEN").empty())
    {
        return L"Status: environment token is configured.";
    }

    std::wstring error;
    const auto stored = githubcopilotquota::ReadCredentialToken(
        githubcopilotquota::GetGitHubOAuthCredentialTarget(),
        error);
    if (stored.has_value())
    {
        return L"Status: signed in with GitHub.";
    }

    return L"Status: not signed in.";
}

int ShowOptionsTaskDialog(HWND parent, const std::wstring& content)
{
    using TaskDialogIndirectFn = HRESULT(WINAPI*)(
        const TASKDIALOGCONFIG*,
        int*,
        int*,
        BOOL*);

    const HMODULE comctl = LoadLibraryW(L"comctl32.dll");
    const auto task_dialog_indirect = comctl == nullptr
        ? nullptr
        : reinterpret_cast<TaskDialogIndirectFn>(GetProcAddress(comctl, "TaskDialogIndirect"));
    if (task_dialog_indirect == nullptr)
    {
        const auto fallback = MessageBoxW(
            parent,
            (content + L"\n\nYes: Sign in with GitHub\nNo: Sign out\nCancel: Close").c_str(),
            L"GitHub Copilot Quota",
            MB_YESNOCANCEL | MB_ICONINFORMATION);
        if (fallback == IDYES)
        {
            return kSignInButton;
        }
        if (fallback == IDNO)
        {
            return kSignOutButton;
        }
        return IDCLOSE;
    }

    const TASKDIALOG_BUTTON buttons[] = {
        {kSignInButton, L"Sign in with GitHub"},
        {kSignOutButton, L"Sign out"},
    };

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = parent;
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    config.pszWindowTitle = L"GitHub Copilot Quota";
    config.pszMainInstruction = L"GitHub Copilot authentication";
    config.pszContent = content.c_str();
    config.cButtons = static_cast<UINT>(sizeof(buttons) / sizeof(buttons[0]));
    config.pButtons = buttons;
    config.nDefaultButton = kSignInButton;

    int button = IDCLOSE;
    if (FAILED(task_dialog_indirect(&config, &button, nullptr, nullptr)))
    {
        MessageBoxW(parent, content.c_str(), L"GitHub Copilot Quota", MB_OK | MB_ICONINFORMATION);
        return IDCLOSE;
    }
    return button;
}

void ShowInfo(HWND parent, const std::wstring& message)
{
    MessageBoxW(parent, message.c_str(), L"GitHub Copilot Quota", MB_OK | MB_ICONINFORMATION);
}

void ShowError(HWND parent, const std::wstring& message)
{
    MessageBoxW(parent, message.c_str(), L"GitHub Copilot Quota", MB_OK | MB_ICONERROR);
}

bool CopyTextToClipboard(HWND parent, const std::wstring& text)
{
    const auto byte_count = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byte_count);
    if (memory == nullptr)
    {
        return false;
    }

    auto* locked = static_cast<wchar_t*>(GlobalLock(memory));
    if (locked == nullptr)
    {
        GlobalFree(memory);
        return false;
    }
    CopyMemory(locked, text.c_str(), byte_count);
    GlobalUnlock(memory);

    if (!OpenClipboard(parent))
    {
        GlobalFree(memory);
        return false;
    }

    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
    {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }

    CloseClipboard();
    return true;
}

bool ShowGitHubDeviceCode(const githubcopilotquota::DeviceCodeResponse& device, std::wstring& error, void* context)
{
    auto* parent = static_cast<HWND>(context);
    if (device.user_code.empty())
    {
        error = L"GitHub did not return a device code.";
        return false;
    }

    const bool copied = CopyTextToClipboard(parent, device.user_code);
    std::wstring message = L"GitHub device code:\n\n";
    message += device.user_code;
    message += copied
        ? L"\n\nThe code has been copied to the clipboard. Click OK, then paste it if GitHub asks for a code."
        : L"\n\nClick OK, then enter this code if GitHub asks for it.";
    MessageBoxW(parent, message.c_str(), L"GitHub Copilot Quota", MB_OK | MB_ICONINFORMATION);
    return true;
}

class GitHubCopilotQuotaItem final : public IPluginItem
{
public:
    const wchar_t* GetItemName() const override
    {
        return L"GitHub Copilot Quota";
    }

    const wchar_t* GetItemId() const override
    {
        return L"GitHubCopilotQuotaAI";
    }

    const wchar_t* GetItemLableText() const override
    {
        return L"GC:";
    }

    const wchar_t* GetItemValueText() const override;

    const wchar_t* GetItemValueSampleText() const override
    {
        return L" 100% 20.0kcr 31d";
    }

private:
    mutable std::wstring m_value;
};

class GitHubCopilotQuotaPlugin final : public ITMPlugin
{
public:
    static GitHubCopilotQuotaPlugin& Instance()
    {
        static GitHubCopilotQuotaPlugin instance;
        return instance;
    }

    IPluginItem* GetItem(int index) override
    {
        return index == 0 ? &m_item : nullptr;
    }

    void DataRequired() override
    {
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_refreshing || now < m_next_refresh)
            {
                return;
            }
            m_refreshing = true;
        }

        if (m_worker.joinable())
        {
            m_worker.join();
        }

        m_worker = std::thread([] {
            const auto result = githubcopilotquota::FetchQuotaSnapshot();
            GitHubCopilotQuotaPlugin::Instance().ApplyFetchResult(result);
        });
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override
    {
        switch (index)
        {
        case TMI_NAME:
            return L"GitHub Copilot Quota";
        case TMI_DESCRIPTION:
            return L"Displays remaining GitHub Copilot quota.";
        case TMI_AUTHOR:
            return L"OpenAI Codex";
        case TMI_COPYRIGHT:
            return L"MIT";
        case TMI_VERSION:
            return L"1.0.0";
        case TMI_URL:
            return L"";
        default:
            return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tooltip_return = BuildTooltipLocked();
        return m_tooltip_return.c_str();
    }

    OptionReturn ShowOptionsDialog(void* hParent) override
    {
        if (IsEnvFlagSet(L"GITHUB_COPILOT_QUOTA_OPTIONS_SMOKE_TEST"))
        {
            return OR_OPTION_UNCHANGED;
        }

        auto* parent = static_cast<HWND>(hParent);
        const auto button = ShowOptionsTaskDialog(
            parent,
            AuthStatusText()
                + L"\n\nUse GitHub sign-in to store a protected local OAuth token. "
                  L"The environment variable override still takes precedence.");

        if (button == kSignInButton)
        {
            const auto result = githubcopilotquota::RunGitHubDeviceLogin(ShowGitHubDeviceCode, parent);
            if (!result.success)
            {
                ShowError(parent, result.error.empty() ? L"GitHub sign-in failed." : result.error);
                return OR_OPTION_UNCHANGED;
            }
            ResetAuthState();
            ShowInfo(parent, result.username.empty() ? L"Signed in with GitHub." : L"Signed in as " + result.username + L".");
            return OR_OPTION_CHANGED;
        }

        if (button == kSignOutButton)
        {
            std::wstring error;
            if (!githubcopilotquota::DeleteCredentialToken(githubcopilotquota::GetGitHubOAuthCredentialTarget(), error))
            {
                ShowError(parent, error.empty() ? L"GitHub sign-out failed." : error);
                return OR_OPTION_UNCHANGED;
            }
            ResetAuthState();
            ShowInfo(parent, L"Signed out from the stored GitHub token.");
            return OR_OPTION_CHANGED;
        }

        return OR_OPTION_UNCHANGED;
    }

    std::wstring ValueText() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_has_snapshot)
        {
            return githubcopilotquota::FormatQuotaValue(m_snapshot.quota, m_snapshot.period.reset_at, std::time(nullptr));
        }

        if (m_last_error.empty())
        {
            return L" ...";
        }
        return L" ERR";
    }

private:
    ~GitHubCopilotQuotaPlugin()
    {
        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

    void ApplyFetchResult(const githubcopilotquota::FetchResult& result)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_refreshing = false;
        m_last_refresh = std::time(nullptr);

        if (result.success)
        {
            m_snapshot = result.snapshot;
            m_has_snapshot = true;
            m_last_error.clear();
            m_next_refresh = std::chrono::steady_clock::now() + std::chrono::minutes(15);
        }
        else
        {
            m_last_error = result.error.empty() ? L"Unknown GitHub Copilot quota error." : result.error;
            m_next_refresh = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        }
    }

    void ResetAuthState()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_has_snapshot = false;
        m_last_error.clear();
        m_next_refresh = {};
    }

    std::wstring LastRefreshStatusLocked() const
    {
        if (m_refreshing)
        {
            return L"refreshing";
        }
        if (m_last_refresh == 0)
        {
            return L"not refreshed";
        }
        return m_last_error.empty() ? L"OK" : L"failed";
    }

    std::wstring ResetLineLocked() const
    {
        if (m_snapshot.period.reset_at <= 0)
        {
            return L"Reset: not configured";
        }

        return L"Reset: " + githubcopilotquota::FormatResetCountdown(m_snapshot.period.reset_at, std::time(nullptr));
    }

    std::wstring PeriodModeLineLocked() const
    {
        if (m_snapshot.period.is_copilot_internal)
        {
            return L"Period mode: GitHub Copilot internal quota";
        }

        if (m_snapshot.period.is_calendar_month_estimate)
        {
            return L"Period mode: calendar month estimate";
        }

        if (m_snapshot.config.has_billing_day)
        {
            return L"Period mode: billing day " + std::to_wstring(m_snapshot.config.billing_day);
        }

        return L"Period mode: configured billing period";
    }

    std::wstring BuildTooltipLocked() const
    {
        std::wstring tooltip = L"GitHub Copilot quota";
        if (!m_snapshot.config.plan.empty())
        {
            tooltip += L"\nPlan: ";
            tooltip += m_snapshot.config.plan;
        }

        if (m_has_snapshot)
        {
            tooltip += L"\nUsername: ";
            tooltip += m_snapshot.username.empty() ? L"unknown" : m_snapshot.username;
            tooltip += L"\nAllowance: ";
            tooltip += m_snapshot.allowance.source.empty() ? L"configured" : m_snapshot.allowance.source;
            tooltip += L", total ";
            tooltip += githubcopilotquota::FormatCreditCount(m_snapshot.allowance.total_credits);
            tooltip += L"\nConsumed: ";
            tooltip += githubcopilotquota::FormatCreditCount(m_snapshot.quota.consumed_credits);
            tooltip += L"\nRemaining: ";
            tooltip += githubcopilotquota::FormatCreditCount(m_snapshot.quota.remaining_credits);
            tooltip += L" (";
            tooltip += githubcopilotquota::FormatPercent(m_snapshot.quota.remaining_percent);
            tooltip += L")";
            tooltip += L"\n";
            tooltip += ResetLineLocked();
            tooltip += L"\n";
            tooltip += PeriodModeLineLocked();
        }
        else if (m_refreshing)
        {
            tooltip += L"\nRefreshing...";
        }
        else
        {
            tooltip += m_last_refresh == 0 ? L"\nWaiting for first refresh." : L"\nNo successful refresh yet.";
        }

        tooltip += L"\nLast refresh status: ";
        tooltip += LastRefreshStatusLocked();
        if (!m_last_error.empty())
        {
            tooltip += L"\nLast error: ";
            tooltip += m_last_error;
        }
        return tooltip;
    }

    GitHubCopilotQuotaItem m_item;

    mutable std::mutex m_mutex;
    githubcopilotquota::QuotaSnapshot m_snapshot;
    bool m_has_snapshot{};
    bool m_refreshing{};
    std::time_t m_last_refresh{};
    std::chrono::steady_clock::time_point m_next_refresh{};
    std::thread m_worker;
    std::wstring m_last_error;
    std::wstring m_tooltip_return;
};

const wchar_t* GitHubCopilotQuotaItem::GetItemValueText() const
{
    m_value = GitHubCopilotQuotaPlugin::Instance().ValueText();
    return m_value.c_str();
}
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &GitHubCopilotQuotaPlugin::Instance();
}
