#include "EditorUi.h"

#include "EditorMaterial.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

#include <imgui.h>

namespace
{

constexpr float Pi = 3.14159265358979323846f;

struct EditorKeyToImGui
{
    EditorKey editorKey;
    ImGuiKey imguiKey;
};

constexpr std::array<EditorKeyToImGui, 29> kImGuiKeyTable = { {
    { EditorKey::Tab, ImGuiKey_Tab },
    { EditorKey::Left, ImGuiKey_LeftArrow },
    { EditorKey::Right, ImGuiKey_RightArrow },
    { EditorKey::Up, ImGuiKey_UpArrow },
    { EditorKey::Down, ImGuiKey_DownArrow },
    { EditorKey::PageUp, ImGuiKey_PageUp },
    { EditorKey::PageDown, ImGuiKey_PageDown },
    { EditorKey::Home, ImGuiKey_Home },
    { EditorKey::End, ImGuiKey_End },
    { EditorKey::Insert, ImGuiKey_Insert },
    { EditorKey::Delete, ImGuiKey_Delete },
    { EditorKey::Backspace, ImGuiKey_Backspace },
    { EditorKey::Space, ImGuiKey_Space },
    { EditorKey::Enter, ImGuiKey_Enter },
    { EditorKey::Escape, ImGuiKey_Escape },
    { EditorKey::F1, ImGuiKey_F1 },
    { EditorKey::Minus, ImGuiKey_Minus },
    { EditorKey::Equal, ImGuiKey_Equal },
    { EditorKey::LeftBracket, ImGuiKey_LeftBracket },
    { EditorKey::RightBracket, ImGuiKey_RightBracket },
    { EditorKey::Backslash, ImGuiKey_Backslash },
    { EditorKey::Semicolon, ImGuiKey_Semicolon },
    { EditorKey::Apostrophe, ImGuiKey_Apostrophe },
    { EditorKey::Comma, ImGuiKey_Comma },
    { EditorKey::Period, ImGuiKey_Period },
    { EditorKey::Slash, ImGuiKey_Slash },
    { EditorKey::Shift, ImGuiMod_Shift },
    { EditorKey::Ctrl, ImGuiMod_Ctrl },
    { EditorKey::Alt, ImGuiMod_Alt },
} };

ImGuiKey MapEditorKey(EditorKey key)
{
    if (key >= EditorKey::A && key <= EditorKey::Z)
    {
        return ImGuiKey(ImGuiKey_A + int(uint16_t(key) - uint16_t(EditorKey::A)));
    }

    if (key >= EditorKey::Num0 && key <= EditorKey::Num9)
    {
        return ImGuiKey(ImGuiKey_0 + int(uint16_t(key) - uint16_t(EditorKey::Num0)));
    }

    for (const EditorKeyToImGui &entry : kImGuiKeyTable)
    {
        if (entry.editorKey == key)
        {
            return entry.imguiKey;
        }
    }

    return ImGuiKey_None;
}

void ApplyEditorStyle()
{
    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.11f, 0.94f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.18f, 0.20f, 0.23f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.22f, 0.26f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.34f, 0.40f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.22f, 0.26f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.36f, 0.42f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.36f, 0.44f, 0.52f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.62f, 0.80f, 1.00f);
    colors[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.92f, 1.00f);
}

} // namespace

bool EditorUi::Initialize(EditorWindow *window, QrInstance instance, const std::string &fontPath,
                          std::string &error)
{
    if (m_ready)
    {
        return true;
    }

    m_window = window;
    m_instance = instance;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // Dragging on empty panel space must not move the window: the outline button in a light
    // field is easy to miss, and a stray drag used to slide the whole panel off screen.
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    ApplyEditorStyle();

    ImFont *font = nullptr;

    if (!fontPath.empty())
    {
        font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 17.0f);
    }

    if (font == nullptr)
    {
        ImFontConfig config;
        config.SizePixels = 17.0f;
        font = io.Fonts->AddFontDefault(&config);
    }

    unsigned char *pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        error = "ImGui font atlas is empty";
        ImGui::DestroyContext();
        return false;
    }

    QrMaterialCreateInfo info = {};
    info.size = { uint32_t(width), uint32_t(height) };
    info.textures.pDataAlbedoAlpha = pixels;
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    const QrResult result = qrCreateMaterial(m_instance, &info, &m_fontMaterial);

    if (result != QR_SUCCESS)
    {
        error = std::string("font atlas material: ") + qrGetResultDescription(result);
        ImGui::DestroyContext();
        return false;
    }

    io.Fonts->SetTexID(ImTextureID(uintptr_t(m_fontMaterial)));

    if (m_window != nullptr)
    {
        m_window->GetCursorPosition(m_mouseX, m_mouseY);
    }

    m_ready = true;
    return true;
}

void EditorUi::Shutdown()
{
    if (!m_ready)
    {
        return;
    }

    if (m_fontMaterial != QR_NO_MATERIAL && m_instance != nullptr)
    {
        qrDestroyMaterial(m_instance, m_fontMaterial);
        m_fontMaterial = QR_NO_MATERIAL;
    }

    ImGui::DestroyContext();
    m_ready = false;
}

bool EditorUi::WantsMouse() const
{
    if (!m_ready)
    {
        return false;
    }

    const ImGuiIO &io = ImGui::GetIO();
    return io.WantCaptureMouse;
}

