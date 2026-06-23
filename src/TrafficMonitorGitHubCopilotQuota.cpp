#include "PluginInterface.h"
#include "GitHubCopilotQuotaCore.h"
#include "GitHubCopilotQuotaFetch.h"
#include "PluginVersion.h"

#include <Windows.h>
#include <commctrl.h>

#include <chrono>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace
{
constexpr int kSignInButton = 1001;
constexpr int kSignOutButton = 1002;
constexpr int kSaveButton = 1003;
constexpr int kQuotaRemainingRadio = 1004;
constexpr int kQuotaUsedRadio = 1005;
constexpr int kResetCountdownRadio = 1006;
constexpr int kResetTimeRadio = 1007;
constexpr int kShowCreditsCheckbox = 1008;
constexpr int kShowResetInfoCheckbox = 1009;
constexpr const wchar_t* kOptionsDialogClassName = L"TrafficMonitorGitHubCopilotQuotaOptions";

class GitHubCopilotQuotaPlugin;

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
        error = L"Failed to open TrafficMonitor GitHub Copilot quota config " + path + L": " + WindowsErrorMessage(code);
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
    {
        CloseHandle(file);
        error = L"Invalid TrafficMonitor GitHub Copilot quota config size.";
        return false;
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD bytes_read = 0;
    const BOOL ok = bytes.empty()
        || ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &bytes_read, nullptr);
    CloseHandle(file);
    if (!ok)
    {
        error = L"Failed to read TrafficMonitor GitHub Copilot quota config " + path + L": " + WindowsErrorMessage(GetLastError());
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
        error = L"Failed to decode TrafficMonitor GitHub Copilot quota config as UTF-8.";
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
            error = L"Failed to create TrafficMonitor GitHub Copilot quota config directory: " + WindowsErrorMessage(GetLastError());
            return false;
        }
    }

    const int byte_length = WideCharToMultiByte(CP_UTF8, 0, content.data(), static_cast<int>(content.size()), nullptr, 0, nullptr, nullptr);
    if (byte_length < 0)
    {
        error = L"Failed to encode TrafficMonitor GitHub Copilot quota config as UTF-8.";
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
        error = L"Failed to open TrafficMonitor GitHub Copilot quota config for writing: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    DWORD written = 0;
    const BOOL ok = bytes.empty() || WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    if (!ok || written != bytes.size())
    {
        error = L"Failed to write TrafficMonitor GitHub Copilot quota config: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    return true;
}

std::optional<githubcopilotquota::PluginConfig> LoadGitHubConfig(std::wstring& error)
{
    std::wstring json;
    if (!ReadFileUtf8AsWide(githubcopilotquota::GetDefaultConfigPath(), json, true, error))
    {
        return std::nullopt;
    }
    return githubcopilotquota::ParseConfigJson(json, error);
}

bool SaveGitHubConfig(const githubcopilotquota::PluginConfig& config, std::wstring& error)
{
    return WriteFileWideAsUtf8(
        githubcopilotquota::GetDefaultConfigPath(),
        githubcopilotquota::SerializeConfigJson(config),
        error);
}

bool SameDisplayOptions(const githubcopilotquota::DisplayOptions& lhs, const githubcopilotquota::DisplayOptions& rhs)
{
    return lhs.quota_display == rhs.quota_display
        && lhs.reset_display == rhs.reset_display
        && lhs.show_reset_info == rhs.show_reset_info
        && lhs.show_remaining_credits == rhs.show_remaining_credits;
}

std::wstring AuthStatusText()
{
    std::wstring error;
    const auto target = githubcopilotquota::GetGitHubOAuthCredentialTarget();
    const auto stored = githubcopilotquota::ReadCredentialToken(
        target,
        error);
    if (stored.has_value())
    {
        const auto username = githubcopilotquota::ReadCredentialUsername(target, error);
        if (username.has_value())
        {
            return L"Status: signed in as " + *username + L".";
        }
        return L"Status: signed in with GitHub.";
    }

    return L"Status: not signed in.";
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
        auto* get_dpi_for_window = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (get_dpi_for_window != nullptr)
        {
            const UINT dpi = get_dpi_for_window(parent);
            if (dpi > 0)
            {
                return static_cast<int>(dpi);
            }
        }
    }

    if (user32 != nullptr)
    {
        using GetDpiForSystemFn = UINT(WINAPI*)();
        auto* get_dpi_for_system = reinterpret_cast<GetDpiForSystemFn>(
            GetProcAddress(user32, "GetDpiForSystem"));
        if (get_dpi_for_system != nullptr)
        {
            const UINT dpi = get_dpi_for_system();
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

void AdjustWindowRectForDpi(RECT* rect, DWORD style, DWORD ex_style, int dpi)
{
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr)
    {
        using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        auto* adjust_for_dpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
            GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        if (adjust_for_dpi != nullptr)
        {
            adjust_for_dpi(rect, style, FALSE, ex_style, static_cast<UINT>(dpi));
            return;
        }
    }

    AdjustWindowRectEx(rect, style, FALSE, ex_style);
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

struct OptionsDialogState
{
    std::wstring status;
    std::wstring body;
    githubcopilotquota::PluginConfig original_config;
    githubcopilotquota::PluginConfig config;
    bool options_changed{};
    int dpi{96};
    int result{IDCLOSE};
    HFONT title_font{};
    HFONT status_font{};
    HFONT body_font{};
    HBRUSH content_brush{};
    HBRUSH footer_brush{};
    HWND title_hwnd{};
    HWND status_hwnd{};
    HWND body_hwnd{};
};

void CaptureDisplayOptions(HWND window, OptionsDialogState& state)
{
    state.config.display.quota_display = IsDlgButtonChecked(window, kQuotaUsedRadio) == BST_CHECKED
        ? githubcopilotquota::QuotaDisplayMode::Used
        : githubcopilotquota::QuotaDisplayMode::Remaining;
    state.config.display.show_reset_info = IsDlgButtonChecked(window, kShowResetInfoCheckbox) == BST_CHECKED;
    state.config.display.reset_display = IsDlgButtonChecked(window, kResetTimeRadio) == BST_CHECKED
        ? githubcopilotquota::ResetDisplayMode::Time
        : githubcopilotquota::ResetDisplayMode::Countdown;
    state.config.display.show_remaining_credits = IsDlgButtonChecked(window, kShowCreditsCheckbox) == BST_CHECKED;
    state.options_changed = !SameDisplayOptions(state.config.display, state.original_config.display);
}

void SetResetChoiceEnabled(HWND window, bool enabled)
{
    EnableWindow(GetDlgItem(window, kResetCountdownRadio), enabled);
    EnableWindow(GetDlgItem(window, kResetTimeRadio), enabled);
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

    const int x = area.left + (area_width - window_width) / 2;
    const int y = area.top + (area_height - window_height) / 2;
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK OptionsDialogProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    auto* state = reinterpret_cast<OptionsDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message)
    {
    case WM_CREATE:
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<OptionsDialogState*>(create->lpCreateParams);
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
            L"GitHub Copilot authentication",
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

        y += ScaleForDpi(44, state->dpi);
        state->status_hwnd = CreateWindowExW(
            0,
            L"STATIC",
            state->status.c_str(),
            WS_CHILD | WS_VISIBLE,
            margin,
            y,
            content_width,
            ScaleForDpi(20, state->dpi),
            window,
            nullptr,
            nullptr,
            nullptr);
        SendMessageW(state->status_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(state->status_font), TRUE);

        y += ScaleForDpi(32, state->dpi);
        state->body_hwnd = CreateWindowExW(
            0,
            L"STATIC",
            state->body.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            margin,
            y,
            content_width,
            ScaleForDpi(42, state->dpi),
            window,
            nullptr,
            nullptr,
            nullptr);
        SendMessageW(state->body_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        y += ScaleForDpi(56, state->dpi);
        auto* quota_label = CreateWindowExW(
            0,
            L"STATIC",
            L"Quota:",
            WS_CHILD | WS_VISIBLE,
            margin,
            y,
            ScaleForDpi(90, state->dpi),
            ScaleForDpi(22, state->dpi),
            window,
            nullptr,
            nullptr,
            nullptr);
        SendMessageW(quota_label, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        auto* remaining_radio = CreateWindowExW(
            0,
            L"BUTTON",
            L"Remaining",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
            margin + ScaleForDpi(100, state->dpi),
            y,
            ScaleForDpi(120, state->dpi),
            ScaleForDpi(24, state->dpi),
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kQuotaRemainingRadio)),
            nullptr,
            nullptr);
        SendMessageW(remaining_radio, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        auto* used_radio = CreateWindowExW(
            0,
            L"BUTTON",
            L"Used",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
            margin + ScaleForDpi(230, state->dpi),
            y,
            ScaleForDpi(85, state->dpi),
            ScaleForDpi(24, state->dpi),
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kQuotaUsedRadio)),
            nullptr,
            nullptr);
        SendMessageW(used_radio, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        y += ScaleForDpi(34, state->dpi);
        auto* reset_label = CreateWindowExW(
            0,
            L"STATIC",
            L"Reset:",
            WS_CHILD | WS_VISIBLE,
            margin,
            y,
            ScaleForDpi(90, state->dpi),
            ScaleForDpi(22, state->dpi),
            window,
            nullptr,
            nullptr,
            nullptr);
        SendMessageW(reset_label, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        auto* show_reset_info = CreateWindowExW(
            0,
            L"BUTTON",
            L"Show reset info",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            margin + ScaleForDpi(100, state->dpi),
            y,
            ScaleForDpi(190, state->dpi),
            ScaleForDpi(24, state->dpi),
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShowResetInfoCheckbox)),
            nullptr,
            nullptr);
        SendMessageW(show_reset_info, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        y += ScaleForDpi(34, state->dpi);
        auto* countdown_radio = CreateWindowExW(
            0,
            L"BUTTON",
            L"Countdown",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
            margin + ScaleForDpi(100, state->dpi),
            y,
            ScaleForDpi(120, state->dpi),
            ScaleForDpi(24, state->dpi),
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResetCountdownRadio)),
            nullptr,
            nullptr);
        SendMessageW(countdown_radio, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        auto* reset_time_radio = CreateWindowExW(
            0,
            L"BUTTON",
            L"Reset time",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
            margin + ScaleForDpi(230, state->dpi),
            y,
            ScaleForDpi(120, state->dpi),
            ScaleForDpi(24, state->dpi),
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResetTimeRadio)),
            nullptr,
            nullptr);
        SendMessageW(reset_time_radio, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        y += ScaleForDpi(34, state->dpi);
        auto* show_credits = CreateWindowExW(
            0,
            L"BUTTON",
            L"Show remaining credits",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            margin + ScaleForDpi(100, state->dpi),
            y,
            ScaleForDpi(200, state->dpi),
            ScaleForDpi(24, state->dpi),
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShowCreditsCheckbox)),
            nullptr,
            nullptr);
        SendMessageW(show_credits, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        CheckRadioButton(
            window,
            kQuotaRemainingRadio,
            kQuotaUsedRadio,
            state->config.display.quota_display == githubcopilotquota::QuotaDisplayMode::Used ? kQuotaUsedRadio : kQuotaRemainingRadio);
        CheckRadioButton(
            window,
            kResetCountdownRadio,
            kResetTimeRadio,
            state->config.display.reset_display == githubcopilotquota::ResetDisplayMode::Time ? kResetTimeRadio : kResetCountdownRadio);
        SendMessageW(
            show_reset_info,
            BM_SETCHECK,
            state->config.display.show_reset_info ? BST_CHECKED : BST_UNCHECKED,
            0);
        SendMessageW(
            show_credits,
            BM_SETCHECK,
            state->config.display.show_remaining_credits ? BST_CHECKED : BST_UNCHECKED,
            0);
        SetResetChoiceEnabled(window, state->config.display.show_reset_info);

        const int button_y = client.bottom - footer_height + ScaleForDpi(12, state->dpi);
        const int gap = ScaleForDpi(10, state->dpi);
        const int close_width = ScaleForDpi(88, state->dpi);
        const int save_width = ScaleForDpi(80, state->dpi);
        const int sign_out_width = ScaleForDpi(94, state->dpi);
        const int sign_in_width = ScaleForDpi(156, state->dpi);
        const int button_height = ScaleForDpi(28, state->dpi);
        int button_x = client.right - margin - close_width;

        auto* close_button = CreateWindowExW(
            0,
            L"BUTTON",
            L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            button_x,
            button_y,
            close_width,
            button_height,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
            nullptr,
            nullptr);
        SendMessageW(close_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        button_x -= gap + save_width;
        auto* save_button = CreateWindowExW(
            0,
            L"BUTTON",
            L"Save",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            button_x,
            button_y,
            save_width,
            button_height,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveButton)),
            nullptr,
            nullptr);
        SendMessageW(save_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        button_x -= gap + sign_out_width;
        auto* sign_out_button = CreateWindowExW(
            0,
            L"BUTTON",
            L"Sign out",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            button_x,
            button_y,
            sign_out_width,
            button_height,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSignOutButton)),
            nullptr,
            nullptr);
        SendMessageW(sign_out_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);

        button_x -= gap + sign_in_width;
        auto* sign_in_button = CreateWindowExW(
            0,
            L"BUTTON",
            L"Sign in with GitHub",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            button_x,
            button_y,
            sign_in_width,
            button_height,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSignInButton)),
            nullptr,
            nullptr);
        SendMessageW(sign_in_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);
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
        if (command == kShowResetInfoCheckbox)
        {
            SetResetChoiceEnabled(window, IsDlgButtonChecked(window, kShowResetInfoCheckbox) == BST_CHECKED);
            return 0;
        }
        if (command == kSaveButton || command == kSignInButton || command == kSignOutButton)
        {
            CaptureDisplayOptions(window, *state);
            state->result = command;
            DestroyWindow(window);
            return 0;
        }
        if (command == IDCANCEL || command == IDCLOSE)
        {
            state->result = IDCLOSE;
            DestroyWindow(window);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (state != nullptr)
        {
            state->result = IDCLOSE;
        }
        DestroyWindow(window);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

int ShowOptionsModalDialog(
    HWND parent,
    const std::wstring& status,
    const std::wstring& body,
    githubcopilotquota::PluginConfig& config,
    bool& options_changed)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = OptionsDialogProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kOptionsDialogClassName;

    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return IDCLOSE;
    }

    OptionsDialogState state;
    state.status = status;
    state.body = body;
    state.original_config = config;
    state.config = config;
    state.dpi = DialogDpi(parent);
    state.title_font = CreateDialogFont(13, FW_NORMAL, state.dpi);
    state.status_font = CreateDialogFont(9, FW_BOLD, state.dpi);
    state.body_font = CreateDialogFont(9, FW_NORMAL, state.dpi);
    state.content_brush = CreateSolidBrush(RGB(255, 255, 255));
    state.footer_brush = CreateSolidBrush(GetSysColor(COLOR_3DFACE));

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD ex_style = WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    RECT window_rect{0, 0, ScaleForDpi(560, state.dpi), ScaleForDpi(334, state.dpi)};
    AdjustWindowRectForDpi(&window_rect, style, ex_style, state.dpi);

    HWND window = CreateWindowExW(
        ex_style,
        kOptionsDialogClassName,
        L"TrafficMonitor GitHub Copilot Quota",
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        parent,
        nullptr,
        instance,
        &state);

    if (window == nullptr)
    {
        const auto fallback = MessageBoxW(
            parent,
            (status + L"\n\n" + body + L"\n\nYes: Sign in with GitHub\nNo: Sign out\nCancel: Close").c_str(),
            L"TrafficMonitor GitHub Copilot Quota",
            MB_YESNOCANCEL | MB_ICONINFORMATION);
        if (fallback == IDYES)
        {
            state.result = kSignInButton;
        }
        else if (fallback == IDNO)
        {
            state.result = kSignOutButton;
        }
    }
    else
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
    DeleteObject(state.status_font);
    DeleteObject(state.body_font);
    DeleteObject(state.content_brush);
    DeleteObject(state.footer_brush);
    config = state.config;
    options_changed = state.options_changed;
    return state.result;
}

