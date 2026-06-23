#include "PluginInterface.h"
#include "../src/PluginVersion.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstring>
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

std::wstring JoinPath(std::wstring base, const wchar_t* child)
{
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
    {
        base.push_back(L'\\');
    }
    base += child;
    return base;
}

void WriteAsciiFile(const std::wstring& path, const char* content)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "test config file should be writable");
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DWORD written = 0;
    const auto size = static_cast<DWORD>(std::strlen(content));
    const BOOL ok = WriteFile(file, content, size, &written, nullptr);
    CloseHandle(file);
    Check(ok && written == size, "test config file should be fully written");
}

void PrepareDisplayConfig(const std::wstring& appdata)
{
    const auto dir = JoinPath(appdata, L"TrafficMonitorGitHubCopilotQuota");
    CreateDirectoryW(dir.c_str(), nullptr);
    WriteAsciiFile(
        JoinPath(dir, L"config.json"),
        "{\n"
        "  \"quota_display\": \"used\",\n"
        "  \"reset_display\": \"time\",\n"
        "  \"show_reset_info\": false,\n"
        "  \"show_remaining_credits\": false\n"
        "}\n");
}

struct FindChildByTextContext
{
    const wchar_t* text{};
    HWND window{};
};

BOOL CALLBACK FindChildByText(HWND window, LPARAM parameter)
{
    auto* context = reinterpret_cast<FindChildByTextContext*>(parameter);
    wchar_t text[256]{};
    GetWindowTextW(window, text, static_cast<int>(_countof(text)));
    if (std::wstring(text) == context->text)
    {
        context->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindChildWindowByText(HWND parent, const wchar_t* text)
{
    FindChildByTextContext context{text, nullptr};
    EnumChildWindows(parent, FindChildByText, reinterpret_cast<LPARAM>(&context));
    return context.window;
}

struct FindOwnWindowContext
{
    const wchar_t* class_name{};
    const wchar_t* title{};
    DWORD process_id{};
    HWND window{};
};

BOOL CALLBACK FindOwnWindow(HWND window, LPARAM parameter)
{
    auto* context = reinterpret_cast<FindOwnWindowContext*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != context->process_id)
    {
        return TRUE;
    }

    wchar_t class_name[128]{};
    wchar_t title[256]{};
    GetClassNameW(window, class_name, static_cast<int>(_countof(class_name)));
    GetWindowTextW(window, title, static_cast<int>(_countof(title)));
    if (std::wstring(class_name) == context->class_name && std::wstring(title) == context->title)
    {
        context->window = window;
        return FALSE;
    }

    return TRUE;
}

HWND FindOwnWindowByClassAndTitle(const wchar_t* class_name, const wchar_t* title)
{
    FindOwnWindowContext context{class_name, title, GetCurrentProcessId(), nullptr};
    EnumWindows(FindOwnWindow, reinterpret_cast<LPARAM>(&context));
    return context.window;
}

int SystemDpi()
{
    HDC dc = GetDC(nullptr);
    if (dc == nullptr)
    {
        return 96;
    }

    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
}

int ScaleForDpi(int value, int dpi)
{
    return MulDiv(value, dpi, 96);
}

void VerifyOptionsDialogUsesCompactLayout(ITMPlugin* plugin)
{
    std::atomic<bool> finished{false};
    std::atomic<DWORD> dialog_thread_id{0};
    std::thread dialog_thread([&] {
        dialog_thread_id = GetCurrentThreadId();
        plugin->ShowOptionsDialog(nullptr);
        finished = true;
    });

    HWND dialog = nullptr;
    for (int attempt = 0; attempt < 50 && !finished; ++attempt)
    {
        dialog = FindOwnWindowByClassAndTitle(
            L"TrafficMonitorGitHubCopilotQuotaOptions",
            L"TrafficMonitor GitHub Copilot Quota");
        if (dialog != nullptr)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    Check(dialog != nullptr, "options dialog window should open for layout smoke test");
    if (dialog != nullptr)
    {
        const int dpi = SystemDpi();
        RECT rect{};
        GetWindowRect(dialog, &rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;

        Check(width >= ScaleForDpi(520, dpi), "options dialog should scale up from the 96-DPI layout width");
        Check(width <= ScaleForDpi(660, dpi), "options dialog should not be double-DPI-scaled wider than the compact layout");
        Check(height >= ScaleForDpi(285, dpi), "options dialog should scale up from the 96-DPI layout height");
        Check(height <= ScaleForDpi(380, dpi), "options dialog should not be double-DPI-scaled taller than the compact layout");

        const auto title = FindChildWindowByText(dialog, L"GitHub Copilot authentication");
        Check(title != nullptr, "options dialog title control should exist");
        if (title != nullptr)
        {
            const auto font = reinterpret_cast<HFONT>(SendMessageW(title, WM_GETFONT, 0, 0));
            LOGFONTW log_font{};
            Check(font != nullptr && GetObjectW(font, sizeof(log_font), &log_font) == sizeof(log_font),
                "options dialog title font should be inspectable");
            const int title_height = -log_font.lfHeight;
            Check(title_height >= MulDiv(12, dpi, 72),
                "options dialog title font should scale with the current DPI");
            Check(title_height <= MulDiv(15, dpi, 72),
                "options dialog title font should not be oversized");
        }

        const auto show_reset_info = FindChildWindowByText(dialog, L"Show reset info");
        const auto countdown = FindChildWindowByText(dialog, L"Countdown");
        const auto reset_time = FindChildWindowByText(dialog, L"Reset time");
        Check(show_reset_info != nullptr, "GitHub Copilot options should include reset info checkbox");
        Check(show_reset_info != nullptr && SendMessageW(show_reset_info, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
            "GitHub Copilot reset info checkbox should reflect hidden reset config");
        Check(countdown != nullptr && !IsWindowEnabled(countdown),
            "GitHub Copilot countdown option should be disabled when reset info is hidden");
        Check(reset_time != nullptr && !IsWindowEnabled(reset_time),
            "GitHub Copilot reset time option should be disabled when reset info is hidden");

        PostMessageW(dialog, WM_CLOSE, 0, 0);
    }
    else if (dialog_thread_id != 0)
    {
        PostThreadMessageW(dialog_thread_id, WM_QUIT, 0, 0);
    }

    for (int attempt = 0; attempt < 50 && !finished; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (dialog_thread.joinable())
    {
        if (finished)
        {
            dialog_thread.join();
        }
        else
        {
            Check(false, "options dialog layout smoke test should finish");
            dialog_thread.detach();
        }
    }
}
}

int main()
{
    SetProcessDPIAware();

    const auto appdata = CreateIsolatedAppDataDir();
    PrepareDisplayConfig(appdata);
    EnvironmentVariableGuard appdata_guard(L"APPDATA", appdata.c_str());

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
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_NAME)) == L"TrafficMonitor GitHub Copilot Quota", "plugin name should match");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_DESCRIPTION)) == L"Displays remaining GitHub Copilot quota in TrafficMonitor.", "plugin description should match");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_AUTHOR)) == L"zhangxinxu", "plugin author should identify the repository owner");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_COPYRIGHT)) == L"MIT", "plugin copyright field should name the project license");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_VERSION)) == kTrafficMonitorQuotaPluginVersion, "plugin version should match the unified release version");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_URL)) == L"https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins", "plugin URL should point to the public repository");
        VerifyOptionsDialogUsesCompactLayout(plugin);

        IPluginItem* item = plugin->GetItem(0);
        Check(item != nullptr, "first item should exist");
        Check(plugin->GetItem(1) == nullptr, "second item should not exist");

        if (item != nullptr)
        {
            Check(std::wstring(item->GetItemId()) == L"GitHubCopilotQuotaAI", "item id should match");
            Check(std::wstring(item->GetItemName()) == L"TrafficMonitor GitHub Copilot Quota", "item name should match");
            Check(std::wstring(item->GetItemLableText()) == L"GC:", "label should avoid trim-prone whitespace");
            Check(std::wstring(item->GetItemValueSampleText()) == L" 100%", "sample should omit hidden reset info and hidden credit width");

            const std::wstring initial_value(item->GetItemValueText());
            Check(initial_value == L" ...", "initial value should include visible spacing before loading");
            Check(StartsWith(initial_value, L" "), "initial value should start with visible spacing");
            Check(item->IsDrawResourceUsageGraph() == 1, "GitHub Copilot item should opt into TrafficMonitor resource graph drawing");
            Check(item->GetResourceUsageGraphValue() == 0.0f, "GitHub Copilot graph value should be empty before first refresh");
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