bool EditorUi::WantsKeyboard() const
{
    if (!m_ready)
    {
        return false;
    }

    const ImGuiIO &io = ImGui::GetIO();
    return io.WantCaptureKeyboard;
}

bool EditorUi::WantsTextInput() const
{
    if (!m_ready)
    {
        return false;
    }

    const ImGuiIO &io = ImGui::GetIO();
    return io.WantTextInput;
}

void EditorUi::SetCursorVisible(bool visible)
{
    if (m_cursorVisible == visible)
    {
        return;
    }

    m_cursorVisible = visible;

    if (m_window != nullptr)
    {
        m_window->SetCursorVisible(visible);
    }
}

void EditorUi::UpdateModifiers(const EditorModifiers &modifiers)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Shift, modifiers.shift);
    io.AddKeyEvent(ImGuiMod_Ctrl, modifiers.ctrl);
    io.AddKeyEvent(ImGuiMod_Alt, modifiers.alt);
}

void EditorUi::AddKey(EditorKey editorKey, bool down, const EditorModifiers &modifiers)
{
    UpdateModifiers(modifiers);

    if (editorKey == EditorKey::Shift || editorKey == EditorKey::Ctrl ||
        editorKey == EditorKey::Alt)
    {
        return;
    }

    const ImGuiKey key = MapEditorKey(editorKey);

    if (key == ImGuiKey_None)
    {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(key, down);
}

namespace
{
void HandleUiMouseButton(EditorWindow *window, ImGuiIO &io, uint32_t &mouseButtons,
                         const EditorEvent &event)
{
    const uint32_t previousButtons = mouseButtons;

    if (window != nullptr && event.down && mouseButtons == 0)
    {
        window->SetMouseCapture(true);
    }

    io.AddMouseButtonEvent(event.button, event.down);

    if (event.down)
    {
        mouseButtons |= 1u << uint32_t(event.button);
        return;
    }

    mouseButtons &= ~(1u << uint32_t(event.button));

    if (previousButtons != 0 && mouseButtons == 0 && window != nullptr)
    {
        window->SetMouseCapture(false);
    }
}
} // namespace

bool EditorUi::HandleEvent(const EditorEvent &event)
{
    if (!m_ready)
    {
        return false;
    }

    ImGuiIO &io = ImGui::GetIO();

    switch (event.type)
    {
    case EditorEventType::MouseMove:
        m_mouseX = event.x;
        m_mouseY = event.y;
        io.AddMousePosEvent(m_mouseX, m_mouseY);
        return false;

    case EditorEventType::MouseButton:
        HandleUiMouseButton(m_window, io, m_mouseButtons, event);
        return false;

    case EditorEventType::MouseWheel:
        io.AddMouseWheelEvent(0.0f, event.wheelY);
        return false;

    case EditorEventType::Key:
        AddKey(event.key, event.down, event.modifiers);
        return false;

    case EditorEventType::Text:
        if (event.text[0] != '\0')
        {
            io.AddInputCharactersUTF8(event.text);
        }
        return false;

    case EditorEventType::Focus:
        if (!event.down)
        {
            m_mouseButtons = 0;
        }
        io.AddFocusEvent(event.down);
        return false;

    default:
        return false;
    }
}

void EditorUi::BeginFrame(float deltaSeconds, uint32_t clientWidth, uint32_t clientHeight)
{
    if (!m_ready)
    {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(clientWidth), float(clientHeight));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = deltaSeconds > 0.0f ? deltaSeconds : 1.0f / 60.0f;
    ImGui::NewFrame();
}

bool EditorUi::EndFrame(QrInstance instance, uint32_t clientWidth, uint32_t clientHeight)
{
    if (!m_ready || instance == nullptr || clientWidth == 0 || clientHeight == 0)
    {
        return false;
    }

    ImGui::Render();

    ImDrawData *drawData = ImGui::GetDrawData();

    if (drawData == nullptr || drawData->CmdListsCount == 0)
    {
        return true;
    }

    const float width = float(clientWidth);
    const float height = float(clientHeight);

    float projection[16] = {};
    projection[0] = 2.0f / width;
    projection[5] = 2.0f / height;
    projection[10] = -1.0f;
    projection[12] = -1.0f;
    projection[13] = -1.0f;
    projection[15] = 1.0f;

    QrViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = width;
    viewport.height = height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    QrTransform identity = {};
    identity.matrix[0][0] = 1.0f;
    identity.matrix[1][1] = 1.0f;
    identity.matrix[2][2] = 1.0f;

    for (int listIndex = 0; listIndex < drawData->CmdListsCount; listIndex++)
    {
        const ImDrawList *drawList = drawData->CmdLists[listIndex];

        m_vertices.resize(size_t(drawList->VtxBuffer.Size));

        for (int i = 0; i < drawList->VtxBuffer.Size; i++)
        {
            const ImDrawVert &source = drawList->VtxBuffer[i];
            QrVertex &vertex = m_vertices[size_t(i)];

            vertex = QrVertex();
            vertex.position[0] = source.pos.x;
            vertex.position[1] = source.pos.y;
            vertex.position[2] = 0.0f;
            vertex.normal[2] = 1.0f;
            vertex.texCoord[0] = source.uv.x;
            vertex.texCoord[1] = source.uv.y;
            vertex.packedColor = source.col;
        }

        for (const ImDrawCmd &command : drawList->CmdBuffer)
        {
            if (command.UserCallback != nullptr || command.ElemCount == 0)
            {
                continue;
            }

            const float clipLeft = std::clamp(float(command.ClipRect.x), 0.0f, width);
            const float clipTop = std::clamp(float(command.ClipRect.y), 0.0f, height);
            const float clipRight = std::clamp(float(command.ClipRect.z), 0.0f, width);
            const float clipBottom = std::clamp(float(command.ClipRect.w), 0.0f, height);

            const int32_t left = int32_t(std::floor(clipLeft));
            const int32_t top = int32_t(std::floor(clipTop));
            const uint32_t scissorWidth = uint32_t(std::max(0.0f, std::ceil(clipRight) - float(left)));
            const uint32_t scissorHeight = uint32_t(std::max(0.0f, std::ceil(clipBottom) - float(top)));

            if (scissorWidth == 0 || scissorHeight == 0)
            {
                continue;
            }

            m_indices.resize(command.ElemCount);
            uint32_t highest = 0;

            for (unsigned i = 0; i < command.ElemCount; i++)
            {
                const uint32_t index = uint32_t(drawList->IdxBuffer.Data[command.IdxOffset + i]) -
                                       uint32_t(command.VtxOffset);
                m_indices[i] = index;
                highest = std::max(highest, index);
            }

            QrRasterizedGeometryUploadInfo upload = {};
            upload.renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN;
            upload.vertexCount = highest + 1;
            upload.pVertices = m_vertices.data() + command.VtxOffset;
            upload.indexCount = command.ElemCount;
            upload.pIndices = m_indices.data();
            upload.transform = identity;
            upload.color = { 1.0f, 1.0f, 1.0f, 1.0f };
            upload.texCoordScale[0] = 1.0f;
            upload.texCoordScale[1] = 1.0f;
            upload.material = QrMaterial(uintptr_t(command.GetTexID()));
            upload.pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
            upload.blendFuncSrc = QR_BLEND_FACTOR_SRC_ALPHA;
            upload.blendFuncDst = QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            upload.scissor = { left, top, scissorWidth, scissorHeight };

            qrUploadRasterizedGeometry(instance, &upload, projection, &viewport);
        }
    }

    return true;
}

namespace
{

// The preview zoom the game keeps per widget: the state storage is keyed by the widget's ImGuiID,
// so two open mask blocks each keep their own view.
struct EditorPreviewView
{
    float zoom = 1.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
};

constexpr float kEditorPreviewZoomMax = 5.0f;

EditorPreviewView LoadEditorPreviewView(ImGuiID id)
{
    ImGuiStorage *storage = ImGui::GetStateStorage();
    EditorPreviewView view;

    view.zoom = std::clamp(storage->GetFloat(id, 1.0f), 1.0f, kEditorPreviewZoomMax);
    view.u0 = std::clamp(storage->GetFloat(id + 1, 0.0f), 0.0f, 1.0f - 1.0f / view.zoom);
    view.v0 = std::clamp(storage->GetFloat(id + 2, 0.0f), 0.0f, 1.0f - 1.0f / view.zoom);

    return view;
}

void StoreEditorPreviewView(ImGuiID id, const EditorPreviewView &view)
{
    ImGuiStorage *storage = ImGui::GetStateStorage();

    storage->SetFloat(id, view.zoom);
    storage->SetFloat(id + 1, view.u0);
    storage->SetFloat(id + 2, view.v0);
}

// Zooms about the pointer, so the texel under the cursor stays under it.
EditorPreviewView WheelEditorPreviewView(EditorPreviewView view, const ImVec2 &origin,
                                         const ImVec2 &size, float wheel)
{
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float width = size.x > 1.0f ? size.x : 1.0f;
    const float height = size.y > 1.0f ? size.y : 1.0f;
    const float fractionU = std::clamp((mouse.x - origin.x) / width, 0.0f, 1.0f);
    const float fractionV = std::clamp((mouse.y - origin.y) / height, 0.0f, 1.0f);
    const float centreU = view.u0 + fractionU / view.zoom;
    const float centreV = view.v0 + fractionV / view.zoom;

    view.zoom = std::clamp(view.zoom * std::pow(1.4f, wheel), 1.0f, kEditorPreviewZoomMax);
    view.u0 = std::clamp(centreU - fractionU / view.zoom, 0.0f, 1.0f - 1.0f / view.zoom);
    view.v0 = std::clamp(centreV - fractionV / view.zoom, 0.0f, 1.0f - 1.0f / view.zoom);

    return view;
}

struct PurpleOverlayBounds
{
    int minX = 0;
    int maxX = 0;
    int minY = 0;
    int maxY = 0;
    int gridW = 1;
    int gridH = 1;
};

PurpleOverlayBounds ComputePurpleOverlayBounds(const EditorPreviewView &view, uint32_t texWidth,
                                               uint32_t texHeight)
{
    const float span = 1.0f / view.zoom;
    PurpleOverlayBounds b;
    b.minX = std::clamp(int(std::floor(view.u0 * float(texWidth))), 0, int(texWidth) - 1);
    b.maxX = std::clamp(int(std::ceil((view.u0 + span) * float(texWidth))) - 1, b.minX,
                        int(texWidth) - 1);
    b.minY = std::clamp(int(std::floor(view.v0 * float(texHeight))), 0, int(texHeight) - 1);
    b.maxY = std::clamp(int(std::ceil((view.v0 + span) * float(texHeight))) - 1, b.minY,
                        int(texHeight) - 1);
    b.gridW = std::min(96, b.maxX - b.minX + 1);
    b.gridH = std::min(96, b.maxY - b.minY + 1);
    return b;
}

int SampleOverlayCellAlpha(const std::vector<float> &mask, uint32_t texWidth, int tx0, int tx1,
                           int ty0, int ty1)
{
    float sum = 0.0f;
    int count = 0;
    for (int y = ty0; y < ty1; y++)
    {
        const size_t row = size_t(y) * texWidth;
        for (int x = tx0; x < tx1; x++)
        {
            const float w = mask[row + size_t(x)];
            sum += (w > 0.0f) ? std::min(w, 1.0f) : 0.0f;
            count++;
        }
    }
    if (count <= 0)
    {
        return 0;
    }
    return int(std::clamp(std::lround((sum / float(count)) * 128.0f), 0l, 128l));
}

struct PurpleRunState
{
    int startAlpha = 0;
    int alphaSum = 0;
    int count = 0;
    float x0 = 0.0f;
};

void FlushPurpleRun(ImDrawList *drawList, float py0, float py1, float x1, PurpleRunState &run)
{
    if (run.count <= 0)
    {
        return;
    }
    const int avgAlpha = std::clamp((run.alphaSum + run.count / 2) / run.count, 1, 128);
    drawList->AddRectFilled(ImVec2(run.x0, py0), ImVec2(x1, py1), IM_COL32(216, 24, 255, avgAlpha));
    run.count = 0;
    run.alphaSum = 0;
}

struct PurpleOverlayRowCtx
{
    ImDrawList *drawList = nullptr;
    ImVec2 origin;
    ImVec2 size;
    EditorPreviewView view;
    uint32_t texWidth = 0;
    uint32_t texHeight = 0;
    const std::vector<float> &mask;
    PurpleOverlayBounds bounds;
};

void DrawPurpleOverlayRow(const PurpleOverlayRowCtx &ctx, int gy)
{
    const PurpleOverlayBounds &b = ctx.bounds;
    const int visW = b.maxX - b.minX + 1;
    const int visH = b.maxY - b.minY + 1;
    const float span = 1.0f / ctx.view.zoom;
    const float scaleX = ctx.size.x / span;
    const float scaleY = ctx.size.y / span;
    const int ty0 = b.minY + (gy * visH) / b.gridH;
    const int ty1 = std::min(b.maxY + 1, std::max(ty0 + 1, b.minY + ((gy + 1) * visH) / b.gridH));
    const float py0 = ctx.origin.y + (float(ty0) / float(ctx.texHeight) - ctx.view.v0) * scaleY;
    const float py1 = ctx.origin.y + (float(ty1) / float(ctx.texHeight) - ctx.view.v0) * scaleY;

    PurpleRunState run;
    for (int gx = 0; gx < b.gridW; gx++)
    {
        const int tx0 = b.minX + (gx * visW) / b.gridW;
        const int tx1 =
            std::min(b.maxX + 1, std::max(tx0 + 1, b.minX + ((gx + 1) * visW) / b.gridW));
        const float px0 = ctx.origin.x + (float(tx0) / float(ctx.texWidth) - ctx.view.u0) * scaleX;
        const int alpha = SampleOverlayCellAlpha(ctx.mask, ctx.texWidth, tx0, tx1, ty0, ty1);

        if (alpha <= 0)
        {
            FlushPurpleRun(ctx.drawList, py0, py1, px0, run);
            continue;
        }
        if (run.count > 0 && std::abs(alpha - run.startAlpha) <= 2)
        {
            run.alphaSum += alpha;
            run.count++;
            continue;
        }
        FlushPurpleRun(ctx.drawList, py0, py1, px0, run);
        run.startAlpha = alpha;
        run.alphaSum = alpha;
        run.count = 1;
        run.x0 = px0;
    }

    const float endX =
        ctx.origin.x + (float(b.maxX + 1) / float(ctx.texWidth) - ctx.view.u0) * scaleX;
    FlushPurpleRun(ctx.drawList, py0, py1, endX, run);
}

void DrawPreviewPurpleMaskOverlay(ImDrawList *drawList, const ImVec2 &origin, const ImVec2 &size,
                                  const EditorPreviewView &view, uint32_t texWidth,
                                  uint32_t texHeight, const std::vector<float> *overlayMask)
{
    if (drawList == nullptr || overlayMask == nullptr || texWidth == 0 || texHeight == 0 ||
        overlayMask->size() != size_t(texWidth) * size_t(texHeight))
    {
        return;
    }

    const PurpleOverlayBounds bounds = ComputePurpleOverlayBounds(view, texWidth, texHeight);
    const PurpleOverlayRowCtx ctx{ drawList, origin, size, view, texWidth, texHeight, *overlayMask,
                                   bounds };

    drawList->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    for (int gy = 0; gy < bounds.gridH; gy++)
    {
        DrawPurpleOverlayRow(ctx, gy);
    }
    drawList->PopClipRect();
}

void DrawPolygonConcaveFill(ImDrawList *drawList, const std::vector<ImVec2> &shape,
                            const std::vector<float> *overlayMask)
{
    if (overlayMask == nullptr || overlayMask->empty())
    {
        drawList->AddConcavePolyFilled(shape.data(), int(shape.size()), IM_COL32(255, 255, 255, 128));
    }
}

struct PreviewCanvasLayout
{
    ImVec2 origin;
    ImVec2 size;
    EditorPreviewView view;
};

PreviewCanvasLayout BeginPreviewCanvas(uint64_t texture, uint32_t texWidth, uint32_t texHeight,
                                       const std::vector<float> *overlayMask)
{
    const float width = std::max(32.0f, ImGui::GetContentRegionAvail().x);
    const float height =
        (texWidth > 0 && texHeight > 0) ? width * float(texHeight) / float(texWidth) : width;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, height);
    const ImGuiID viewId = ImGui::GetID("##preview_view");
    EditorPreviewView view = LoadEditorPreviewView(viewId);
    const bool over =
        ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + width, origin.y + height));

    if (over && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f)
    {
        view = WheelEditorPreviewView(view, origin, size, ImGui::GetIO().MouseWheel);
        StoreEditorPreviewView(viewId, view);
    }

    const float span = 1.0f / view.zoom;
    if (texture != 0)
    {
        ImGui::Image(ImTextureRef(ImTextureID(uintptr_t(texture))), size, ImVec2(view.u0, view.v0),
                     ImVec2(view.u0 + span, view.v0 + span));
    }
    else
    {
        ImGui::Dummy(size);
    }

    DrawPreviewPurpleMaskOverlay(ImGui::GetWindowDrawList(), origin, size, view, texWidth, texHeight,
                                 overlayMask);
    return PreviewCanvasLayout{ origin, size, view };
}