void ShowInfo(HWND parent, const std::wstring& message)
{
    MessageBoxW(parent, message.c_str(), L"TrafficMonitor GitHub Copilot Quota", MB_OK | MB_ICONINFORMATION);
}

void ShowError(HWND parent, const std::wstring& message)
{
    MessageBoxW(parent, message.c_str(), L"TrafficMonitor GitHub Copilot Quota", MB_OK | MB_ICONERROR);
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
    MessageBoxW(parent, message.c_str(), L"TrafficMonitor GitHub Copilot Quota", MB_OK | MB_ICONINFORMATION);
    return true;
}

std::wstring BuildGitHubCopilotSampleText(const githubcopilotquota::DisplayOptions& options)
{
    std::wstring sample = L" 100%";
    if (options.show_remaining_credits)
    {
        sample += L" 20.0kcr";
    }
    if (!options.show_reset_info)
    {
        return sample;
    }

    sample += options.reset_display == githubcopilotquota::ResetDisplayMode::Time
        ? L" 12-31 23:59"
        : L" 31d";
    return sample;
}

class GitHubCopilotQuotaItem final : public IPluginItem
{
public:
    const wchar_t* GetItemName() const override
    {
        return L"TrafficMonitor GitHub Copilot Quota";
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

    const wchar_t* GetItemValueSampleText() const override;

    int IsDrawResourceUsageGraph() const override
    {
        return 1;
    }

    float GetResourceUsageGraphValue() const override;

private:
    mutable std::wstring m_value;
    mutable std::wstring m_sample;
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
            return L"TrafficMonitor GitHub Copilot Quota";
        case TMI_DESCRIPTION:
            return L"Displays remaining GitHub Copilot quota in TrafficMonitor.";
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
        auto* parent = static_cast<HWND>(hParent);
        std::wstring config_error;
        auto config = LoadGitHubConfig(config_error);
        if (!config.has_value())
        {
            ShowError(parent, config_error);
            return OR_OPTION_UNCHANGED;
        }

        bool options_changed = false;
        const auto button = ShowOptionsModalDialog(
            parent,
            AuthStatusText(),
            L"Use GitHub sign-in to store a protected local OAuth token for TrafficMonitor.",
            *config,
            options_changed);

        if (options_changed)
        {
            if (!SaveGitHubConfig(*config, config_error))
            {
                ShowError(parent, config_error);
                return OR_OPTION_UNCHANGED;
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_config = *config;
        }

        if (button == kSaveButton)
        {
            return options_changed ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
        }

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

        return options_changed ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
    }

    std::wstring ValueText() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_has_snapshot)
        {
            return githubcopilotquota::FormatQuotaValue(m_snapshot.quota, m_snapshot.period.reset_at, std::time(nullptr), m_config.display);
        }

