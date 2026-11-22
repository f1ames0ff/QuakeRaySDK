#include "EditorPlatform.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <vector>

namespace
{

struct VkEntry
{
    UINT vk;
    EditorKey key;
};

constexpr std::array<VkEntry, 29> kVkTable = { {
    { VK_TAB, EditorKey::Tab },
    { VK_LEFT, EditorKey::Left },
    { VK_RIGHT, EditorKey::Right },
    { VK_UP, EditorKey::Up },
    { VK_DOWN, EditorKey::Down },
    { VK_PRIOR, EditorKey::PageUp },
    { VK_NEXT, EditorKey::PageDown },
    { VK_HOME, EditorKey::Home },
    { VK_END, EditorKey::End },
    { VK_INSERT, EditorKey::Insert },
    { VK_DELETE, EditorKey::Delete },
    { VK_BACK, EditorKey::Backspace },
    { VK_SPACE, EditorKey::Space },
    { VK_RETURN, EditorKey::Enter },
    { VK_ESCAPE, EditorKey::Escape },
    { VK_F1, EditorKey::F1 },
    { VK_OEM_MINUS, EditorKey::Minus },
    { VK_OEM_PLUS, EditorKey::Equal },
    { VK_OEM_4, EditorKey::LeftBracket },
    { VK_OEM_6, EditorKey::RightBracket },
    { VK_OEM_5, EditorKey::Backslash },
    { VK_OEM_1, EditorKey::Semicolon },
    { VK_OEM_7, EditorKey::Apostrophe },
    { VK_OEM_COMMA, EditorKey::Comma },
    { VK_OEM_PERIOD, EditorKey::Period },
    { VK_OEM_2, EditorKey::Slash },
    { VK_SHIFT, EditorKey::Shift },
    { VK_CONTROL, EditorKey::Ctrl },
    { VK_MENU, EditorKey::Alt },
} };

EditorKey MapVirtualKeyToEditorKey(UINT vk)
{
    if (vk >= 'A' && vk <= 'Z')
    {
        return static_cast<EditorKey>(uint16_t(EditorKey::A) + uint16_t(vk - 'A'));
    }

    if (vk >= '0' && vk <= '9')
    {
        return static_cast<EditorKey>(uint16_t(EditorKey::Num0) + uint16_t(vk - '0'));
    }

    for (const VkEntry &entry : kVkTable)
    {
        if (entry.vk == vk)
        {
            return entry.key;
        }
    }

    return EditorKey::Unknown;
}

EditorModifiers CurrentModifiers()
{
    EditorModifiers mods = {};
    mods.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    mods.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    mods.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    return mods;
}

std::wstring WideFromUtf8(const char *text)
{
    if (text == nullptr || text[0] == '\0')
    {
        return L"QuakeRay Editor";
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);

    if (length <= 1)
    {
        return L"QuakeRay Editor";
    }

    std::wstring wide(size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);

    if (!wide.empty() && wide.back() == L'\0')
    {
        wide.pop_back();
    }

    return wide;
}

std::string Utf8FromWide(const wchar_t *text)
{
    if (text == nullptr || text[0] == L'\0')
    {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);

    if (length <= 1)
    {
        return {};
    }

    std::string utf8(size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), length, nullptr, nullptr);

    if (!utf8.empty() && utf8.back() == '\0')
    {
        utf8.pop_back();
    }

    return utf8;
}

} // namespace

struct EditorWindow::Impl
{
    HINSTANCE hinstance = nullptr;
    HWND hwnd = nullptr;
    const EventHandler *activeHandler = nullptr;
    bool quitRequested = false;
    int exitCode = 0;
    bool cursorVisible = true;

    void Emit(const EditorEvent &event) const
    {
        if (activeHandler != nullptr && *activeHandler)
        {
            (*activeHandler)(event);
        }
    }

    bool HandleMouseMessage(UINT message, WPARAM wParam, LPARAM lParam) const
    {
        if (message == WM_MOUSEMOVE)
        {
            EditorEvent event = {};
            event.type = EditorEventType::MouseMove;
            event.x = float(GET_X_LPARAM(lParam));
            event.y = float(GET_Y_LPARAM(lParam));
            event.modifiers = CurrentModifiers();
            Emit(event);
            return true;
        }

        if (message == WM_MOUSEWHEEL)
        {
            EditorEvent event = {};
            event.type = EditorEventType::MouseWheel;
            event.wheelY = float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA);
            event.modifiers = CurrentModifiers();
            Emit(event);
            return true;
        }