void DrawBrushCursorRings(ImDrawList *drawList, const ImVec2 &origin, const ImVec2 &size,
                          float span, uint32_t texWidth, const EditorBrushToolSettings &tool)
{
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float outerR = std::max(1.5f, tool.radiusTexels / float(texWidth) / span * size.x);

    drawList->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    drawList->AddCircle(mouse, outerR, IM_COL32(236, 140, 255, 240), 32, 1.5f);
    if (tool.hardness < 0.99f)
    {
        const float innerR = outerR * std::clamp(tool.hardness, 0.0f, 1.0f);
        if (innerR >= 1.0f)
        {
            drawList->AddCircle(mouse, innerR, IM_COL32(255, 255, 255, 175), 28, 1.0f);
        }
    }
    drawList->PopClipRect();
}

struct BrushStrokeState
{
    bool active = false;
    ImGuiID item = 0;
    int button = ImGuiMouseButton_Left;
    float lastU = 0.0f;
    float lastV = 0.0f;
    std::vector<uint8_t> baseMask;
    std::vector<float> accum;
};

EditorBrushStrokeSpec MakeBrushSegmentSpec(float u0, float v0, float u1, float v1,
                                           const EditorBrushToolSettings &tool, int button)
{
    return EditorBrushStrokeSpec{
        u0,
        v0,
        u1,
        v1,
        tool.radiusTexels,
        tool.hardness,
        tool.opacity,
        tool.erase || button == ImGuiMouseButton_Right,
    };
}