        if (m_last_error.empty())
        {
            return L" ...";
        }
        return L" ERR";
    }

    std::wstring SampleText() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return BuildGitHubCopilotSampleText(m_config.display);
    }

    float ResourceGraphValue() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_has_snapshot)
        {
            return githubcopilotquota::FormatResourceGraphValue(m_snapshot.quota, m_config.display);
        }
        return 0.0f;
    }

private:
    GitHubCopilotQuotaPlugin()
    {
        std::wstring error;
        if (const auto config = LoadGitHubConfig(error))
        {
            m_config = *config;
        }
    }

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
            m_config.display = result.snapshot.config.display;
            m_has_snapshot = true;
            m_last_error.clear();
            m_next_refresh = std::chrono::steady_clock::now() + std::chrono::minutes(15);
        }
        else
        {
            m_last_error = result.error.empty() ? L"Unknown TrafficMonitor GitHub Copilot quota error." : result.error;
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

        return L"Period mode: unknown";
    }

    std::wstring BuildTooltipLocked() const
    {
        std::wstring tooltip = L"TrafficMonitor GitHub Copilot quota";
        if (!m_snapshot.plan.empty())
        {
            tooltip += L"\nPlan: ";
            tooltip += m_snapshot.plan;
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
    githubcopilotquota::PluginConfig m_config;
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

const wchar_t* GitHubCopilotQuotaItem::GetItemValueSampleText() const
{
    m_sample = GitHubCopilotQuotaPlugin::Instance().SampleText();
    return m_sample.c_str();
}

float GitHubCopilotQuotaItem::GetResourceUsageGraphValue() const
{
    return GitHubCopilotQuotaPlugin::Instance().ResourceGraphValue();
}
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &GitHubCopilotQuotaPlugin::Instance();
}
