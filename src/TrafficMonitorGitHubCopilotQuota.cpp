#include "PluginInterface.h"
#include "GitHubCopilotQuotaCore.h"
#include "GitHubCopilotQuotaFetch.h"

#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

namespace
{
class GitHubCopilotQuotaPlugin;

class GitHubCopilotQuotaItem final : public IPluginItem
{
public:
    const wchar_t* GetItemName() const override
    {
        return L"GitHub Copilot AI Credits";
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
            return L"Displays remaining GitHub Copilot monthly AI Credits.";
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