bool UpdateBrushStrokeState(ImGuiID item, uint32_t texWidth, uint32_t texHeight,
                            const ImVec2 &pointerUv, const EditorBrushToolSettings &tool,
                            std::vector<uint8_t> &brushMask)
{
    static BrushStrokeState s_stroke;
    bool changed = false;

    if (ImGui::IsItemActivated())
    {
        const int btn =
            ImGui::GetIO().MouseDown[ImGuiMouseButton_Right] ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;
        s_stroke.active = true;
        s_stroke.item = item;
        s_stroke.button = btn;
        s_stroke.lastU = pointerUv.x;
        s_stroke.lastV = pointerUv.y;
        s_stroke.baseMask = brushMask;
        s_stroke.accum.assign(brushMask.size(), 0.0f);
        const EditorBrushStrokeSpec seg =
            MakeBrushSegmentSpec(pointerUv.x, pointerUv.y, pointerUv.x, pointerUv.y, tool, btn);
        changed |= ApplyEditorBrushSegment(seg, texWidth, texHeight, s_stroke.baseMask,
                                           s_stroke.accum, brushMask);
    }
    else if (s_stroke.active && s_stroke.item == item)
    {
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(s_stroke.button))
        {
            if (pointerUv.x != s_stroke.lastU || pointerUv.y != s_stroke.lastV)
            {
                const EditorBrushStrokeSpec seg =
                    MakeBrushSegmentSpec(s_stroke.lastU, s_stroke.lastV, pointerUv.x, pointerUv.y,
                                         tool, s_stroke.button);
                changed |= ApplyEditorBrushSegment(seg, texWidth, texHeight, s_stroke.baseMask,
                                                   s_stroke.accum, brushMask);
                s_stroke.lastU = pointerUv.x;
                s_stroke.lastV = pointerUv.y;
            }
        }
        else
        {
            s_stroke.active = false;
            s_stroke.item = 0;
        }
    }

    return changed;
}

} // namespace

