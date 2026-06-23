#include "PluginInterface.h"
#include "CodexQuotaCore.h"
#include "CodexQuotaFetch.h"
#include "PluginVersion.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace
{
enum class WindowKind
{
    FiveHour,
    Weekly
};

constexpr int kQuotaRemainingRadio = 2001;
constexpr int kQuotaUsedRadio = 2002;
constexpr int kResetCountdownRadio = 2003;
constexpr int kResetTimeRadio = 2004;
constexpr int kSaveButton = 2005;
constexpr int kShowResetInfoCheckbox = 2006;
constexpr const wchar_t* kOptionsDialogClassName = L"TrafficMonitorCodexQuotaOptions";

class CodexQuotaPlugin;

std::wstring WindowsErrorMessage(DWORD error_code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    if (length == 0 || buffer == nullptr)
    {
        return L"Windows error " + std::to_wstring(error_code);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L'.'))
    {
        message.pop_back();
    }
    return message;
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
    value.resize(written);
    return value;
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

std::wstring GetDefaultConfigPath()
{
    const auto appdata = GetEnvVar(L"APPDATA");
    if (!appdata.empty())
    {
        return JoinPath(JoinPath(appdata, L"TrafficMonitorCodexQuota"), L"config.json");
    }

    return L"TrafficMonitorCodexQuota\\config.json";
}

bool ReadFileUtf8AsWide(const std::wstring& path, std::wstring& content, bool allow_missing, std::wstring& error)
{
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD code = GetLastError();
        if (allow_missing && (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND))
        {
            content.clear();
            return true;
        }
        error = L"Failed to open TrafficMonitor Codex quota config " + path + L": " + WindowsErrorMessage(code);
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
    {
        CloseHandle(file);
        error = L"Invalid TrafficMonitor Codex quota config size.";
        return false;
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD bytes_read = 0;
    const BOOL ok = bytes.empty()
        || ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &bytes_read, nullptr);
    CloseHandle(file);
    if (!ok)
    {
        error = L"Failed to read TrafficMonitor Codex quota config " + path + L": " + WindowsErrorMessage(GetLastError());
        return false;
    }
    bytes.resize(bytes_read);
    if (bytes.empty())
    {
        content.clear();
        return true;
    }

    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wide_length <= 0)
    {
        error = L"Failed to decode TrafficMonitor Codex quota config as UTF-8.";
        return false;
    }

    content.assign(static_cast<size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), content.data(), wide_length);
    return true;
}

bool WriteFileWideAsUtf8(const std::wstring& path, const std::wstring& content, std::wstring& error)
{
    const auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        const auto dir = path.substr(0, slash);
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            error = L"Failed to create TrafficMonitor Codex quota config directory: " + WindowsErrorMessage(GetLastError());
            return false;
        }
    }

    const int byte_length = WideCharToMultiByte(CP_UTF8, 0, content.data(), static_cast<int>(content.size()), nullptr, 0, nullptr, nullptr);
    if (byte_length < 0)
    {
        error = L"Failed to encode TrafficMonitor Codex quota config as UTF-8.";
        return false;
    }
    std::string bytes(static_cast<size_t>(byte_length), '\0');
    if (byte_length > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, content.data(), static_cast<int>(content.size()), bytes.data(), byte_length, nullptr, nullptr);
    }

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = L"Failed to open TrafficMonitor Codex quota config for writing: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    DWORD written = 0;
    const BOOL ok = bytes.empty() || WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    if (!ok || written != bytes.size())
    {
        error = L"Failed to write TrafficMonitor Codex quota config: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    return true;
}

std::optional<codexquota::PluginConfig> LoadCodexConfig(std::wstring& error)
{
    std::wstring json;
    if (!ReadFileUtf8AsWide(GetDefaultConfigPath(), json, true, error))
    {
        return std::nullopt;
    }
    return codexquota::ParseConfigJson(json, error);
}

bool SaveCodexConfig(const codexquota::PluginConfig& config, std::wstring& error)
{
    return WriteFileWideAsUtf8(GetDefaultConfigPath(), codexquota::SerializeConfigJson(config), error);
}

