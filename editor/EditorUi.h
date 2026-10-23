#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include <qray/qray.h>

// The game's QR_GUI_ImagePick: the image a colour is taken from, at the panel's full width and the
// texture's aspect, with Ctrl+wheel zoom. Returns true when the author clicked it, with outU/outV
// carrying the position in [0,1] as it is drawn on screen.
bool EditorImagePick(const char *id, uint64_t texture, uint32_t textureWidth, uint32_t textureHeight,
                     float &outU, float &outV);

// The game's QR_GUI_PolygonEdit: a uv polygon painted over the image it selects. Dragging a point
// moves it, dragging a side slides that side (Shift keeps its direction), dragging inside moves the
// whole outline, Ctrl+click inserts a point on the nearest side, right-click removes one and
// Ctrl+wheel zooms. Returns true when the polygon changed.
bool EditorPolygonEdit(const char *id, uint64_t texture, uint32_t textureWidth, uint32_t textureHeight,
                       std::vector<float> &uv, int32_t maxPoints);

// Dear ImGui host used by the editor. The draw lists are rasterized through the
// renderer's rasterized-geometry path, so ImGui's own Vulkan backend and
// pipelines are not used.
class EditorUi
{
public:
    bool Initialize(HWND window, QrInstance instance, const std::string &fontPath, std::string &error);
    void Shutdown();

    void BeginFrame(float deltaSeconds, uint32_t clientWidth, uint32_t clientHeight);
    bool EndFrame(QrInstance instance, uint32_t clientWidth, uint32_t clientHeight);

    // Returns true when the message was consumed by the UI and must not reach
    // the editor's camera handling.
    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool WantsMouse() const;
    bool WantsKeyboard() const;

    float MouseX() const { return m_mouseX; }
    float MouseY() const { return m_mouseY; }

    void SetCursorVisible(bool visible);
    float DisplayScale() const { return m_scale; }

private:
    void TrackMouse(HWND window);
    void AddKey(uint32_t virtualKey, bool down);
    void UpdateModifiers();

    HWND m_window = nullptr;
    QrInstance m_instance = nullptr;
    QrMaterial m_fontMaterial = QR_NO_MATERIAL;
    bool m_ready = false;
    bool m_cursorVisible = true;
    float m_scale = 1.0f;
    float m_mouseX = 0.0f;
    float m_mouseY = 0.0f;
    uint32_t m_mouseButtons = 0;
    std::vector<QrVertex> m_vertices;
    std::vector<uint32_t> m_indices;
};