bool EditorImagePick(const char *id, uint64_t texture, uint32_t textureWidth, uint32_t textureHeight,
                     float &outU, float &outV, const std::vector<float> *overlayMask)
{
    ImGui::PushID(id);

    const PreviewCanvasLayout layout =
        BeginPreviewCanvas(texture, textureWidth, textureHeight, overlayMask);
    ImGui::SetCursorScreenPos(layout.origin);
    ImGui::InvisibleButton("##pick", layout.size, ImGuiButtonFlags_MouseButtonLeft);

    // While the pointer is on the image and Ctrl is down the wheel is the zoom's, not the panel's
    // scroll, so the picker claims it before the scroll does.
    if (ImGui::IsItemHovered() && ImGui::GetIO().KeyCtrl)
    {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    }

    bool picked = false;
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float span = 1.0f / layout.view.zoom;
        const float w = layout.size.x > 1.0f ? layout.size.x : 1.0f;
        const float h = layout.size.y > 1.0f ? layout.size.y : 1.0f;

        outU = std::clamp(layout.view.u0 + (mouse.x - layout.origin.x) / w * span, 0.0f, 1.0f);
        outV = std::clamp(layout.view.v0 + (mouse.y - layout.origin.y) / h * span, 0.0f, 1.0f);
        picked = true;
    }

    ImGui::PopID();
    return picked;
}

