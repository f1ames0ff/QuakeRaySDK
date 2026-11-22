#include "EditorPlatform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#ifdef None
#undef None
#endif
#ifdef Success
#undef Success
#endif
#ifdef Always
#undef Always
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef Status
#undef Status
#endif

#include <algorithm>
#include <array>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unistd.h>
#include <vector>

namespace
{

constexpr unsigned long kX11None = 0L;

struct KeysymEntry
{
    KeySym keysym;
    EditorKey key;
};

constexpr std::array<KeysymEntry, 34> kKeysymTable = { {
    { XK_Tab, EditorKey::Tab },
    { XK_ISO_Left_Tab, EditorKey::Tab },
    { XK_Left, EditorKey::Left },
    { XK_Right, EditorKey::Right },
    { XK_Up, EditorKey::Up },
    { XK_Down, EditorKey::Down },
    { XK_Page_Up, EditorKey::PageUp },
    { XK_Page_Down, EditorKey::PageDown },
    { XK_Home, EditorKey::Home },
    { XK_End, EditorKey::End },
    { XK_Insert, EditorKey::Insert },
    { XK_Delete, EditorKey::Delete },
    { XK_BackSpace, EditorKey::Backspace },
    { XK_space, EditorKey::Space },
    { XK_Return, EditorKey::Enter },
    { XK_KP_Enter, EditorKey::Enter },
    { XK_Escape, EditorKey::Escape },
    { XK_F1, EditorKey::F1 },
    { XK_minus, EditorKey::Minus },
    { XK_equal, EditorKey::Equal },
    { XK_bracketleft, EditorKey::LeftBracket },
    { XK_bracketright, EditorKey::RightBracket },
    { XK_backslash, EditorKey::Backslash },
    { XK_semicolon, EditorKey::Semicolon },
    { XK_apostrophe, EditorKey::Apostrophe },
    { XK_comma, EditorKey::Comma },
    { XK_period, EditorKey::Period },
    { XK_slash, EditorKey::Slash },
    { XK_Shift_L, EditorKey::Shift },
    { XK_Shift_R, EditorKey::Shift },
    { XK_Control_L, EditorKey::Ctrl },
    { XK_Control_R, EditorKey::Ctrl },
    { XK_Alt_L, EditorKey::Alt },
    { XK_Alt_R, EditorKey::Alt },
} };

EditorKey MapKeysymToEditorKey(KeySym keysym)
{
    if (keysym >= XK_a && keysym <= XK_z)
    {
        return static_cast<EditorKey>(uint16_t(EditorKey::A) + uint16_t(keysym - XK_a));
    }

    if (keysym >= XK_A && keysym <= XK_Z)
    {
        return static_cast<EditorKey>(uint16_t(EditorKey::A) + uint16_t(keysym - XK_A));
    }

    if (keysym >= XK_0 && keysym <= XK_9)
    {
        return static_cast<EditorKey>(uint16_t(EditorKey::Num0) + uint16_t(keysym - XK_0));
    }

    for (const KeysymEntry &entry : kKeysymTable)
    {
        if (entry.keysym == keysym)
        {
            return entry.key;
        }
    }

    return EditorKey::Unknown;
}

EditorModifiers ModifiersFromX11State(unsigned int state, EditorKey key, bool down)
{
    EditorModifiers mods = {};
    mods.shift = (state & ShiftMask) != 0;
    mods.ctrl = (state & ControlMask) != 0;
    mods.alt = (state & Mod1Mask) != 0;

    if (key == EditorKey::Shift)
    {
        mods.shift = down;
    }
    else if (key == EditorKey::Ctrl)
    {
        mods.ctrl = down;
    }
    else if (key == EditorKey::Alt)
    {
        mods.alt = down;
    }

    return mods;
}

int LookupKeyText(XIC xic, XKeyEvent &keyEvent, char *outText, size_t maxBytes)
{
    if (maxBytes < 2)
    {
        return 0;
    }

    KeySym sym = kX11None;
    int length = 0;

#ifdef X_HAVE_UTF8_STRING
    if (xic != nullptr)
    {
        int status = 0;
        length = Xutf8LookupString(xic, &keyEvent, outText, int(maxBytes - 1), &sym, &status);
    }
    else
#endif
    {
        length = XLookupString(&keyEvent, outText, int(maxBytes - 1), &sym, nullptr);
    }

    if (length <= 0)
    {
        return 0;
    }

    outText[length] = '\0';

    if (length == 1)
    {
        const unsigned char byte = static_cast<unsigned char>(outText[0]);
        if (byte < 0x20 || byte == 0x7F)
        {
            return 0;
        }
    }

    return length;
}

} // namespace