bool SameDisplayOptions(const codexquota::DisplayOptions& lhs, const codexquota::DisplayOptions& rhs)
{
    return lhs.quota_display == rhs.quota_display
        && lhs.reset_display == rhs.reset_display
        && lhs.show_reset_info == rhs.show_reset_info;
}

int FallbackSystemDpi()
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

int DialogDpi(HWND parent)
{
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr && parent != nullptr && IsWindow(parent))
    {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        auto* get_dpi_for_window = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
        if (get_dpi_for_window != nullptr)
        {
            const UINT dpi = get_dpi_for_window(parent);
            if (dpi > 0)
            {
                return static_cast<int>(dpi);
            }
        }
    }
    return FallbackSystemDpi();
}

int ScaleForDpi(int value, int dpi)
{
    return MulDiv(value, dpi, 96);
}

HFONT CreateDialogFont(int point_size, int weight, int dpi)
{
    return CreateFontW(
        -MulDiv(point_size, dpi, 72),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

void AdjustWindowRectForDpi(RECT* rect, DWORD style, DWORD ex_style, int dpi)
{
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr)
    {
        using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        auto* adjust_for_dpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        if (adjust_for_dpi != nullptr)
        {
            adjust_for_dpi(rect, style, FALSE, ex_style, static_cast<UINT>(dpi));
            return;
        }
    }
    AdjustWindowRectEx(rect, style, FALSE, ex_style);
}

void CenterWindow(HWND window, HWND parent)
{
    RECT window_rect{};
    GetWindowRect(window, &window_rect);

    RECT area{};
    if (parent != nullptr && IsWindow(parent))
    {
        GetWindowRect(parent, &area);
    }
    else
    {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    }

    const int window_width = window_rect.right - window_rect.left;
    const int window_height = window_rect.bottom - window_rect.top;
    const int area_width = area.right - area.left;
    const int area_height = area.bottom - area.top;
    SetWindowPos(
        window,
        nullptr,
        area.left + (area_width - window_width) / 2,
        area.top + (area_height - window_height) / 2,
        0,
        0,
        SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

struct CodexOptionsDialogState
{
    codexquota::PluginConfig original_config;
    codexquota::PluginConfig config;
    bool changed{};
    int dpi{96};
    HFONT title_font{};
    HFONT body_font{};
    HBRUSH content_brush{};
    HBRUSH footer_brush{};
    HWND title_hwnd{};
};

void SetButtonFont(HWND parent, int id, HFONT font)
{
    SendMessageW(GetDlgItem(parent, id), WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void SetResetChoiceEnabled(HWND window, bool enabled)
{
    EnableWindow(GetDlgItem(window, kResetCountdownRadio), enabled);
    EnableWindow(GetDlgItem(window, kResetTimeRadio), enabled);
}

void CaptureDisplayOptions(HWND window, CodexOptionsDialogState& state)
{
    state.config.display.quota_display = IsDlgButtonChecked(window, kQuotaUsedRadio) == BST_CHECKED
        ? codexquota::QuotaDisplayMode::Used
        : codexquota::QuotaDisplayMode::Remaining;
    state.config.display.show_reset_info = IsDlgButtonChecked(window, kShowResetInfoCheckbox) == BST_CHECKED;
    state.config.display.reset_display = IsDlgButtonChecked(window, kResetTimeRadio) == BST_CHECKED
        ? codexquota::ResetDisplayMode::Time
        : codexquota::ResetDisplayMode::Countdown;
    state.changed = !SameDisplayOptions(state.config.display, state.original_config.display);
}

LRESULT CALLBACK CodexOptionsDialogProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    auto* state = reinterpret_cast<CodexOptionsDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message)
    {
    case WM_CREATE:
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<CodexOptionsDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        RECT client{};
        GetClientRect(window, &client);
        const int margin = ScaleForDpi(16, state->dpi);
        const int footer_height = ScaleForDpi(52, state->dpi);
        const int content_width = client.right - client.left - margin * 2;
        int y = ScaleForDpi(15, state->dpi);

        state->title_hwnd = CreateWindowExW(
            0,
            L"STATIC",
            L"Codex quota display",
            WS_CHILD | WS_VISIBLE,
            margin,
            y,
            content_width,
            ScaleForDpi(26, state->dpi),
            window,
            nullptr,
            nullptr,
            nullptr);
        SendMessageW(state->title_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(state->title_font), TRUE);

        y += ScaleForDpi(42, state->dpi);
        CreateWindowExW(0, L"STATIC", L"Quota:", WS_CHILD | WS_VISIBLE, margin, y, ScaleForDpi(100, state->dpi), ScaleForDpi(22, state->dpi), window, nullptr, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Remaining", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, margin + ScaleForDpi(110, state->dpi), y, ScaleForDpi(120, state->dpi), ScaleForDpi(24, state->dpi), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kQuotaRemainingRadio)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Used", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, margin + ScaleForDpi(240, state->dpi), y, ScaleForDpi(90, state->dpi), ScaleForDpi(24, state->dpi), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kQuotaUsedRadio)), nullptr, nullptr);

        y += ScaleForDpi(36, state->dpi);
        CreateWindowExW(0, L"STATIC", L"Reset:", WS_CHILD | WS_VISIBLE, margin, y, ScaleForDpi(100, state->dpi), ScaleForDpi(22, state->dpi), window, nullptr, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Show reset info", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, margin + ScaleForDpi(110, state->dpi), y, ScaleForDpi(170, state->dpi), ScaleForDpi(24, state->dpi), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShowResetInfoCheckbox)), nullptr, nullptr);

        y += ScaleForDpi(32, state->dpi);
        CreateWindowExW(0, L"BUTTON", L"Countdown", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, margin + ScaleForDpi(110, state->dpi), y, ScaleForDpi(120, state->dpi), ScaleForDpi(24, state->dpi), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResetCountdownRadio)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Reset time", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, margin + ScaleForDpi(240, state->dpi), y, ScaleForDpi(120, state->dpi), ScaleForDpi(24, state->dpi), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResetTimeRadio)), nullptr, nullptr);

        for (const auto id : {kQuotaRemainingRadio, kQuotaUsedRadio, kShowResetInfoCheckbox, kResetCountdownRadio, kResetTimeRadio})
        {
            SetButtonFont(window, id, state->body_font);
        }
        EnumChildWindows(window, [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(state->body_font));
        SendMessageW(state->title_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(state->title_font), TRUE);

        CheckRadioButton(
            window,
            kQuotaRemainingRadio,
            kQuotaUsedRadio,
            state->config.display.quota_display == codexquota::QuotaDisplayMode::Used ? kQuotaUsedRadio : kQuotaRemainingRadio);
        CheckRadioButton(
            window,
            kResetCountdownRadio,
            kResetTimeRadio,
            state->config.display.reset_display == codexquota::ResetDisplayMode::Time ? kResetTimeRadio : kResetCountdownRadio);
        SendMessageW(
            GetDlgItem(window, kShowResetInfoCheckbox),
            BM_SETCHECK,
            state->config.display.show_reset_info ? BST_CHECKED : BST_UNCHECKED,
            0);
        SetResetChoiceEnabled(window, state->config.display.show_reset_info);

        const int button_y = client.bottom - footer_height + ScaleForDpi(12, state->dpi);
        const int button_height = ScaleForDpi(28, state->dpi);
        const int close_width = ScaleForDpi(88, state->dpi);
        const int save_width = ScaleForDpi(100, state->dpi);
        const int gap = ScaleForDpi(10, state->dpi);
        int button_x = client.right - margin - close_width;

        auto* cancel_button = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, button_x, button_y, close_width, button_height, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), nullptr, nullptr);
        SendMessageW(cancel_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);
        button_x -= gap + save_width;
        auto* save_button = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, button_x, button_y, save_width, button_height, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveButton)), nullptr, nullptr);
        SendMessageW(save_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);
        SetFocus(save_button);
        return 0;
    }
    case WM_ERASEBKGND:
    {
        if (state == nullptr)
        {
            break;
        }
        auto* dc = reinterpret_cast<HDC>(w_param);
        RECT client{};
        GetClientRect(window, &client);
        RECT footer = client;
        footer.top = footer.bottom - ScaleForDpi(52, state->dpi);
        FillRect(dc, &client, state->content_brush);
        FillRect(dc, &footer, state->footer_brush);
        HPEN separator = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
        auto* old_pen = SelectObject(dc, separator);
        MoveToEx(dc, client.left, footer.top, nullptr);
        LineTo(dc, client.right, footer.top);
        SelectObject(dc, old_pen);
        DeleteObject(separator);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    {
        if (state == nullptr)
        {
            break;
        }
        auto* dc = reinterpret_cast<HDC>(w_param);
        const auto control = reinterpret_cast<HWND>(l_param);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, control == state->title_hwnd ? RGB(0, 51, 153) : RGB(0, 0, 0));
        return reinterpret_cast<LRESULT>(state->content_brush);
    }
    case WM_COMMAND:
    {
        if (state == nullptr)
        {
            break;
        }
        const auto command = LOWORD(w_param);
        if (command == kSaveButton)
        {
            CaptureDisplayOptions(window, *state);
            DestroyWindow(window);
            return 0;
        }
        if (command == kShowResetInfoCheckbox)
        {
            SetResetChoiceEnabled(window, IsDlgButtonChecked(window, kShowResetInfoCheckbox) == BST_CHECKED);
            return 0;
        }
        if (command == IDCANCEL || command == IDCLOSE)
        {
            DestroyWindow(window);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

bool ShowCodexOptionsDialog(HWND parent, codexquota::PluginConfig& config)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = CodexOptionsDialogProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kOptionsDialogClassName;
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    CodexOptionsDialogState state;
    state.original_config = config;
    state.config = config;
    state.dpi = DialogDpi(parent);
    state.title_font = CreateDialogFont(13, FW_NORMAL, state.dpi);
    state.body_font = CreateDialogFont(9, FW_NORMAL, state.dpi);
    state.content_brush = CreateSolidBrush(RGB(255, 255, 255));
    state.footer_brush = CreateSolidBrush(GetSysColor(COLOR_3DFACE));

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD ex_style = WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    RECT window_rect{0, 0, ScaleForDpi(440, state.dpi), ScaleForDpi(244, state.dpi)};
    AdjustWindowRectForDpi(&window_rect, style, ex_style, state.dpi);

    HWND window = CreateWindowExW(
        ex_style,
        kOptionsDialogClassName,
        L"TrafficMonitor Codex Quota",
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        parent,
        nullptr,
        instance,
        &state);
    if (window != nullptr)
    {
        CenterWindow(window, parent);
        const BOOL disable_parent = parent != nullptr && IsWindow(parent) && IsWindowEnabled(parent);
        if (disable_parent)
        {
            EnableWindow(parent, FALSE);
        }
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);

        MSG message{};
        while (IsWindow(window))
        {
            const BOOL got_message = GetMessageW(&message, nullptr, 0, 0);
            if (got_message <= 0)
            {
                break;
            }
            if (!IsDialogMessageW(window, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (disable_parent)
        {
            EnableWindow(parent, TRUE);
            SetActiveWindow(parent);
        }
    }

    DeleteObject(state.title_font);
    DeleteObject(state.body_font);
    DeleteObject(state.content_brush);
    DeleteObject(state.footer_brush);

    if (state.changed)
    {
        config = state.config;
    }
    return state.changed;
}

std::wstring BuildCodexSampleText(WindowKind kind, const codexquota::DisplayOptions& options)
{
    std::wstring sample = L" 100%";
    if (!options.show_reset_info)
    {
        return sample;
    }
    if (options.reset_display == codexquota::ResetDisplayMode::Time)
    {
        sample += L" 12-31 23:59";
        return sample;
    }

    sample += kind == WindowKind::FiveHour ? L" 4h 59m" : L" 6d 23h";
    return sample;
}

class CodexQuotaItem final : public IPluginItem
{
public:
    explicit CodexQuotaItem(WindowKind kind) : m_kind(kind) {}

    const wchar_t* GetItemName() const override
    {
        return m_kind == WindowKind::FiveHour ? L"TrafficMonitor Codex 5h" : L"TrafficMonitor Codex Week";
    }

    const wchar_t* GetItemId() const override
    {
        return m_kind == WindowKind::FiveHour ? L"CodexQuota5h" : L"CodexQuotaWeek";
    }

    const wchar_t* GetItemLableText() const override
    {
        return m_kind == WindowKind::FiveHour ? L"CX 5h:" : L"CX 7d:";
    }

    const wchar_t* GetItemValueText() const override;

    const wchar_t* GetItemValueSampleText() const override;

    int IsDrawResourceUsageGraph() const override
    {
        return 1;
    }

    float GetResourceUsageGraphValue() const override;

private:
    WindowKind m_kind;
    mutable std::wstring m_value;
    mutable std::wstring m_sample;
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
            return L"TrafficMonitor Codex Quota";
        case TMI_DESCRIPTION:
            return L"Displays remaining Codex 5-hour and weekly quota percentage in TrafficMonitor.";
        case TMI_AUTHOR:
            return L"zhangxinxu";
        case TMI_COPYRIGHT:
            return L"MIT";
        case TMI_VERSION:
            return kTrafficMonitorQuotaPluginVersion;
        case TMI_URL:
            return L"https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins";
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
        std::wstring error;
        auto config = LoadCodexConfig(error);
        if (!config.has_value())
        {
            MessageBoxW(static_cast<HWND>(hParent), error.c_str(), L"TrafficMonitor Codex Quota", MB_OK | MB_ICONERROR);
            return OR_OPTION_UNCHANGED;
        }

        if (!ShowCodexOptionsDialog(static_cast<HWND>(hParent), *config))
        {
            return OR_OPTION_UNCHANGED;
        }

        if (!SaveCodexConfig(*config, error))
        {
            MessageBoxW(static_cast<HWND>(hParent), error.c_str(), L"TrafficMonitor Codex Quota", MB_OK | MB_ICONERROR);
            return OR_OPTION_UNCHANGED;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_config = *config;
            m_tooltip = BuildTooltipLocked();
        }
        return OR_OPTION_CHANGED;
    }

    std::wstring ValueText(WindowKind kind) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto* window = SelectWindow(kind);
        if (m_has_usage && window != nullptr && window->present)
        {
            return L" " + codexquota::FormatWindowText(window->used_percent, window->reset_at, std::time(nullptr), m_config.display);
        }

        if (m_last_error.empty())
        {
            return L" ...";
        }
        return L" ERR";
    }

    std::wstring SampleText(WindowKind kind) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return BuildCodexSampleText(kind, m_config.display);
    }

    float ResourceGraphValue(WindowKind kind) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto* window = SelectWindow(kind);
        if (m_has_usage && window != nullptr && window->present)
        {
            return codexquota::FormatResourceGraphValue(window->used_percent, m_config.display);
        }
        return 0.0f;
    }