bool EditorPolygonEdit(const char *id, uint64_t texture, uint32_t textureWidth,
                       uint32_t textureHeight, std::vector<float> &uv, int32_t maxPoints,
                       const std::vector<float> *overlayMask)
{
    ImGui::PushID(id);

    int32_t points = int32_t(uv.size() / 2);

    if (maxPoints > 32)
    {
        maxPoints = 32;
    }

    if (textureWidth == 0 || textureHeight == 0 || points < 3 || points > maxPoints)
    {
        ImGui::PopID();

        return false;
    }

    // The gesture outlives a frame, so it is kept between calls and tied to the widget's item ID:
    // two open mask blocks must not act on each other's drag.
    static int32_t dragIndex = -1;
    static int32_t dragEdge = -1;
    static bool dragAll = false;
    static ImGuiID dragItem = 0;
    static float dragAnchorU = 0.0f;
    static float dragAnchorV = 0.0f;
    static float dragEdgeBase[2][2] = {};
    static bool dragEdgePerpendicular = false;
    static std::vector<float> dragBase;

    float width = ImGui::GetContentRegionAvail().x;

    if (width < 32.0f)
    {
        width = 32.0f;
    }

    // The grab radius is in pixels, the polygon is in uv: an 18 pixel halo around a drawn point.
    constexpr float grab = 18.0f;
    const float height = width * float(textureHeight) / float(textureWidth);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 size(width, height);
    const ImGuiID viewId = ImGui::GetID("##preview_view");
    EditorPreviewView view = LoadEditorPreviewView(viewId);

    if (ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + width, origin.y + height)) &&
        ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f)
    {
        view = WheelEditorPreviewView(view, origin, size, ImGui::GetIO().MouseWheel);
        StoreEditorPreviewView(viewId, view);
    }

    const float span = 1.0f / view.zoom;

    // The view maps the texture through the zoom, so a point of the polygon follows it and the
    // pointer keeps reading the texel it is on.
    const auto screenX = [&](float u) { return origin.x + (u - view.u0) / span * width; };
    const auto screenY = [&](float v) { return origin.y + (v - view.v0) / span * height; };

    ImGui::Image(ImTextureRef(ImTextureID(uintptr_t(texture))), size, ImVec2(view.u0, view.v0),
                 ImVec2(view.u0 + span, view.v0 + span));
    DrawPreviewPurpleMaskOverlay(ImGui::GetWindowDrawList(), origin, size, view, textureWidth,
                                 textureHeight, overlayMask);
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##mask", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

    if (ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + width, origin.y + height)) &&
        ImGui::GetIO().KeyCtrl)
    {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    }

    const ImGuiID item = ImGui::GetItemID();
    bool changed = false;
    int32_t hover = -1;

    const auto pointAt = [&](int32_t index, float &u, float &v) {
        u = uv[size_t(index) * 2];
        v = uv[size_t(index) * 2 + 1];
    };

    if (ImGui::IsItemHovered())
    {
        for (int32_t p = 0; p < points; ++p)
        {
            const float px = screenX(uv[size_t(p) * 2]);
            const float py = screenY(uv[size_t(p) * 2 + 1]);

            if (mouse.x >= px - grab && mouse.x <= px + grab && mouse.y >= py - grab &&
                mouse.y <= py + grab)
            {
                hover = p;
                break;
            }
        }

        ImGui::SetMouseCursor(hover >= 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
    }

    // The side nearest a uv position, with the point on it, so inserting and framing share it.
    const auto nearestEdge = [&](float u, float v, float &edgeU, float &edgeV) {
        int32_t best = 0;
        float bestDistance = 0.0f;

        for (int32_t p = 0; p < points; ++p)
        {
            const int32_t q = (p + 1) % points;
            float ax, ay, bx, by;
            pointAt(p, ax, ay);
            pointAt(q, bx, by);

            const float dx = bx - ax;
            const float dy = by - ay;
            const float lengthSquared = dx * dx + dy * dy;
            float t = 0.0f;

            if (lengthSquared > 1e-8f)
            {
                t = std::clamp(((u - ax) * dx + (v - ay) * dy) / lengthSquared, 0.0f, 1.0f);
            }

            const float ex = ax + t * dx;
            const float ey = ay + t * dy;
            const float d = (u - ex) * (u - ex) + (v - ey) * (v - ey);

            if (p == 0 || d < bestDistance)
            {
                bestDistance = d;
                best = p;
                edgeU = ex;
                edgeV = ey;
            }
        }

        return best;
    };

    const auto insidePolygon = [&](float u, float v) {
        bool inside = false;

        for (int32_t p = 0, q = points - 1; p < points; q = p++)
        {
            float ux, uy, vx, vy;
            pointAt(p, ux, uy);
            pointAt(q, vx, vy);

            if ((uy > v) != (vy > v) && u < (vx - ux) * (v - uy) / (vy - uy) + ux)
            {
                inside = !inside;
            }
        }

        return inside;
    };

    int32_t hoverEdge = -1;
    float hoverEdgeU = 0.0f;
    float hoverEdgeV = 0.0f;

    if (ImGui::IsItemHovered() && hover < 0)
    {
        const float u = std::clamp(view.u0 + (mouse.x - origin.x) / (width > 1.0f ? width : 1.0f) * span,
                                   0.0f, 1.0f);
        const float v =
            std::clamp(view.v0 + (mouse.y - origin.y) / (height > 1.0f ? height : 1.0f) * span, 0.0f, 1.0f);
        float edgeU = 0.0f;
        float edgeV = 0.0f;
        const int32_t edge = nearestEdge(u, v, edgeU, edgeV);
        const float reach = 9.0f * span / (width > 1.0f ? width : 1.0f);
        const float dx = u - edgeU;
        const float dy = v - edgeV;

        if (std::sqrt(dx * dx + dy * dy) < reach)
        {
            hoverEdge = edge;
            hoverEdgeU = edgeU;
            hoverEdgeV = edgeV;
        }

        if (hoverEdge >= 0)
        {
            if (ImGui::GetIO().KeyShift)
            {
                // Shift slides the side along its own normal, so the cursor says which way it steps.
                float ax, ay, bx, by;
                const int32_t q = (hoverEdge + 1) % points;
                pointAt(hoverEdge, ax, ay);
                pointAt(q, bx, by);

                const bool moreVertical = std::fabs(by - ay) >= std::fabs(bx - ax);

                ImGui::SetMouseCursor(moreVertical ? ImGuiMouseCursor_ResizeEW
                                                   : ImGuiMouseCursor_ResizeNS);
            }
            else
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
        }
    }

    const float pointerU =
        std::clamp(view.u0 + (mouse.x - origin.x) / (width > 1.0f ? width : 1.0f) * span, 0.0f, 1.0f);
    const float pointerV =
        std::clamp(view.v0 + (mouse.y - origin.y) / (height > 1.0f ? height : 1.0f) * span, 0.0f, 1.0f);
    const bool inside = insidePolygon(pointerU, pointerV);

    if (ImGui::IsItemActivated())
    {
        if (ImGui::GetIO().MouseDown[ImGuiMouseButton_Right])
        {
            if (hover >= 0 && points > 3)
            {
                uv.erase(uv.begin() + hover * 2, uv.begin() + hover * 2 + 2);
                changed = true;
                points--;
            }
        }
        else if (hover >= 0)
        {
            dragIndex = hover;
            dragEdge = -1;
            dragAll = false;
            dragItem = item;
        }
        else if (ImGui::GetIO().KeyCtrl && points < maxPoints)
        {
            float edgeU = 0.0f;
            float edgeV = 0.0f;
            const int32_t edge = nearestEdge(pointerU, pointerV, edgeU, edgeV);
            const int32_t at = (edge + 1) % points;

            uv.insert(uv.begin() + at * 2, { pointerU, pointerV });
            changed = true;
            points++;
            dragIndex = at;
            dragEdge = -1;
            dragAll = false;
            dragItem = item;
        }
        else if (hoverEdge >= 0)
        {
            const int32_t q = (hoverEdge + 1) % points;
            float ax, ay, bx, by;
            pointAt(hoverEdge, ax, ay);
            pointAt(q, bx, by);

            dragEdge = hoverEdge;
            dragEdgePerpendicular = ImGui::GetIO().KeyShift;
            dragEdgeBase[0][0] = ax;
            dragEdgeBase[0][1] = ay;
            dragEdgeBase[1][0] = bx;
            dragEdgeBase[1][1] = by;
            dragAnchorU = pointerU;
            dragAnchorV = pointerV;
            dragIndex = -1;
            dragAll = false;
            dragItem = item;
        }
        else if (inside)
        {
            dragAnchorU = pointerU;
            dragAnchorV = pointerV;
            dragBase = uv;
            dragIndex = -1;
            dragEdge = -1;
            dragAll = true;
            dragItem = item;
        }
    }

    if (dragItem == item && dragAll)
    {
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const float deltaU = pointerU - dragAnchorU;
            const float deltaV = pointerV - dragAnchorV;

            for (int32_t p = 0; p < points && size_t(p) * 2 + 1 < dragBase.size(); ++p)
            {
                const float u = std::clamp(dragBase[size_t(p) * 2] + deltaU, 0.0f, 1.0f);
                const float v = std::clamp(dragBase[size_t(p) * 2 + 1] + deltaV, 0.0f, 1.0f);

                if (uv[size_t(p) * 2] != u || uv[size_t(p) * 2 + 1] != v)
                {
                    uv[size_t(p) * 2] = u;
                    uv[size_t(p) * 2 + 1] = v;
                    changed = true;
                }
            }
        }
        else
        {
            dragAll = false;
            dragItem = 0;
        }
    }
    else if (dragItem == item && dragEdge >= 0 && dragEdge < points)
    {
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const int32_t q = (dragEdge + 1) % points;
            float deltaU = pointerU - dragAnchorU;
            float deltaV = pointerV - dragAnchorV;

            if (dragEdgePerpendicular)
            {
                // The side steps along its own normal, so the motion is projected on the
                // perpendicular and the side keeps the direction it had.
                const float edgeU = dragEdgeBase[1][0] - dragEdgeBase[0][0];
                const float edgeV = dragEdgeBase[1][1] - dragEdgeBase[0][1];
                const float length = std::sqrt(edgeU * edgeU + edgeV * edgeV);

                if (length > 1e-8f)
                {
                    const float normalU = -edgeV / length;
                    const float normalV = edgeU / length;
                    const float projection = deltaU * normalU + deltaV * normalV;

                    deltaU = normalU * projection;
                    deltaV = normalV * projection;
                }
                else
                {
                    deltaU = 0.0f;
                    deltaV = 0.0f;
                }
            }

            const float moved[2][2] = {
                { std::clamp(dragEdgeBase[0][0] + deltaU, 0.0f, 1.0f),
                  std::clamp(dragEdgeBase[0][1] + deltaV, 0.0f, 1.0f) },
                { std::clamp(dragEdgeBase[1][0] + deltaU, 0.0f, 1.0f),
                  std::clamp(dragEdgeBase[1][1] + deltaV, 0.0f, 1.0f) },
            };

            for (int32_t pair = 0; pair < 2; ++pair)
            {
                const int32_t index = pair == 0 ? dragEdge : q;

                if (uv[size_t(index) * 2] != moved[pair][0] ||
                    uv[size_t(index) * 2 + 1] != moved[pair][1])
                {
                    uv[size_t(index) * 2] = moved[pair][0];
                    uv[size_t(index) * 2 + 1] = moved[pair][1];
                    changed = true;
                }
            }
        }
        else
        {
            dragEdge = -1;
            dragItem = 0;
        }
    }
    else if (dragItem == item && dragIndex >= 0 && dragIndex < points)
    {
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (uv[size_t(dragIndex) * 2] != pointerU ||
                uv[size_t(dragIndex) * 2 + 1] != pointerV)
            {
                uv[size_t(dragIndex) * 2] = pointerU;
                uv[size_t(dragIndex) * 2 + 1] = pointerV;
                changed = true;
            }
        }
        else
        {
            dragIndex = -1;
            dragItem = 0;
        }
    }

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    std::vector<ImVec2> shape(static_cast<size_t>(points));

    for (int32_t p = 0; p < points; ++p)
    {
        shape[size_t(p)] =
            ImVec2(screenX(uv[size_t(p) * 2]), screenY(uv[size_t(p) * 2 + 1]));
    }

    // A zoomed view may carry points outside the preview, so the mask and its handles are clipped.
    drawList->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    DrawPolygonConcaveFill(drawList, shape, overlayMask);
    drawList->AddPolyline(shape.data(), points, IM_COL32(255, 255, 255, 220), ImDrawFlags_Closed,
                          1.5f);

    {
        const int32_t edge = (dragItem == item && dragEdge >= 0) ? dragEdge : hoverEdge;

        if (edge >= 0 && edge < points)
        {
            const int32_t q = (edge + 1) % points;

            drawList->AddLine(shape[size_t(edge)], shape[size_t(q)], IM_COL32(255, 210, 120, 255),
                              2.5f);
        }
    }

    for (int32_t p = 0; p < points; ++p)
    {
        const ImU32 fill = (p == hover || (dragItem == item && p == dragIndex))
                               ? IM_COL32(255, 210, 120, 255)
                               : IM_COL32(255, 255, 255, 255);

        drawList->AddCircleFilled(shape[size_t(p)], 9.0f, fill);
        drawList->AddCircle(shape[size_t(p)], 9.0f, IM_COL32(0, 0, 0, 180), 16, 1.5f);
    }

    drawList->PopClipRect();
    drawList->AddRect(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(150, 160, 185, 255));

    ImGui::PopID();

    return changed;
}

