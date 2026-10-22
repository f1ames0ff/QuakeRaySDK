#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <qray/qray.h>

namespace
{

constexpr wchar_t kWindowClass[] = L"QuakeRayEditorWindow";
constexpr wchar_t kWindowTitle[] = L"QuakeRay Editor";

LRESULT CALLBACK EditorWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT paint = {};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT rect = {};
            GetClientRect(hwnd, &rect);
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand)
{
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = EditorWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kWindowClass;

    if (RegisterClassExW(&windowClass) == 0)
    {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        nullptr, nullptr, instance, nullptr);

    if (hwnd == nullptr)
    {
        return 1;
    }

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG message = {};

    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return int(message.wParam);
}