        const bool isLeft = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP);
        const bool isRight = (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP);
        const bool isMiddle = (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP);

        if (!isLeft && !isRight && !isMiddle)
        {
            return false;
        }

        EditorEvent event = {};
        event.type = EditorEventType::MouseButton;
        event.button = isLeft ? 0 : (isRight ? 1 : 2);
        event.down = (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                      message == WM_MBUTTONDOWN);
        event.x = float(GET_X_LPARAM(lParam));
        event.y = float(GET_Y_LPARAM(lParam));
        event.modifiers = CurrentModifiers();
        Emit(event);
        return true;
    }

    bool HandleKeyOrTextMessage(UINT message, WPARAM wParam, LPARAM lParam) const
    {
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
            message == WM_KEYUP || message == WM_SYSKEYUP)
        {
            EditorEvent event = {};
            event.type = EditorEventType::Key;
            event.key = MapVirtualKeyToEditorKey(UINT(wParam));
            event.down = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
            event.repeat = event.down && ((lParam & (1LL << 30)) != 0);
            event.modifiers = CurrentModifiers();
            Emit(event);
            return true;
        }

        if (message == WM_CHAR)
        {
            const wchar_t character = wchar_t(wParam);
            EditorEvent event = {};
            event.type = EditorEventType::Text;
            const int length = WideCharToMultiByte(CP_UTF8, 0, &character, 1, event.text,
                                                   int(sizeof(event.text) - 1), nullptr, nullptr);

            if (length > 0)
            {
                event.text[length] = '\0';
                Emit(event);
            }

            return true;
        }

        return false;
    }

    void HandleWindowStateMessage(UINT message, LPARAM lParam) const
    {
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
        {
            EditorEvent event = {};
            event.type = EditorEventType::Focus;
            event.down = (message == WM_SETFOCUS);
            Emit(event);
            return;
        }

        if (message == WM_SIZE)
        {
            EditorEvent event = {};
            event.type = EditorEventType::Resize;
            event.width = uint32_t(LOWORD(lParam));
            event.height = uint32_t(HIWORD(lParam));
            Emit(event);
        }
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto *impl = reinterpret_cast<Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (impl != nullptr && message != WM_CLOSE && message != WM_DESTROY && message != WM_PAINT)
        {
            if (!impl->HandleMouseMessage(message, wParam, lParam) &&
                !impl->HandleKeyOrTextMessage(message, wParam, lParam))
            {
                impl->HandleWindowStateMessage(message, lParam);
            }
        }

        if (message == WM_CLOSE)
        {
            if (impl != nullptr)
            {
                EditorEvent event = {};
                event.type = EditorEventType::Close;
                impl->Emit(event);
            }

            return 0;
        }

        if (message == WM_DESTROY)
        {
            if (impl != nullptr)
            {
                impl->quitRequested = true;
            }

            PostQuitMessage(0);
            return 0;
        }

        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint = {};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT rect = {};
            GetClientRect(hwnd, &rect);
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(hwnd, &paint);
            return 0;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
};

EditorWindow::EditorWindow()
    : m_impl(std::make_unique<Impl>())
{
}

EditorWindow::~EditorWindow()
{
    Destroy();
}

bool EditorWindow::Create(const char *title, uint32_t width, uint32_t height, void *nativeInstance,
                          int showCommand)
{
    Destroy();

    m_impl->hinstance = nativeInstance != nullptr ? static_cast<HINSTANCE>(nativeInstance)
                                                  : GetModuleHandleW(nullptr);
    m_impl->quitRequested = false;
    m_impl->exitCode = 0;

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = Impl::WindowProc;
    windowClass.hInstance = m_impl->hinstance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"QuakeRayEditorWindow";

    RegisterClassExW(&windowClass);

    const std::wstring wideTitle = WideFromUtf8(title);
    m_impl->hwnd = CreateWindowExW(
        0, windowClass.lpszClassName, wideTitle.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, int(width), int(height),
        nullptr, nullptr, m_impl->hinstance, nullptr);

    if (m_impl->hwnd == nullptr)
    {
        return false;
    }

    SetWindowLongPtrW(m_impl->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(m_impl.get()));
    ShowWindow(m_impl->hwnd, showCommand > 0 ? showCommand : SW_SHOWDEFAULT);
    UpdateWindow(m_impl->hwnd);
    return true;
}