struct EditorWindow::Impl
{
    Display *display = nullptr;
    ::Window window = kX11None;
    Atom wmDeleteWindow = kX11None;
    Cursor invisibleCursor = kX11None;
    XIM xim = nullptr;
    XIC xic = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool quitRequested = false;
    int exitCode = 0;
    bool cursorVisible = true;
    bool pendingRepeat = false;
    unsigned int repeatKeycode = 0;

    bool IsAutoRepeatRelease(const XKeyEvent &release) const
    {
        if (XEventsQueued(display, QueuedAfterReading) <= 0)
        {
            return false;
        }

        XEvent next = {};
        XPeekEvent(display, &next);
        return next.type == KeyPress && next.xkey.time == release.time &&
               next.xkey.keycode == release.keycode;
    }

    void EmitButtonEvent(const XButtonEvent &button, bool down,
                         const EventHandler &handler) const
    {
        if (down && (button.button == Button4 || button.button == Button5))
        {
            EditorEvent wheel = {};
            wheel.type = EditorEventType::MouseWheel;
            wheel.wheelY = button.button == Button4 ? 1.0f : -1.0f;
            wheel.x = float(button.x);
            wheel.y = float(button.y);
            wheel.modifiers = ModifiersFromX11State(button.state, EditorKey::Unknown, false);
            handler(wheel);
            return;
        }

        int32_t mapped = -1;
        if (button.button == Button1)
        {
            mapped = 0;
        }
        else if (button.button == Button3)
        {
            mapped = 1;
        }
        else if (button.button == Button2)
        {
            mapped = 2;
        }

        if (mapped < 0)
        {
            return;
        }

        EditorEvent event = {};
        event.type = EditorEventType::MouseButton;
        event.button = mapped;
        event.down = down;
        event.x = float(button.x);
        event.y = float(button.y);
        event.modifiers = ModifiersFromX11State(button.state, EditorKey::Unknown, false);
        handler(event);
    }

    void EmitKeyEvent(XKeyEvent keyEvent, bool down, const EventHandler &handler)
    {
        if (!down && IsAutoRepeatRelease(keyEvent))
        {
            pendingRepeat = true;
            repeatKeycode = keyEvent.keycode;
            return;
        }

        const bool isRepeat = down && pendingRepeat && repeatKeycode == keyEvent.keycode;
        pendingRepeat = false;
        repeatKeycode = 0;

        const KeySym keysym = XLookupKeysym(&keyEvent, 0);
        const EditorKey key = MapKeysymToEditorKey(keysym);
        const EditorModifiers mods = ModifiersFromX11State(keyEvent.state, key, down);

        EditorEvent event = {};
        event.type = EditorEventType::Key;
        event.key = key;
        event.down = down;
        event.repeat = isRepeat;
        event.modifiers = mods;
        handler(event);

        if (down && !mods.ctrl && !mods.alt)
        {
            EditorEvent textEvent = {};
            textEvent.type = EditorEventType::Text;
            if (LookupKeyText(xic, keyEvent, textEvent.text, sizeof(textEvent.text)) > 0)
            {
                handler(textEvent);
            }
        }
    }

