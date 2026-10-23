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