void EditorWindow::Destroy()
{
    if (!m_impl->cursorVisible)
    {
        ShowCursor(TRUE);
        m_impl->cursorVisible = true;
    }

    if (m_impl->hwnd != nullptr && IsWindow(m_impl->hwnd))
    {
        DestroyWindow(m_impl->hwnd);
    }

    m_impl->hwnd = nullptr;
}

bool EditorWindow::IsValid() const
{
    return m_impl->hwnd != nullptr && IsWindow(m_impl->hwnd);
}

void EditorWindow::PollEvents(const EventHandler &handler)
{
    m_impl->activeHandler = &handler;
    MSG message = {};

    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);

        if (message.message == WM_QUIT)
        {
            m_impl->quitRequested = true;
            m_impl->exitCode = int(message.wParam);
            break;
        }
    }

    m_impl->activeHandler = nullptr;
}

bool EditorWindow::QuitRequested() const
{
    return m_impl->quitRequested;
}

int EditorWindow::ExitCode() const
{
    return m_impl->exitCode;
}

void EditorWindow::GetClientSize(uint32_t &outWidth, uint32_t &outHeight) const
{
    RECT client = {};

    if (m_impl->hwnd != nullptr)
    {
        GetClientRect(m_impl->hwnd, &client);
    }

    outWidth = uint32_t(std::max<LONG>(0, client.right - client.left));
    outHeight = uint32_t(std::max<LONG>(0, client.bottom - client.top));
}

void EditorWindow::GetCursorPosition(float &outX, float &outY) const
{
    POINT point = {};

    if (m_impl->hwnd != nullptr)
    {
        GetCursorPos(&point);
        ScreenToClient(m_impl->hwnd, &point);
    }

    outX = float(point.x);
    outY = float(point.y);
}

bool EditorWindow::WarpCursorToCenter(int32_t &outX, int32_t &outY)
{
    if (m_impl->hwnd == nullptr)
    {
        return false;
    }

    RECT client = {};
    GetClientRect(m_impl->hwnd, &client);
    POINT center = { (client.right - client.left) / 2, (client.bottom - client.top) / 2 };
    ClientToScreen(m_impl->hwnd, &center);
    SetCursorPos(center.x, center.y);
    ScreenToClient(m_impl->hwnd, &center);
    outX = int32_t(center.x);
    outY = int32_t(center.y);
    return true;
}

void EditorWindow::SetCursorVisible(bool visible)
{
    if (m_impl->cursorVisible == visible)
    {
        return;
    }

    m_impl->cursorVisible = visible;
    ShowCursor(visible ? TRUE : FALSE);
}

void EditorWindow::SetMouseCapture(bool capture)
{
    if (m_impl->hwnd == nullptr)
    {
        return;
    }

    if (capture)
    {
        SetCapture(m_impl->hwnd);
    }
    else
    {
        ReleaseCapture();
    }
}

void EditorWindow::BindSurface(QrInstanceCreateInfo &createInfo, EditorSurfaceStorage &storage) const
{
    storage.win32 = {
        .hinstance = m_impl->hinstance,
        .hwnd = m_impl->hwnd,
    };
    createInfo.pWin32SurfaceInfo = &storage.win32;
}

void EditorDebugLog(const char *message)
{
    if (message != nullptr)
    {
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }
}

void EditorShowError(const EditorWindow *window, const char *title, const char *message)
{
    HWND owner = (window != nullptr && window->m_impl != nullptr) ? window->m_impl->hwnd : nullptr;
    MessageBoxA(owner, message != nullptr ? message : "",
                title != nullptr ? title : "QuakeRay Editor", MB_ICONERROR);
}

std::string EditorExecutableDirectory()
{
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');

    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));

        if (length == 0)
        {
            return {};
        }

        if (length < buffer.size())
        {
            const std::filesystem::path exePath(std::wstring(buffer.data(), length));
            return Utf8FromWide(exePath.parent_path().c_str());
        }

        buffer.resize(buffer.size() * 2, L'\0');
    }
}
