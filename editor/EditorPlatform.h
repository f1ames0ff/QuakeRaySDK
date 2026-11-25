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

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <qray/qray.h>

enum class EditorKey : uint16_t
{
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Tab,
    Left,
    Right,
    Up,
    Down,
    PageUp,
    PageDown,
    Home,
    End,
    Insert,
    Delete,
    Backspace,
    Space,
    Enter,
    Escape,
    F1,
    Minus,
    Equal,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Apostrophe,
    Comma,
    Period,
    Slash,
    Shift,
    Ctrl,
    Alt,
    Count
};

struct EditorModifiers
{
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

enum class EditorEventType
{
    MouseMove,
    MouseButton,
    MouseWheel,
    Key,
    Text,
    Focus,
    Close,
    Resize
};

struct EditorEvent
{
    EditorEventType type = EditorEventType::MouseMove;
    float x = 0.0f;
    float y = 0.0f;
    float wheelY = 0.0f;
    int32_t button = 0; // 0 = left, 1 = right, 2 = middle
    bool down = false;
    bool repeat = false;
    EditorKey key = EditorKey::Unknown;
    EditorModifiers modifiers = {};
    uint32_t width = 0;
    uint32_t height = 0;
    char text[32] = {};
};

struct EditorSurfaceStorage
{
#if defined(_WIN32)
    QrWin32SurfaceCreateInfo win32 = {};
#else
    QrXlibSurfaceCreateInfo xlib = {};
#endif
};

class EditorWindow
{
public:
    using EventHandler = std::function<void(const EditorEvent &)>;

    EditorWindow();
    ~EditorWindow();

    EditorWindow(const EditorWindow &) = delete;
    EditorWindow &operator=(const EditorWindow &) = delete;

    bool Create(const char *title, uint32_t width, uint32_t height, void *nativeInstance,
                int showCommand);
    void Destroy();
    bool IsValid() const;

    void PollEvents(const EventHandler &handler);
    bool QuitRequested() const;
    int ExitCode() const;

    void GetClientSize(uint32_t &outWidth, uint32_t &outHeight) const;
    void GetCursorPosition(float &outX, float &outY) const;
    bool WarpCursorToCenter(int32_t &outX, int32_t &outY);
    void SetCursorVisible(bool visible);
    void SetMouseCapture(bool capture);
    void BindSurface(QrInstanceCreateInfo &createInfo, EditorSurfaceStorage &storage) const;

private:
    friend void EditorShowError(const EditorWindow *window, const char *title, const char *message);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

void EditorDebugLog(const char *message);
void EditorShowError(const EditorWindow *window, const char *title, const char *message);
std::string EditorExecutableDirectory();
