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

bool EndsWith(const std::wstring& value, const wchar_t* suffix)
{
    const std::wstring suffix_value(suffix);
    return value.size() >= suffix_value.size()
        && value.compare(value.size() - suffix_value.size(), suffix_value.size(), suffix_value) == 0;
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
            Check(std::wstring(five_hour->GetItemLableText()) == L"Codex 5h", "5h label should match");
            Check(std::wstring(five_hour->GetItemValueText()) == L"...", "5h initial value should be loading");
        }
        if (weekly != nullptr)
        {
            Check(std::wstring(weekly->GetItemId()) == L"CodexQuotaWeek", "weekly item id should match");
            Check(std::wstring(weekly->GetItemName()) == L"Codex Week", "weekly item name should match");
            Check(std::wstring(weekly->GetItemLableText()) == L"Codex W", "weekly label should match");
            Check(std::wstring(weekly->GetItemValueText()) == L"...", "weekly initial value should be loading");
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
                if (EndsWith(five_hour_value, L"%") && EndsWith(weekly_value, L"%"))
                {
                    break;
                }
            }

            Check(EndsWith(five_hour_value, L"%"), "live 5h plugin value should become a percent");
            Check(EndsWith(weekly_value, L"%"), "live weekly plugin value should become a percent");
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