private:
    CodexQuotaPlugin()
        : m_five_hour_item(WindowKind::FiveHour),
          m_weekly_item(WindowKind::Weekly)
    {
        std::wstring error;
        if (const auto config = LoadCodexConfig(error))
        {
            m_config = *config;
        }
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

        std::wstring line = std::wstring(title) + L": "
            + codexquota::FormatWindowText(window.used_percent, 0, std::time(nullptr), m_config.display);
        line += m_config.display.quota_display == codexquota::QuotaDisplayMode::Used ? L" used" : L" remaining";
        if (window.reset_at > 0)
        {
            line += m_config.display.reset_display == codexquota::ResetDisplayMode::Time ? L", resets at " : L", resets in ";
            line += m_config.display.reset_display == codexquota::ResetDisplayMode::Time
                ? codexquota::FormatResetTime(window.reset_at, std::time(nullptr))
                : codexquota::FormatResetCountdown(window.reset_at, std::time(nullptr));
        }
        return line;
    }

    std::wstring BuildTooltipLocked() const
    {
        std::wstring tooltip = L"TrafficMonitor Codex quota";
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
    codexquota::PluginConfig m_config;
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

const wchar_t* CodexQuotaItem::GetItemValueSampleText() const
{
    m_sample = CodexQuotaPlugin::Instance().SampleText(m_kind);
    return m_sample.c_str();
}

float CodexQuotaItem::GetResourceUsageGraphValue() const
{
    return CodexQuotaPlugin::Instance().ResourceGraphValue(m_kind);
}
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &CodexQuotaPlugin::Instance();
}
