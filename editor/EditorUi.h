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
#include <string>
#include <vector>

#include <qray/qray.h>

#include "EditorPlatform.h"

// The game's QR_GUI_ImagePick: the image a colour is taken from, at the panel's full width and the
// texture's aspect, with Ctrl+wheel zoom. Returns true when the author clicked it, with outU/outV
// carrying the position in [0,1] as it is drawn on screen.
bool EditorImagePick(const char *id, uint64_t texture, uint32_t textureWidth, uint32_t textureHeight,
                     float &outU, float &outV, const std::vector<float> *overlayMask = nullptr);

// The game's QR_GUI_PolygonEdit: a uv polygon painted over the image it selects. Dragging a point
// moves it, dragging a side slides that side (Shift keeps its direction), dragging inside moves the
// whole outline, Ctrl+click inserts a point on the nearest side, right-click removes one and
// Ctrl+wheel zooms. Returns true when the polygon changed.
bool EditorPolygonEdit(const char *id, uint64_t texture, uint32_t textureWidth, uint32_t textureHeight,
                       std::vector<float> &uv, int32_t maxPoints,
                       const std::vector<float> *overlayMask = nullptr);

struct EditorBrushToolSettings
{
    float radiusTexels = 8.0f;
    float hardness = 0.5f;
    float opacity = 1.0f;
    bool erase = false;
};

// Interactive texel-space brush mask canvas over the preview texture. LMB paints (or erases when
// tool.erase is set), RMB always erases, Ctrl+wheel zooms. Returns true when brushMask changed.
bool EditorBrushEdit(const char *id, uint64_t texture, uint32_t textureWidth, uint32_t textureHeight,
                     std::vector<uint8_t> &brushMask, const EditorBrushToolSettings &tool,
                     const std::vector<float> *overlayMask = nullptr);

// Dear ImGui host used by the editor. The draw lists are rasterized through the
// renderer's rasterized-geometry path, so ImGui's own Vulkan backend and
// pipelines are not used.
class EditorUi
{
public:
    bool Initialize(EditorWindow *window, QrInstance instance, const std::string &fontPath,
                    std::string &error);
    void Shutdown();

    void BeginFrame(float deltaSeconds, uint32_t clientWidth, uint32_t clientHeight);
    bool EndFrame(QrInstance instance, uint32_t clientWidth, uint32_t clientHeight);

    // Returns true when the event was consumed by the UI and must not reach
    // the editor's camera handling.
    bool HandleEvent(const EditorEvent &event);

    bool WantsMouse() const;
    bool WantsKeyboard() const;
    bool WantsTextInput() const;

    float MouseX() const { return m_mouseX; }
    float MouseY() const { return m_mouseY; }

    void SetCursorVisible(bool visible);
    float DisplayScale() const { return m_scale; }

private:
    void AddKey(EditorKey editorKey, bool down, const EditorModifiers &modifiers);
    void UpdateModifiers(const EditorModifiers &modifiers);

    EditorWindow *m_window = nullptr;
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
