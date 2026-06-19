#include "PluginInterface.h"

#include <Windows.h>
#include <iostream>
#include <string>

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

std::wstring CurrentExeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value(path);
    const auto slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

bool Contains(const std::wstring& value, const wchar_t* text)
{
    return value.find(text) != std::wstring::npos;
}

bool StartsWith(const std::wstring& value, const wchar_t* prefix)
{
    const std::wstring prefix_value(prefix);
    return value.size() >= prefix_value.size()
        && value.compare(0, prefix_value.size(), prefix_value) == 0;
}

bool RunLiveRefresh()
{
    wchar_t flag[8]{};
    return GetEnvironmentVariableW(L"CODEX_QUOTA_RUN_LIVE_TEST", flag, 8) != 0;
}
}

int main()
{
    const auto dll_path = CurrentExeDir() + L"\\TrafficMonitorCodexQuota.dll";
    HMODULE module = LoadLibraryW(dll_path.c_str());
    if (module == nullptr)
    {
        std::wcerr << L"FAIL: LoadLibrary failed for " << dll_path << L" error " << GetLastError() << L'\n';
        return 1;
    }

    auto get_instance = reinterpret_cast<ITMPlugin* (*)()>(GetProcAddress(module, "TMPluginGetInstance"));
    Check(get_instance != nullptr, "TMPluginGetInstance should be exported");
    ITMPlugin* plugin = get_instance == nullptr ? nullptr : get_instance();
    Check(plugin != nullptr, "TMPluginGetInstance should return a plugin");

    if (plugin != nullptr)
    {
        Check(plugin->GetAPIVersion() >= 7, "plugin API version should support OnInitialize");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_NAME)) == L"Codex Quota", "plugin name should match");

        IPluginItem* five_hour = plugin->GetItem(0);
        IPluginItem* weekly = plugin->GetItem(1);
        Check(five_hour != nullptr, "first item should exist");
        Check(weekly != nullptr, "second item should exist");
        Check(plugin->GetItem(2) == nullptr, "third item should not exist");

        if (five_hour != nullptr)
        {
            Check(std::wstring(five_hour->GetItemId()) == L"CodexQuota5h", "5h item id should match");
            Check(std::wstring(five_hour->GetItemName()) == L"Codex 5h", "5h item name should match");
            Check(std::wstring(five_hour->GetItemLableText()) == L"5h:", "5h label should avoid trim-prone whitespace");
            Check(std::wstring(five_hour->GetItemValueSampleText()) == L" 100% 4h 59m", "5h sample should reserve value-leading spacing and full countdown width");
            Check(std::wstring(five_hour->GetItemValueText()) == L" ...", "5h initial value should include visible spacing before loading");
        }
        if (weekly != nullptr)
        {
            Check(std::wstring(weekly->GetItemId()) == L"CodexQuotaWeek", "weekly item id should match");
            Check(std::wstring(weekly->GetItemName()) == L"Codex Week", "weekly item name should match");
            Check(std::wstring(weekly->GetItemLableText()) == L"7d:", "weekly label should avoid trim-prone whitespace");
            Check(std::wstring(weekly->GetItemValueSampleText()) == L" 100% 6d 23h", "weekly sample should reserve value-leading spacing and full countdown width");
            Check(std::wstring(weekly->GetItemValueText()) == L" ...", "weekly initial value should include visible spacing before loading");
        }

        if (RunLiveRefresh() && five_hour != nullptr && weekly != nullptr)
        {
            plugin->DataRequired();
            std::wstring five_hour_value;
            std::wstring weekly_value;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                Sleep(100);
                five_hour_value = five_hour->GetItemValueText();
                weekly_value = weekly->GetItemValueText();
                if (Contains(five_hour_value, L"%") && Contains(weekly_value, L"%"))
                {
                    break;
                }
            }

            Check(Contains(five_hour_value, L"%"), "live 5h plugin value should contain a percent");
            Check(StartsWith(five_hour_value, L" "), "live 5h plugin value should include visible spacing before the percent");
            Check(Contains(five_hour_value, L"m") || Contains(five_hour_value, L"h") || Contains(five_hour_value, L"now"), "live 5h plugin value should contain a reset countdown");
            Check(Contains(weekly_value, L"%"), "live weekly plugin value should contain a percent");
            Check(StartsWith(weekly_value, L" "), "live weekly plugin value should include visible spacing before the percent");
            Check(Contains(weekly_value, L"h") || Contains(weekly_value, L"d") || Contains(weekly_value, L"w") || Contains(weekly_value, L"now"), "live weekly plugin value should contain a reset countdown");
        }
    }

    FreeLibrary(module);

    if (failures != 0)
    {
        std::cerr << failures << " smoke test(s) failed\n";
        return 1;
    }

    std::cout << "Plugin smoke tests passed\n";
    return 0;
}