    void DispatchEvent(const XEvent &xevent, const EventHandler &handler)
    {
        switch (xevent.type)
        {
        case ClientMessage:
            if (static_cast<Atom>(xevent.xclient.data.l[0]) == wmDeleteWindow)
            {
                EditorEvent event = {};
                event.type = EditorEventType::Close;
                handler(event);
            }
            break;

        case ConfigureNotify:
        {
            const uint32_t newWidth = uint32_t(std::max(0, xevent.xconfigure.width));
            const uint32_t newHeight = uint32_t(std::max(0, xevent.xconfigure.height));
            if (newWidth != width || newHeight != height)
            {
                width = newWidth;
                height = newHeight;
                EditorEvent event = {};
                event.type = EditorEventType::Resize;
                event.width = newWidth;
                event.height = newHeight;
                handler(event);
            }
            break;
        }

        case MotionNotify:
        {
            EditorEvent event = {};
            event.type = EditorEventType::MouseMove;
            event.x = float(xevent.xmotion.x);
            event.y = float(xevent.xmotion.y);
            event.modifiers = ModifiersFromX11State(xevent.xmotion.state, EditorKey::Unknown, false);
            handler(event);
            break;
        }

        case ButtonPress:
        case ButtonRelease:
            EmitButtonEvent(xevent.xbutton, xevent.type == ButtonPress, handler);
            break;

        case KeyPress:
        case KeyRelease:
            EmitKeyEvent(xevent.xkey, xevent.type == KeyPress, handler);
            break;

        case FocusIn:
        case FocusOut:
        {
            EditorEvent event = {};
            event.type = EditorEventType::Focus;
            event.down = (xevent.type == FocusIn);
            handler(event);
            break;
        }

        default:
            break;
        }
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

bool EditorWindow::Create(const char *title, uint32_t width, uint32_t height, void *, int)
{
    Destroy();

    m_impl->quitRequested = false;
    m_impl->exitCode = 0;

    std::setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");

    m_impl->display = XOpenDisplay(nullptr);
    if (m_impl->display == nullptr)
    {
        std::fprintf(stderr, "XOpenDisplay failed; check DISPLAY\n");
        return false;
    }

    const int screen = DefaultScreen(m_impl->display);
    const unsigned long black = BlackPixel(m_impl->display, screen);
    m_impl->window = XCreateSimpleWindow(
        m_impl->display, RootWindow(m_impl->display, screen),
        0, 0, width, height, 0, black, black);

    if (m_impl->window == kX11None)
    {
        std::fprintf(stderr, "XCreateSimpleWindow failed\n");
        Destroy();
        return false;
    }

    XStoreName(m_impl->display, m_impl->window, title != nullptr ? title : "QuakeRay Editor");

    m_impl->wmDeleteWindow = XInternAtom(m_impl->display, "WM_DELETE_WINDOW", 0);
    if (m_impl->wmDeleteWindow != kX11None)
    {
        XSetWMProtocols(m_impl->display, m_impl->window, &m_impl->wmDeleteWindow, 1);
    }

    XSelectInput(m_impl->display, m_impl->window,
                 ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask);

    static constexpr char kBlankBits[] = { 0 };
    Pixmap blank = XCreateBitmapFromData(m_impl->display, m_impl->window, kBlankBits, 1, 1);
    if (blank != kX11None)
    {
        XColor dummy = {};
        m_impl->invisibleCursor = XCreatePixmapCursor(m_impl->display, blank, blank, &dummy, &dummy, 0, 0);
        XFreePixmap(m_impl->display, blank);
    }

    m_impl->xim = XOpenIM(m_impl->display, nullptr, nullptr, nullptr);
    if (m_impl->xim != nullptr)
    {
        m_impl->xic = XCreateIC(m_impl->xim,
                                XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                                XNClientWindow, m_impl->window,
                                XNFocusWindow, m_impl->window,
                                nullptr);
    }

    XMapWindow(m_impl->display, m_impl->window);
    XSync(m_impl->display, 0);

    ::Window root = kX11None;
    int x = 0;
    int y = 0;
    unsigned int actualWidth = width;
    unsigned int actualHeight = height;
    unsigned int border = 0;
    unsigned int depth = 0;
    XGetGeometry(m_impl->display, m_impl->window, &root, &x, &y,
                 &actualWidth, &actualHeight, &border, &depth);
    m_impl->width = actualWidth;
    m_impl->height = actualHeight;
    return true;
}

void EditorWindow::Destroy()
{
    if (m_impl->xic != nullptr)
    {
        XDestroyIC(m_impl->xic);
        m_impl->xic = nullptr;
    }

    if (m_impl->xim != nullptr)
    {
        XCloseIM(m_impl->xim);
        m_impl->xim = nullptr;
    }

    if (m_impl->display != nullptr)
    {
        if (m_impl->invisibleCursor != kX11None)
        {
            XFreeCursor(m_impl->display, m_impl->invisibleCursor);
            m_impl->invisibleCursor = kX11None;
        }

        if (m_impl->window != kX11None)
        {
            XDestroyWindow(m_impl->display, m_impl->window);
            m_impl->window = kX11None;
        }

        XCloseDisplay(m_impl->display);
        m_impl->display = nullptr;
    }

    m_impl->width = 0;
    m_impl->height = 0;
    m_impl->cursorVisible = true;
}

bool EditorWindow::IsValid() const
{
    return m_impl->display != nullptr && m_impl->window != kX11None;
}

void EditorWindow::PollEvents(const EventHandler &handler)
{
    if (!IsValid() || !handler)
    {
        return;
    }

    while (XPending(m_impl->display) > 0)
    {
        XEvent xevent = {};
        XNextEvent(m_impl->display, &xevent);
        m_impl->DispatchEvent(xevent, handler);
    }
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
    outWidth = m_impl->width;
    outHeight = m_impl->height;
}

void EditorWindow::GetCursorPosition(float &outX, float &outY) const
{
    if (!IsValid())
    {
        outX = 0.0f;
        outY = 0.0f;
        return;
    }

    ::Window root = kX11None;
    ::Window child = kX11None;
    int rootX = 0;
    int rootY = 0;
    int winX = 0;
    int winY = 0;
    unsigned int mask = 0;

    if (XQueryPointer(m_impl->display, m_impl->window, &root, &child,
                      &rootX, &rootY, &winX, &winY, &mask) != 0)
    {
        outX = float(winX);
        outY = float(winY);
        return;
    }

    outX = 0.0f;
    outY = 0.0f;
}

bool EditorWindow::WarpCursorToCenter(int32_t &outX, int32_t &outY)
{
    if (!IsValid() || m_impl->width == 0 || m_impl->height == 0)
    {
        return false;
    }

    const int cx = int(m_impl->width / 2);
    const int cy = int(m_impl->height / 2);
    XWarpPointer(m_impl->display, kX11None, m_impl->window, 0, 0, 0, 0, cx, cy);
    XFlush(m_impl->display);
    outX = cx;
    outY = cy;
    return true;
}

void EditorWindow::SetCursorVisible(bool visible)
{
    if (!IsValid() || m_impl->cursorVisible == visible)
    {
        return;
    }

    m_impl->cursorVisible = visible;

    if (!visible && m_impl->invisibleCursor != kX11None)
    {
        XDefineCursor(m_impl->display, m_impl->window, m_impl->invisibleCursor);
    }
    else
    {
        XUndefineCursor(m_impl->display, m_impl->window);
    }

    XFlush(m_impl->display);
}

void EditorWindow::SetMouseCapture(bool)
{
    // X11 maintains an automatic passive pointer grab while a mouse button is pressed.
}

void EditorWindow::BindSurface(QrInstanceCreateInfo &createInfo, EditorSurfaceStorage &storage) const
{
    storage.xlib = {
        .dpy = m_impl->display,
        .window = static_cast<unsigned long>(m_impl->window),
    };
    createInfo.pXlibSurfaceCreateInfo = &storage.xlib;
}

void EditorDebugLog(const char *message)
{
    if (message != nullptr)
    {
        std::fprintf(stderr, "%s\n", message);
    }
}

void EditorShowError(const EditorWindow *, const char *title, const char *message)
{
    std::fprintf(stderr, "%s: %s\n",
                 title != nullptr ? title : "QuakeRay Editor",
                 message != nullptr ? message : "");
}

std::string EditorExecutableDirectory()
{
    std::vector<char> buffer(1024, '\0');

    for (;;)
    {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);

        if (length <= 0)
        {
            return {};
        }

        if (size_t(length) < buffer.size() - 1)
        {
            buffer[size_t(length)] = '\0';
            return std::filesystem::path(std::string(buffer.data(), size_t(length)))
                .parent_path()
                .string();
        }

        buffer.resize(buffer.size() * 2, '\0');
    }
}