bool EditorBrushEdit(const char *id, uint64_t texture, uint32_t textureWidth,
                     uint32_t textureHeight, std::vector<uint8_t> &brushMask,
                     const EditorBrushToolSettings &tool, const std::vector<float> *overlayMask)
{
    ImGui::PushID(id);

    if (textureWidth == 0 || textureHeight == 0)
    {
        ImGui::PopID();
        return false;
    }

    const size_t expectedPixels = size_t(textureWidth) * size_t(textureHeight);
    if (brushMask.size() != expectedPixels)
    {
        brushMask.assign(expectedPixels, 0);
    }

    const PreviewCanvasLayout layout =
        BeginPreviewCanvas(texture, textureWidth, textureHeight, overlayMask);
    ImGui::SetCursorScreenPos(layout.origin);
    ImGui::InvisibleButton("##brush", layout.size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

    if (ImGui::IsItemHovered() && ImGui::GetIO().KeyCtrl)
    {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    }

    const float span = 1.0f / layout.view.zoom;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float w = layout.size.x > 1.0f ? layout.size.x : 1.0f;
    const float h = layout.size.y > 1.0f ? layout.size.y : 1.0f;
    const ImVec2 pointerUv(
        std::clamp(layout.view.u0 + (mouse.x - layout.origin.x) / w * span, 0.0f, 1.0f),
        std::clamp(layout.view.v0 + (mouse.y - layout.origin.y) / h * span, 0.0f, 1.0f));

    const bool changed = UpdateBrushStrokeState(ImGui::GetItemID(), textureWidth, textureHeight,
                                                pointerUv, tool, brushMask);

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
        DrawBrushCursorRings(drawList, layout.origin, layout.size, span, textureWidth, tool);
    }
    drawList->AddRect(layout.origin,
                      ImVec2(layout.origin.x + layout.size.x, layout.origin.y + layout.size.y),
                      IM_COL32(150, 160, 185, 255));

    ImGui::PopID();
    return changed;
}
