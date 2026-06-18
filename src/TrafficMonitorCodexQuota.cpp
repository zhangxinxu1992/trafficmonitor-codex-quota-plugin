#include "PluginInterface.h"
#include "CodexQuotaCore.h"
#include "CodexQuotaFetch.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

namespace
{
enum class WindowKind
{
    FiveHour,
    Weekly
};

class CodexQuotaPlugin;

class CodexQuotaItem final : public IPluginItem
{
public:
    explicit CodexQuotaItem(WindowKind kind) : m_kind(kind) {}

    const wchar_t* GetItemName() const override
    {
        return m_kind == WindowKind::FiveHour ? L"Codex 5h" : L"Codex Week";
    }

    const wchar_t* GetItemId() const override
    {
        return m_kind == WindowKind::FiveHour ? L"CodexQuota5h" : L"CodexQuotaWeek";
    }

    const wchar_t* GetItemLableText() const override
    {
        return m_kind == WindowKind::FiveHour ? L"Codex 5h" : L"Codex W";
    }

    const wchar_t* GetItemValueText() const override;

    const wchar_t* GetItemValueSampleText() const override
    {
        return L"100%";
    }

private:
    WindowKind m_kind;
    mutable std::wstring m_value;
};

class CodexQuotaPlugin final : public ITMPlugin
{
public:
    static CodexQuotaPlugin& Instance()
    {
        static CodexQuotaPlugin instance;
        return instance;
    }

    IPluginItem* GetItem(int index) override
    {
        switch (index)
        {
        case 0:
            return &m_five_hour_item;
        case 1:
            return &m_weekly_item;
        default:
            return nullptr;
        }
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
            m_tooltip = BuildTooltipLocked();
        }

        if (m_worker.joinable())
        {
            m_worker.join();
        }

        m_worker = std::thread([] {
            const auto result = codexquota::FetchUsageSnapshot();
            CodexQuotaPlugin::Instance().ApplyFetchResult(result);
        });
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override
    {
        switch (index)
        {
        case TMI_NAME:
            return L"Codex Quota";
        case TMI_DESCRIPTION:
            return L"Displays Codex 5-hour and weekly quota usage.";
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

    std::wstring ValueText(WindowKind kind) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto* window = SelectWindow(kind);
        if (m_has_usage && window != nullptr && window->present)
        {
            return codexquota::FormatPercent(window->used_percent);
        }

        if (m_last_error.empty())
        {
            return L"...";
        }
        return L"ERR";
    }

private:
    CodexQuotaPlugin()
        : m_five_hour_item(WindowKind::FiveHour),
          m_weekly_item(WindowKind::Weekly)
    {
        m_tooltip = BuildTooltipLocked();
    }

    ~CodexQuotaPlugin()
    {
        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

    const codexquota::RateWindow* SelectWindow(WindowKind kind) const
    {
        if (!m_has_usage)
        {
            return nullptr;
        }
        return kind == WindowKind::FiveHour ? &m_usage.primary : &m_usage.secondary;
    }

    void ApplyFetchResult(const codexquota::FetchResult& result)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_refreshing = false;
        m_last_refresh = std::time(nullptr);

        if (result.success)
        {
            m_usage = result.usage;
            m_has_usage = true;
            m_last_error.clear();
            m_next_refresh = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        }
        else
        {
            m_last_error = result.error.empty() ? L"Unknown Codex usage error." : result.error;
            m_next_refresh = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        }

        m_tooltip = BuildTooltipLocked();
    }

    std::wstring WindowLineLocked(const wchar_t* title, const codexquota::RateWindow& window) const
    {
        if (!window.present)
        {
            return std::wstring(title) + L": unavailable";
        }

        std::wstring line = std::wstring(title) + L": " + codexquota::FormatPercent(window.used_percent) + L" used";
        if (window.reset_at > 0)
        {
            line += L", resets in ";
            line += codexquota::FormatResetCountdown(window.reset_at, std::time(nullptr));
        }
        return line;
    }

    std::wstring BuildTooltipLocked() const
    {
        std::wstring tooltip = L"Codex quota";
        if (!m_usage.plan_type.empty())
        {
            tooltip += L" (";
            tooltip += m_usage.plan_type;
            tooltip += L")";
        }

        tooltip += L"\n";
        if (m_has_usage)
        {
            tooltip += WindowLineLocked(L"5h", m_usage.primary);
            tooltip += L"\n";
            tooltip += WindowLineLocked(L"Week", m_usage.secondary);
        }
        else if (m_refreshing)
        {
            tooltip += L"Refreshing...";
        }
        else
        {
            tooltip += L"Waiting for first refresh.";
        }

        if (m_last_refresh > 0)
        {
            tooltip += L"\nLast refresh: ";
            tooltip += m_last_error.empty() ? L"OK" : L"failed";
        }
        if (!m_last_error.empty())
        {
            tooltip += L"\nError: ";
            tooltip += m_last_error;
        }
        return tooltip;
    }

    CodexQuotaItem m_five_hour_item;
    CodexQuotaItem m_weekly_item;

    mutable std::mutex m_mutex;
    codexquota::UsageSnapshot m_usage;
    bool m_has_usage{};
    bool m_refreshing{};
    std::time_t m_last_refresh{};
    std::chrono::steady_clock::time_point m_next_refresh{};
    std::thread m_worker;
    std::wstring m_last_error;
    std::wstring m_tooltip;
    std::wstring m_tooltip_return;
};

const wchar_t* CodexQuotaItem::GetItemValueText() const
{
    m_value = CodexQuotaPlugin::Instance().ValueText(m_kind);
    return m_value.c_str();
}
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &CodexQuotaPlugin::Instance();
}
