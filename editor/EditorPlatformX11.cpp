// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include "EditorPlatform.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
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
#include <cerrno>
#include <climits>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace
{

constexpr unsigned long kX11None = 0L;
constexpr int kX11Success = 0;

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

constexpr std::array<KeysymEntry, 22> kCyrillicShortcutTable = { {
    { XK_Cyrillic_em, EditorKey::V },
    { XK_Cyrillic_EM, EditorKey::V },
    { XK_Cyrillic_es, EditorKey::C },
    { XK_Cyrillic_ES, EditorKey::C },
    { XK_Cyrillic_che, EditorKey::X },
    { XK_Cyrillic_CHE, EditorKey::X },
    { XK_Cyrillic_ef, EditorKey::A },
    { XK_Cyrillic_EF, EditorKey::A },
    { XK_Cyrillic_ya, EditorKey::Z },
    { XK_Cyrillic_YA, EditorKey::Z },
    { XK_Cyrillic_yeru, EditorKey::S },
    { XK_Cyrillic_YERU, EditorKey::S },
    { XK_Cyrillic_en, EditorKey::Y },
    { XK_Cyrillic_EN, EditorKey::Y },
    { XK_Cyrillic_ka, EditorKey::R },
    { XK_Cyrillic_KA, EditorKey::R },
    { XK_Cyrillic_tse, EditorKey::W },
    { XK_Cyrillic_TSE, EditorKey::W },
    { XK_Cyrillic_shcha, EditorKey::O },
    { XK_Cyrillic_SHCHA, EditorKey::O },
    { XK_Cyrillic_softsign, EditorKey::M },
    { XK_Cyrillic_SOFTSIGN, EditorKey::M },
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

EditorKey ResolveX11EditorKey(Display *display, XKeyEvent &keyEvent)
{
    const KeySym primary = XLookupKeysym(&keyEvent, 0);
    if (const EditorKey mapped = MapKeysymToEditorKey(primary); mapped != EditorKey::Unknown)
    {
        return mapped;
    }

    if (display != nullptr)
    {
        const KeyCode kc = KeyCode(keyEvent.keycode);
        for (unsigned int group = 0; group < 4; ++group)
        {
            const KeySym sym = XkbKeycodeToKeysym(display, kc, group, 0);
            if (const EditorKey mapped = MapKeysymToEditorKey(sym); mapped != EditorKey::Unknown)
            {
                return mapped;
            }
        }
    }

    for (const KeysymEntry &entry : kCyrillicShortcutTable)
    {
        if (entry.keysym == primary)
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
    Atom atomClipboard = kX11None;
    Atom atomUtf8String = kX11None;
    Atom atomTargets = kX11None;
    Atom atomIncr = kX11None;
    Atom atomClipProp = kX11None;
    Cursor invisibleCursor = kX11None;
    XIM xim = nullptr;
    XIC xic = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool quitRequested = false;
    int exitCode = 0;
    bool cursorVisible = true;
    bool mouseCaptured = false;
    bool pendingRepeat = false;
    unsigned int repeatKeycode = 0;
    std::string ownedClipboardText;

    void HandleSelectionRequest(const XSelectionRequestEvent &req) const
    {
        XSelectionEvent reply = {};
        reply.type = SelectionNotify;
        reply.display = req.display;
        reply.requestor = req.requestor;
        reply.selection = req.selection;
        reply.target = req.target;
        reply.time = req.time;
        reply.property = kX11None;

        if (req.selection == atomClipboard || req.selection == XA_PRIMARY)
        {
            const Atom destProp = req.property != kX11None ? req.property : req.target;
            if (req.target == atomTargets)
            {
                const Atom supported[] = { atomTargets, atomUtf8String, XA_STRING };
                XChangeProperty(req.display, req.requestor, destProp, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(supported), 3);
                reply.property = destProp;
            }
            else if (req.target == atomUtf8String || req.target == XA_STRING)
            {
                XChangeProperty(req.display, req.requestor, destProp, req.target, 8, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(ownedClipboardText.data()),
                                int(ownedClipboardText.size()));
                reply.property = destProp;
            }
        }

        XSendEvent(req.display, req.requestor, 0, NoEventMask, reinterpret_cast<XEvent *>(&reply));
        XFlush(req.display);
    }

    bool ReadSelectionProperty(std::string &out) const
    {
        Atom actualType = kX11None;
        int actualFormat = 0;
        unsigned long itemCount = 0;
        unsigned long bytesAfter = 0;
        unsigned char *propData = nullptr;
        if (XGetWindowProperty(display, window, atomClipProp, 0, LONG_MAX / 4, 1, AnyPropertyType,
                               &actualType, &actualFormat, &itemCount, &bytesAfter,
                               &propData) != kX11Success)
        {
            return false;
        }

        bool ok = false;
        if (propData != nullptr && actualType != atomIncr && actualFormat == 8 && itemCount > 0)
        {
            out.assign(reinterpret_cast<const char *>(propData), size_t(itemCount));
            ok = true;
        }
        if (propData != nullptr)
        {
            XFree(propData);
        }
        return ok;
    }

    bool RequestSelectionBytes(Atom selection, Atom target, std::string &out) const
    {
        if (selection == kX11None || target == kX11None)
        {
            return false;
        }

        XEvent stale = {};
        while (XCheckTypedWindowEvent(display, window, SelectionNotify, &stale) != 0)
        {
        }

        XDeleteProperty(display, window, atomClipProp);
        XConvertSelection(display, selection, target, atomClipProp, window, CurrentTime);
        XFlush(display);

        const int fd = ConnectionNumber(display);
        for (int attempt = 0; attempt < 15; ++attempt)
        {
            XEvent ev = {};
            if (XCheckTypedWindowEvent(display, window, SelectionNotify, &ev) != 0)
            {
                if (ev.xselection.property == kX11None)
                {
                    return false;
                }
                return ReadSelectionProperty(out);
            }

            pollfd pfd = { fd, POLLIN, 0 };
            if (poll(&pfd, 1, 10) > 0)
            {
                XEventsQueued(display, QueuedAfterReading);
            }
        }

        XEvent ev = {};
        if (XCheckTypedWindowEvent(display, window, SelectionNotify, &ev) != 0 &&
            ev.xselection.property != kX11None)
        {
            return ReadSelectionProperty(out);
        }
        return false;
    }

    bool ReadSelectionWithFallbacks(Atom selection, std::string &out) const
    {
        const ::Window owner = XGetSelectionOwner(display, selection);
        if (owner == kX11None)
        {
            return false;
        }
        if (owner == window && selection == atomClipboard)
        {
            out = ownedClipboardText;
            return !out.empty();
        }
        return RequestSelectionBytes(selection, atomUtf8String, out) ||
               RequestSelectionBytes(selection, XA_STRING, out);
    }

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

        const EditorKey key = ResolveX11EditorKey(display, keyEvent);
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

    void EmitConfigureEvent(const XConfigureEvent &configure, const EventHandler &handler)
    {
        const uint32_t newWidth = uint32_t(std::max(0, configure.width));
        const uint32_t newHeight = uint32_t(std::max(0, configure.height));
        if (newWidth == width && newHeight == height)
        {
            return;
        }
        width = newWidth;
        height = newHeight;
        EditorEvent event = {};
        event.type = EditorEventType::Resize;
        event.width = newWidth;
        event.height = newHeight;
        handler(event);
    }

    void EmitFocusEvent(const XFocusChangeEvent &focus, bool focused,
                        const EventHandler &handler) const
    {
        if (focus.mode == NotifyGrab || focus.mode == NotifyUngrab)
        {
            return;
        }
        EditorEvent event = {};
        event.type = EditorEventType::Focus;
        event.down = focused;
        handler(event);
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
            EmitConfigureEvent(xevent.xconfigure, handler);
            break;

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
            EmitFocusEvent(xevent.xfocus, xevent.type == FocusIn, handler);
            break;

        case SelectionRequest:
            HandleSelectionRequest(xevent.xselectionrequest);
            break;

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

    m_impl->atomClipboard = XInternAtom(m_impl->display, "CLIPBOARD", 0);
    m_impl->atomUtf8String = XInternAtom(m_impl->display, "UTF8_STRING", 0);
    m_impl->atomTargets = XInternAtom(m_impl->display, "TARGETS", 0);
    m_impl->atomIncr = XInternAtom(m_impl->display, "INCR", 0);
    m_impl->atomClipProp = XInternAtom(m_impl->display, "QRAY_CLIPBOARD_DATA", 0);

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
        if (m_impl->mouseCaptured)
        {
            XUngrabPointer(m_impl->display, CurrentTime);
            m_impl->mouseCaptured = false;
        }

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
    m_impl->mouseCaptured = false;
    m_impl->wmDeleteWindow = kX11None;
    m_impl->atomClipboard = kX11None;
    m_impl->atomUtf8String = kX11None;
    m_impl->atomTargets = kX11None;
    m_impl->atomIncr = kX11None;
    m_impl->atomClipProp = kX11None;
    m_impl->ownedClipboardText.clear();
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
    XSync(m_impl->display, 0);

    XEvent discarded = {};
    while (XCheckTypedWindowEvent(m_impl->display, m_impl->window, MotionNotify, &discarded) != 0)
    {
    }

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

void EditorWindow::SetMouseCapture(bool capture)
{
    if (!IsValid() || m_impl->mouseCaptured == capture)
    {
        return;
    }

    if (capture)
    {
        const unsigned long cursor =
            m_impl->cursorVisible ? kX11None : m_impl->invisibleCursor;
        const int status = XGrabPointer(
            m_impl->display, m_impl->window, 1,
            ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
            GrabModeAsync, GrabModeAsync, m_impl->window,
            cursor, CurrentTime);
        m_impl->mouseCaptured = (status == GrabSuccess);
    }
    else
    {
        XUngrabPointer(m_impl->display, CurrentTime);
        m_impl->mouseCaptured = false;
    }

    XFlush(m_impl->display);
}

bool EditorWindow::HasFocus() const
{
    if (!IsValid() || m_impl->display == nullptr)
    {
        return false;
    }

    Window focusWindow = kX11None;
    int revertTo = 0;
    XGetInputFocus(m_impl->display, &focusWindow, &revertTo);
    return focusWindow == m_impl->window;
}

bool EditorWindow::IsMouseCaptured() const
{
    return IsValid() && m_impl->mouseCaptured;
}

std::string EditorWindow::GetClipboardText()
{
    if (!IsValid())
    {
        return {};
    }

    std::string text;
    if (m_impl->ReadSelectionWithFallbacks(m_impl->atomClipboard, text))
    {
        return text;
    }

    m_impl->ReadSelectionWithFallbacks(XA_PRIMARY, text);
    return text;
}

void EditorWindow::SetClipboardText(const char *text)
{
    if (!IsValid())
    {
        return;
    }

    m_impl->ownedClipboardText = text != nullptr ? text : "";
    if (m_impl->atomClipboard != kX11None)
    {
        XSetSelectionOwner(m_impl->display, m_impl->atomClipboard, m_impl->window, CurrentTime);
        XFlush(m_impl->display);
    }
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

namespace
{

void BuildChildEnvironment(const std::vector<std::pair<std::string, std::string>> &overrides,
                           std::vector<std::string> &ownedEnv, std::vector<char *> &envp)
{
    ownedEnv.clear();
    envp.clear();

    if (::environ != nullptr)
    {
        for (char **entry = ::environ; *entry != nullptr; ++entry)
        {
            const std::string_view sv(*entry);
            const size_t eq = sv.find('=');
            const std::string_view key = (eq == std::string_view::npos) ? sv : sv.substr(0, eq);
            const bool overridden = std::any_of(
                overrides.begin(), overrides.end(),
                [key](const std::pair<std::string, std::string> &kv) { return kv.first == key; });
            if (!overridden)
            {
                ownedEnv.emplace_back(sv);
            }
        }
    }

    for (const auto &[k, v] : overrides)
    {
        ownedEnv.push_back(k + "=" + v);
    }

    envp.reserve(ownedEnv.size() + 1u);
    for (std::string &entry : ownedEnv)
    {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
}

bool ReadPipeErrno(int fd, int &outErr)
{
    for (;;)
    {
        const ssize_t r = read(fd, &outErr, sizeof(outErr));
        if (r < 0 && errno == EINTR)
        {
            continue;
        }
        close(fd);
        return r == ssize_t(sizeof(outErr));
    }
}

[[noreturn]] void ChildExecFailure(int pipeFd, int err)
{
    const ssize_t written = write(pipeFd, &err, sizeof(err));
    (void)written;
    close(pipeFd);
    _exit(127);
}

} // namespace

bool EditorSpawnProcess(const EditorProcessLaunch &launch, EditorChildProcess &outProcess,
                        std::string &outError)
{
    outProcess.handle = 0;
    if (launch.executable.empty() || access(launch.executable.c_str(), X_OK) != 0)
    {
        outError = "game executable not found or not executable: " + launch.executable;
        return false;
    }

    std::vector<std::string> ownedArgs;
    ownedArgs.reserve(launch.arguments.size() + 1u);
    ownedArgs.push_back(launch.executable);
    for (const std::string &arg : launch.arguments)
    {
        ownedArgs.push_back(arg);
    }

    std::vector<char *> argv;
    argv.reserve(ownedArgs.size() + 1u);
    for (std::string &arg : ownedArgs)
    {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> ownedEnv;
    std::vector<char *> envp;
    if (!launch.envOverrides.empty())
    {
        BuildChildEnvironment(launch.envOverrides, ownedEnv, envp);
    }

    int errPipe[2] = { -1, -1 };
    if (pipe(errPipe) != 0)
    {
        outError = std::string("failed to create process pipe: ") + std::strerror(errno);
        return false;
    }
    fcntl(errPipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(errPipe[1], F_SETFD, FD_CLOEXEC);

    const pid_t pid = fork();
    if (pid < 0)
    {
        const int err = errno;
        close(errPipe[0]);
        close(errPipe[1]);
        outError = std::string("fork failed: ") + std::strerror(err);
        return false;
    }

    if (pid == 0)
    {
        close(errPipe[0]);
        if (!launch.workingDirectory.empty() && chdir(launch.workingDirectory.c_str()) != 0)
        {
            ChildExecFailure(errPipe[1], errno);
        }
        if (envp.empty())
        {
            execv(launch.executable.c_str(), argv.data());
        }
        else
        {
            execve(launch.executable.c_str(), argv.data(), envp.data());
        }
        ChildExecFailure(errPipe[1], errno);
    }

    close(errPipe[1]);
    int childErr = 0;
    if (ReadPipeErrno(errPipe[0], childErr))
    {
        int status = 0;
        waitpid(pid, &status, 0);
        outError = "failed to start " + launch.executable + ": " + std::strerror(childErr);
        return false;
    }

    outProcess.handle = intptr_t(pid);
    return true;
}

bool EditorIsProcessRunning(EditorChildProcess &process, int *outExitCode)
{
    if (process.handle == 0)
    {
        return false;
    }

    const pid_t pid = pid_t(process.handle);
    int status = 0;
    const pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == 0)
    {
        return true;
    }

    if (outExitCode != nullptr)
    {
        if (waited > 0 && WIFEXITED(status))
        {
            *outExitCode = WEXITSTATUS(status);
        }
        else if (waited > 0 && WIFSIGNALED(status))
        {
            *outExitCode = 128 + WTERMSIG(status);
        }
        else
        {
            *outExitCode = -1;
        }
    }

    process.handle = 0;
    return false;
}

void EditorDetachProcess(EditorChildProcess &process)
{
    process.handle = 0;
}
