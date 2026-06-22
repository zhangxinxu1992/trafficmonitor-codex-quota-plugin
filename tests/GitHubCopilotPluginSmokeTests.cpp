#include "PluginInterface.h"

#include <Windows.h>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

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

bool StartsWith(const std::wstring& value, const wchar_t* prefix)
{
    const std::wstring prefix_value(prefix);
    return value.size() >= prefix_value.size()
        && value.compare(0, prefix_value.size(), prefix_value) == 0;
}

bool Contains(const std::wstring& value, const wchar_t* text)
{
    return value.find(text) != std::wstring::npos;
}

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

std::wstring CreateIsolatedAppDataDir()
{
    wchar_t temp_path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp_path);

    wchar_t temp_name[MAX_PATH]{};
    GetTempFileNameW(temp_path, L"gcq", 0, temp_name);
    DeleteFileW(temp_name);
    CreateDirectoryW(temp_name, nullptr);
    return temp_name;
}

void VerifyRefreshFailurePath(ITMPlugin* plugin, IPluginItem* item)
{
    const auto appdata = CreateIsolatedAppDataDir();
    EnvironmentVariableGuard appdata_guard(L"APPDATA", appdata.c_str());
    EnvironmentVariableGuard token_guard(L"COPILOT_QUOTA_GITHUB_TOKEN", nullptr);
    EnvironmentVariableGuard stored_guard(L"GITHUB_COPILOT_QUOTA_SKIP_STORED_CREDENTIAL", L"1");

    plugin->DataRequired();

    std::wstring value;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        value = item->GetItemValueText();
        if (value == L" ERR")
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    Check(value == L" ERR", "refresh failure should show ERR without a previous snapshot");

    const std::wstring tooltip(plugin->GetTooltipInfo());
    Check(Contains(tooltip, L"Last refresh status: failed"), "tooltip should report a failed refresh");
    Check(Contains(tooltip, L"No successful refresh yet."), "tooltip should report no successful refresh after first failure");
    Check(!Contains(tooltip, L"Waiting for first refresh."), "tooltip should not keep claiming it is waiting after first failure");
}
}

int main()
{
    const auto dll_path = CurrentExeDir() + L"\\TrafficMonitorGitHubCopilotQuota.dll";
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
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_NAME)) == L"GitHub Copilot Quota", "plugin name should match");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_DESCRIPTION)) == L"Displays remaining GitHub Copilot quota.", "plugin description should match");
        {
            EnvironmentVariableGuard options_guard(L"GITHUB_COPILOT_QUOTA_OPTIONS_SMOKE_TEST", L"1");
            Check(plugin->ShowOptionsDialog(nullptr) == ITMPlugin::OR_OPTION_UNCHANGED,
                "plugin options dialog should be provided");
        }

        IPluginItem* item = plugin->GetItem(0);
        Check(item != nullptr, "first item should exist");
        Check(plugin->GetItem(1) == nullptr, "second item should not exist");

        if (item != nullptr)
        {
            Check(std::wstring(item->GetItemId()) == L"GitHubCopilotQuotaAI", "item id should match");
            Check(std::wstring(item->GetItemName()) == L"GitHub Copilot Quota", "item name should match");
            Check(std::wstring(item->GetItemLableText()) == L"GC:", "label should avoid trim-prone whitespace");
            Check(std::wstring(item->GetItemValueSampleText()) == L" 100% 20.0kcr 31d", "sample should reserve value-leading spacing and full countdown width");

            const std::wstring initial_value(item->GetItemValueText());
            Check(initial_value == L" ...", "initial value should include visible spacing before loading");
            Check(StartsWith(initial_value, L" "), "initial value should start with visible spacing");

            VerifyRefreshFailurePath(plugin, item);
        }
    }

    FreeLibrary(module);

    if (failures != 0)
    {
        std::cerr << failures << " GitHub Copilot plugin smoke test(s) failed\n";
        return 1;
    }

    std::cout << "GitHub Copilot plugin smoke tests passed\n";
    return 0;
}
