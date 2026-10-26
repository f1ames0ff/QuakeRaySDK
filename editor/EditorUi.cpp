#include "EditorUi.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include <imgui.h>

namespace
{

constexpr float Pi = 3.14159265358979323846f;

ImGuiKey MapVirtualKey(uint32_t key)
{
    if (key >= 'A' && key <= 'Z')
    {
        return ImGuiKey(ImGuiKey_A + int(key - 'A'));
    }

    if (key >= '0' && key <= '9')
    {
        return ImGuiKey(ImGuiKey_0 + int(key - '0'));
    }

    switch (key)
    {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_OEM_PLUS: return ImGuiKey_Equal;
    case VK_OEM_4: return ImGuiKey_LeftBracket;
    case VK_OEM_6: return ImGuiKey_RightBracket;
    case VK_OEM_5: return ImGuiKey_Backslash;
    case VK_OEM_1: return ImGuiKey_Semicolon;
    case VK_OEM_7: return ImGuiKey_Apostrophe;
    case VK_OEM_COMMA: return ImGuiKey_Comma;
    case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_OEM_2: return ImGuiKey_Slash;
    case VK_SHIFT: return ImGuiMod_Shift;
    case VK_CONTROL: return ImGuiMod_Ctrl;
    case VK_MENU: return ImGuiMod_Alt;
    default: return ImGuiKey_None;
    }
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

bool EditorUi::Initialize(HWND window, QrInstance instance, const std::string &fontPath,
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

    TrackMouse(window);
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

void EditorUi::TrackMouse(HWND window)
{
    if (window == nullptr)
    {
        return;
    }

    POINT point = {};
    GetCursorPos(&point);
    ScreenToClient(window, &point);
    m_mouseX = float(point.x);
    m_mouseY = float(point.y);
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

void EditorUi::SetCursorVisible(bool visible)
{
    if (m_cursorVisible == visible)
    {
        return;
    }

    m_cursorVisible = visible;
    ShowCursor(visible ? TRUE : FALSE);
}

void EditorUi::UpdateModifiers()
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
}

void EditorUi::AddKey(uint32_t virtualKey, bool down)
{
    if (virtualKey == VK_SHIFT || virtualKey == VK_CONTROL || virtualKey == VK_MENU)
    {
        UpdateModifiers();
        return;
    }

    const ImGuiKey key = MapVirtualKey(virtualKey);

    if (key == ImGuiKey_None)
    {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(key, down);
}

bool EditorUi::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!m_ready)
    {
        return false;
    }

    ImGuiIO &io = ImGui::GetIO();

    switch (message)
    {
    case WM_MOUSEMOVE:
        m_mouseX = float(GET_X_LPARAM(lParam));
        m_mouseY = float(GET_Y_LPARAM(lParam));
        io.AddMousePosEvent(m_mouseX, m_mouseY);
        return false;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    {
        const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                          message == WM_MBUTTONDOWN;
        const int button = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) ? 0
                         : (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) ? 1
                                                                                 : 2;
        SetCapture(m_window);
        io.AddMouseButtonEvent(button, down);

        if (down)
        {
            m_mouseButtons |= 1u << button;
        }
        else
        {
            m_mouseButtons &= ~(1u << button);

            if (m_mouseButtons == 0)
            {
                ReleaseCapture();
            }
        }

        return false;
    }

    case WM_MOUSEWHEEL:
        io.AddMouseWheelEvent(0.0f, float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA));
        return false;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        AddKey(uint32_t(wParam), true);
        return false;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        AddKey(uint32_t(wParam), false);
        return false;

    case WM_CHAR:
    {
        const wchar_t character = wchar_t(wParam);
        char utf8[8] = {};
        const int length = WideCharToMultiByte(CP_UTF8, 0, &character, 1, utf8, sizeof(utf8), nullptr,
                                               nullptr);

        if (length > 0)
        {
            utf8[length] = '\0';
            io.AddInputCharactersUTF8(utf8);
        }

        return false;
    }

    case WM_KILLFOCUS:
        io.AddFocusEvent(false);
        return false;

    case WM_SETFOCUS:
        io.AddFocusEvent(true);
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

} // namespace

bool EditorImagePick(const char *id, uint64_t texture, uint32_t textureWidth, uint32_t textureHeight,
                     float &outU, float &outV)
{
    ImGui::PushID(id);

    float width = ImGui::GetContentRegionAvail().x;

    if (width < 32.0f)
    {
        width = 32.0f;
    }

    const float height = (textureWidth > 0 && textureHeight > 0)
                             ? width * float(textureHeight) / float(textureWidth)
                             : width;
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

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##pick", size, ImGuiButtonFlags_MouseButtonLeft);

    // While the pointer is on the image and Ctrl is down the wheel is the zoom's, not the panel's
    // scroll, so the picker claims it before the scroll does.
    if (ImGui::IsItemHovered() && ImGui::GetIO().KeyCtrl)
    {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    }

    const bool hovered = ImGui::IsItemHovered();
    bool picked = false;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        outU = std::clamp(
            view.u0 + (mouse.x - origin.x) / (width > 1.0f ? width : 1.0f) * span, 0.0f, 1.0f);
        outV = std::clamp(
            view.v0 + (mouse.y - origin.y) / (height > 1.0f ? height : 1.0f) * span, 0.0f, 1.0f);
        picked = true;
    }

    ImGui::PopID();

    return picked;
}

bool EditorPolygonEdit(const char *id, uint64_t texture, uint32_t textureWidth,
                       uint32_t textureHeight, std::vector<float> &uv, int32_t maxPoints)
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
    drawList->AddConcavePolyFilled(shape.data(), points, IM_COL32(255, 255, 255, 128));
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
