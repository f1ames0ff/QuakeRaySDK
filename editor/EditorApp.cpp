#include "EditorApp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

#include <windowsx.h>

#include <imgui.h>

namespace
{

struct FileBlob
{
    std::vector<uint8_t> bytes;
};

std::string Utf8FromWide(const wchar_t *text)
{
    if (text == nullptr)
    {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);

    if (length <= 1)
    {
        return {};
    }

    std::string result(size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);

    if (!result.empty() && result.back() == '\0')
    {
        result.pop_back();
    }

    return result;
}

void NormalizeFolder(std::string &folder)
{
    if (!folder.empty())
    {
        const char last = folder.back();

        if (last != '/' && last != '\\')
        {
            folder.push_back('\\');
        }
    }
}

void LookAt(const float eye[3], const float target[3], const float up[3], float view[16])
{
    float forward[3] = {
        target[0] - eye[0],
        target[1] - eye[1],
        target[2] - eye[2],
    };

    const float forwardLength = std::sqrt(forward[0] * forward[0] +
                                          forward[1] * forward[1] +
                                          forward[2] * forward[2]);
    forward[0] /= forwardLength;
    forward[1] /= forwardLength;
    forward[2] /= forwardLength;

    float side[3] = {
        forward[1] * up[2] - forward[2] * up[1],
        forward[2] * up[0] - forward[0] * up[2],
        forward[0] * up[1] - forward[1] * up[0],
    };

    const float sideLength = std::sqrt(side[0] * side[0] +
                                       side[1] * side[1] +
                                       side[2] * side[2]);
    side[0] /= sideLength;
    side[1] /= sideLength;
    side[2] /= sideLength;

    const float correctedUp[3] = {
        side[1] * forward[2] - side[2] * forward[1],
        side[2] * forward[0] - side[0] * forward[2],
        side[0] * forward[1] - side[1] * forward[0],
    };

    view[0] = side[0];
    view[1] = correctedUp[0];
    view[2] = -forward[0];
    view[3] = 0.0f;

    view[4] = side[1];
    view[5] = correctedUp[1];
    view[6] = -forward[1];
    view[7] = 0.0f;

    view[8] = side[2];
    view[9] = correctedUp[2];
    view[10] = -forward[2];
    view[11] = 0.0f;

    view[12] = -(side[0] * eye[0] + side[1] * eye[1] + side[2] * eye[2]);
    view[13] = -(correctedUp[0] * eye[0] + correctedUp[1] * eye[1] + correctedUp[2] * eye[2]);
    view[14] = forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2];
    view[15] = 1.0f;
}

}

EditorApp::EditorApp(HINSTANCE instance)
    : m_instanceHandle(instance)
{
}

EditorApp::~EditorApp()
{
    Shutdown();
}

void EditorApp::PrintMessage(const char *message, void *)
{
    if (message != nullptr)
    {
        OutputDebugStringA(message);
        OutputDebugStringA("\n");

        if (FILE *file = std::fopen("editor.log", "a"))
        {
            std::fprintf(file, "%s\n", message);
            std::fclose(file);
        }
    }
}

void EditorApp::OpenFile(const char *path, void *, const void **data, uint32_t *size,
                         void **fileHandle)
{
    *data = nullptr;
    *size = 0;
    *fileHandle = nullptr;

    if (path == nullptr)
    {
        return;
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file)
    {
        return;
    }

    const std::streamsize length = file.tellg();

    if (length <= 0)
    {
        return;
    }

    FileBlob *blob = new FileBlob();
    blob->bytes.resize(size_t(length));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(blob->bytes.data()), length);

    if (!file)
    {
        delete blob;
        return;
    }

    *data = blob->bytes.data();
    *size = uint32_t(blob->bytes.size());
    *fileHandle = blob;
}

void EditorApp::CloseFile(void *fileHandle, void *)
{
    delete static_cast<FileBlob *>(fileHandle);
}

LRESULT CALLBACK EditorApp::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    EditorApp *app = reinterpret_cast<EditorApp *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (app != nullptr && message != WM_CLOSE && message != WM_DESTROY && message != WM_PAINT)
    {
        app->HandleWindowMessage(message, wParam, lParam);
    }

    switch (message)
    {
        case WM_CLOSE:
            if (app != nullptr)
            {
                app->m_running = false;
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

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

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

bool EditorApp::CreateWindowInstance(int showCommand)
{
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = m_instanceHandle;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"QuakeRayEditorWindow";

    if (RegisterClassExW(&windowClass) == 0)
    {
        return false;
    }

    m_window = CreateWindowExW(
        0, windowClass.lpszClassName, L"QuakeRay Editor", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        nullptr, nullptr, m_instanceHandle, nullptr);

    if (m_window == nullptr)
    {
        return false;
    }

    SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(m_window, showCommand);
    UpdateWindow(m_window);
    return true;
}

bool EditorApp::CreateRenderer()
{
    QrWin32SurfaceCreateInfo surfaceInfo = {
        .hinstance = m_instanceHandle,
        .hwnd = m_window,
    };

    QrInstanceCreateInfo createInfo = {};
    createInfo.pAppName = "QuakeRay Editor";
    createInfo.pAppGUID = "5a7f79c2-38a2-4f3d-9a34-9b0c4f2a9c71";
    createInfo.pWin32SurfaceInfo = &surfaceInfo;
    createInfo.pfnPrint = PrintMessage;
    createInfo.pfnOpenFile = OpenFile;
    createInfo.pfnCloseFile = CloseFile;
    createInfo.pUserLoadFileData = this;
    createInfo.pShaderFolderPath = m_shaderFolder.c_str();
    createInfo.pBlueNoiseFilePath = m_blueNoisePath.c_str();
    createInfo.primaryRaysMaxAlbedoLayers = 2;
    createInfo.indirectIlluminationMaxAlbedoLayers = 1;
    createInfo.rayCullBackFacingTriangles = 1;
    createInfo.allowGeometryWithSkyFlag = 1;
    createInfo.rasterizedMaxVertexCount = 1u << 20;
    createInfo.rasterizedMaxIndexCount = 1u << 21;
    createInfo.rasterizedVertexColorGamma = 1;
    createInfo.rasterizedSkyCubemapSize = 256;
    createInfo.maxTextureCount = 4096;
    createInfo.textureSamplerForceMinificationFilterLinear = 1;
    createInfo.textureSamplerForceNormalMapFilterLinear = 1;
    createInfo.originalAlbedoAlphaTextureIsSRGB = 1;
    createInfo.originalRoughnessMetallicEmissionTextureIsSRGB = 0;
    createInfo.originalNormalTextureIsSRGB = 0;
    createInfo.overridenAlbedoAlphaTextureIsSRGB = 1;
    createInfo.overridenRoughnessMetallicEmissionTextureIsSRGB = 0;
    createInfo.overridenNormalTextureIsSRGB = 0;

    const QrResult result = qrCreateInstance(&createInfo, &m_instance);
    PrintMessage(("instance: " + std::string(qrGetResultDescription(result))).c_str(), this);

    if (result != QR_SUCCESS)
    {
        MessageBoxA(m_window, qrGetResultDescription(result), "QuakeRay Editor", MB_ICONERROR);
        return false;
    }

    return true;
}

bool EditorApp::CreateMaterial()
{
    if (m_instance == nullptr)
    {
        return false;
    }

    const uint8_t albedo[16] = {
        220, 190, 150, 255, 120, 100, 80, 255,
        120, 100, 80, 255, 220, 190, 150, 255,
    };

    QrMaterialCreateInfo info = {};
    info.size = { 2, 2 };
    info.textures.pDataAlbedoAlpha = albedo;
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;

    const QrResult result = qrCreateMaterial(m_instance, &info, &m_material);
    PrintMessage(("material: " + std::string(qrGetResultDescription(result))).c_str(), this);
    return result == QR_SUCCESS;
}

QrGeometryPassThroughType EditorApp::PassThroughTypeForSurface(EditorSurfaceKind kind)
{
    switch (kind)
    {
    case EditorSurfaceKind::Water:
        return QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT;
    case EditorSurfaceKind::Slime:
    case EditorSurfaceKind::Lava:
        return QR_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT;
    case EditorSurfaceKind::Fence:
        return QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED;
    default:
        return QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
    }
}

QrMaterial EditorApp::SurfaceMaterial(const EditorSurface &surface) const
{
    if (surface.texture >= 0 && size_t(surface.texture) < m_textureMaterials.size())
    {
        const QrMaterial material = m_textureMaterials[size_t(surface.texture)];

        if (material != QR_NO_MATERIAL)
        {
            return material;
        }
    }

    return m_material;
}

bool EditorApp::CreateTextureMaterials()
{
    if (m_instance == nullptr)
    {
        return false;
    }

    m_textureMaterials.assign(m_scene.textures.size(), QR_NO_MATERIAL);

    uint32_t created = 0;

    for (size_t i = 0; i < m_scene.textures.size(); i++)
    {
        const EditorTexture &texture = m_scene.textures[i];

        if (texture.width == 0 || texture.height == 0 ||
            texture.rgba.size() != size_t(texture.width) * size_t(texture.height) * 4)
        {
            continue;
        }

        QrMaterialCreateInfo info = {};
        info.size = { texture.width, texture.height };
        info.textures.pDataAlbedoAlpha = texture.rgba.data();
        info.filter = QR_SAMPLER_FILTER_LINEAR;
        info.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;

        QrMaterial material = QR_NO_MATERIAL;

        if (qrCreateMaterial(m_instance, &info, &material) == QR_SUCCESS)
        {
            m_textureMaterials[i] = material;
            created++;
        }
    }

    PrintMessage(("texture materials: " + std::to_string(created) + "/" +
                  std::to_string(m_scene.textures.size())).c_str(), this);

    return true;
}

bool EditorApp::LoadScene()
{
    const std::string sceneDirectory = EditorDirectoryOf(m_scenePath);
    const std::string base = sceneDirectory.empty() ? std::string(".") : sceneDirectory;

    const std::string candidates[] = {
        base,
        base + "\\id1",
        base + "\\..",
        base + "\\..\\id1",
        base + "\\..\\..",
        base + "\\..\\..\\id1",
    };

    if (!m_gamePath.empty())
    {
        m_assets.AddDirectory(m_gamePath);
        m_assets.AddArchiveOrDirectory(m_gamePath + "\\PAK0.PAK");
        m_assets.AddArchiveOrDirectory(m_gamePath + "\\PAK1.PAK");
    }

    for (const std::string &candidate : candidates)
    {
        m_assets.AddDirectory(candidate);
        m_assets.AddArchiveOrDirectory(candidate + "\\PAK0.PAK");
        m_assets.AddArchiveOrDirectory(candidate + "\\PAK1.PAK");
    }

    std::vector<uint8_t> palette;

    if (m_assets.ReadFile("gfx/palette.lmp", palette) && palette.size() >= 768)
    {
        m_palette.assign(palette.begin(), palette.begin() + 768);
    }
    else
    {
        PrintMessage("palette gfx/palette.lmp not found, using BSP embedded palettes", this);
    }

    std::string error;

    if (!LoadEditorScene(m_scenePath, m_palette.empty() ? nullptr : m_palette.data(), m_scene,
                         error))
    {
        MessageBoxA(m_window, error.c_str(), "QuakeRay Editor", MB_ICONERROR);
        return false;
    }

    PrintMessage(("scene loaded: " + m_scenePath).c_str(), this);
    return true;
}

bool EditorApp::UploadScene()
{
    if (m_instance == nullptr)
    {
        return false;
    }

    if (!CreateTextureMaterials())
    {
        return false;
    }

    const QrResult beginResult = qrBeginStaticGeometries(m_instance);

    if (beginResult != QR_SUCCESS)
    {
        PrintMessage(("begin static scene: " + std::string(qrGetResultDescription(beginResult))).c_str(), this);
        return false;
    }

    uint32_t uploadedSurfaces = 0;

    for (size_t surfaceIndex = 0; surfaceIndex < m_scene.surfaces.size(); surfaceIndex++)
    {
        const EditorSurface &surface = m_scene.surfaces[surfaceIndex];

        if (surface.vertices.empty() || surface.indices.empty())
        {
            continue;
        }

        QrGeometryUploadInfo upload = {};
        upload.uniqueID = uint32_t(surfaceIndex) + 1;
        upload.flags = QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT;
        upload.geomType = QR_GEOMETRY_TYPE_STATIC;
        upload.passThroughType = PassThroughTypeForSurface(surface.kind);
        upload.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
        upload.vertexCount = uint32_t(surface.vertices.size());
        upload.pVertices = surface.vertices.data();
        upload.indexCount = uint32_t(surface.indices.size());
        upload.pIndices = surface.indices.data();
        upload.layerColors[0] = { 1.0f, 1.0f, 1.0f, 1.0f };
        upload.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
        upload.geomMaterial.layerMaterials[0] = SurfaceMaterial(surface);
        upload.defaultRoughness = 0.7f;
        upload.defaultMetallicity = 0.0f;
        upload.defaultEmission = 0.0f;
        upload.transform.matrix[0][0] = 1.0f;
        upload.transform.matrix[1][1] = 1.0f;
        upload.transform.matrix[2][2] = 1.0f;

        const QrResult uploadResult = qrUploadGeometry(m_instance, &upload);

        if (uploadResult != QR_SUCCESS)
        {
            PrintMessage(("geometry upload for '" + surface.textureName + "': " +
                          std::string(qrGetResultDescription(uploadResult))).c_str(), this);
            return false;
        }

        uploadedSurfaces++;
    }

    if (uploadedSurfaces == 0 && !m_scene.vertices.empty() && !m_scene.indices.empty())
    {
        QrGeometryUploadInfo upload = {};
        upload.uniqueID = 1;
        upload.flags = QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT;
        upload.geomType = QR_GEOMETRY_TYPE_STATIC;
        upload.passThroughType = QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
        upload.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
        upload.vertexCount = uint32_t(m_scene.vertices.size());
        upload.pVertices = m_scene.vertices.data();
        upload.indexCount = uint32_t(m_scene.indices.size());
        upload.pIndices = m_scene.indices.data();
        upload.layerColors[0] = { 1.0f, 1.0f, 1.0f, 1.0f };
        upload.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
        upload.geomMaterial.layerMaterials[0] = m_material;
        upload.defaultRoughness = 0.7f;
        upload.defaultMetallicity = 0.0f;
        upload.defaultEmission = 0.0f;
        upload.transform.matrix[0][0] = 1.0f;
        upload.transform.matrix[1][1] = 1.0f;
        upload.transform.matrix[2][2] = 1.0f;

        const QrResult uploadResult = qrUploadGeometry(m_instance, &upload);

        if (uploadResult != QR_SUCCESS)
        {
            PrintMessage(("geometry upload: " + std::string(qrGetResultDescription(uploadResult))).c_str(), this);
            return false;
        }
    }

    PrintMessage(("geometry upload: " + std::to_string(uploadedSurfaces) + " surfaces").c_str(),
                 this);

    const QrResult submitResult = qrSubmitStaticGeometries(m_instance);
    PrintMessage(("submit static scene: " + std::string(qrGetResultDescription(submitResult))).c_str(), this);
    return submitResult == QR_SUCCESS;
}

void EditorApp::Frame()
{
    if (m_instance == nullptr)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    float deltaSeconds = std::chrono::duration<float>(now - m_lastFrameTime).count();
    m_lastFrameTime = now;

    if (!(deltaSeconds > 0.0f) || deltaSeconds > 0.25f)
    {
        deltaSeconds = 1.0f / 60.0f;
    }

    m_frameTimeMs = deltaSeconds * 1000.0f;

    RECT client = {};
    GetClientRect(m_window, &client);
    m_clientWidth = uint32_t(std::max<LONG>(0, client.right - client.left));
    m_clientHeight = uint32_t(std::max<LONG>(0, client.bottom - client.top));

    QrStartFrameInfo startInfo = {
        .presentMode = QR_PRESENT_MODE_VSYNC,
        .maxFrameLatency = 1,
        .requestShaderReload = 0,
        .wantFullscreenExclusive = 0,
        .hasFocus = 1,
    };

    if (qrStartFrame(m_instance, &startInfo) != QR_SUCCESS)
    {
        return;
    }

    UploadSun();

    m_ui.BeginFrame(deltaSeconds, m_clientWidth, m_clientHeight);
    DrawPanel(deltaSeconds);
    UpdateCamera(deltaSeconds);
    m_ui.EndFrame(m_instance, m_clientWidth, m_clientHeight);

    const float cosPitch = std::cos(m_cameraPitch);
    const float eye[3] = { m_cameraPosition[0], m_cameraPosition[1], m_cameraPosition[2] };
    const float forward[3] = {
        cosPitch * std::cos(m_cameraYaw),
        cosPitch * std::sin(m_cameraYaw),
        std::sin(m_cameraPitch),
    };

    float target[3] = {
        eye[0] + forward[0],
        eye[1] + forward[1],
        eye[2] + forward[2],
    };

    const float up[3] = { 0.0f, 0.0f, 1.0f };
    float sceneExtent = 10.0f;

    if (m_scene.hasBounds)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            sceneExtent = std::max(sceneExtent,
                                   m_scene.boundsMax[axis] - m_scene.boundsMin[axis]);
        }
    }

    QrDrawFrameInfo draw = {};
    LookAt(eye, target, up, draw.view);
    draw.worldUpVector = { 0.0f, 0.0f, 1.0f };
    draw.fovYRadians = 1.0471975512f;
    draw.cameraNear = std::max(0.1f, sceneExtent * 0.0005f);
    draw.cameraFar = std::max(1000.0f, sceneExtent * 8.0f);
    draw.rayLength = draw.cameraFar;
    draw.rayCullMaskWorld = QR_DRAW_FRAME_RAY_CULL_WORLD_0_BIT |
                            QR_DRAW_FRAME_RAY_CULL_WORLD_1_BIT |
                            QR_DRAW_FRAME_RAY_CULL_SKY_BIT;
    draw.currentTime = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_startTime).count();
    draw.renderUiOnly = 0;
    draw.enableCpuProfiling = 0;

    const QrResult drawResult = qrDrawFrame(m_instance, &draw);

    if (drawResult != QR_SUCCESS)
    {
        static bool loggedDrawFailure = false;

        if (!loggedDrawFailure)
        {
            loggedDrawFailure = true;
            PrintMessage(("draw: " + std::string(qrGetResultDescription(drawResult))).c_str(), this);
        }
    }
}

LRESULT EditorApp::HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    m_ui.HandleMessage(message, wParam, lParam);

    switch (message)
    {
    case WM_MOUSEMOVE:
    {
        const LONG x = GET_X_LPARAM(lParam);
        const LONG y = GET_Y_LPARAM(lParam);

        if (m_looking)
        {
            const float dx = float(x - m_lastMouse.x);
            const float dy = float(y - m_lastMouse.y);

            m_cameraYaw -= dx * m_cameraSensitivity;
            m_cameraPitch = std::clamp(m_cameraPitch - dy * m_cameraSensitivity, -1.55f, 1.55f);

            RECT client = {};
            GetClientRect(m_window, &client);

            POINT center = { (client.right - client.left) / 2, (client.bottom - client.top) / 2 };
            ClientToScreen(m_window, &center);
            SetCursorPos(center.x, center.y);

            m_lastMouse.x = (client.right - client.left) / 2;
            m_lastMouse.y = (client.bottom - client.top) / 2;
        }
        else
        {
            m_lastMouse.x = x;
            m_lastMouse.y = y;
        }

        break;
    }

    case WM_RBUTTONDOWN:
    {
        if (!m_ui.WantsMouse())
        {
            m_looking = true;
            m_ui.SetCursorVisible(false);

            RECT client = {};
            GetClientRect(m_window, &client);
            POINT center = { (client.right - client.left) / 2, (client.bottom - client.top) / 2 };
            ClientToScreen(m_window, &center);
            SetCursorPos(center.x, center.y);
            ScreenToClient(m_window, &center);
            m_lastMouse = center;
        }

        break;
    }

    case WM_RBUTTONUP:
        m_looking = false;
        m_ui.SetCursorVisible(true);
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wParam < 256)
        {
            m_keys[wParam] = true;
        }

        if (wParam == VK_F1)
        {
            m_showPanel = !m_showPanel;
        }
        else if (wParam == 'O' && (m_keys[VK_CONTROL] || (GetKeyState(VK_CONTROL) & 0x8000) != 0))
        {
            ResetCameraToScene();
        }
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wParam < 256)
        {
            m_keys[wParam] = false;
        }
        break;

    case WM_KILLFOCUS:
        for (bool &pressed : m_keys)
        {
            pressed = false;
        }

        m_looking = false;
        m_ui.SetCursorVisible(true);
        break;

    default:
        break;
    }

    return 0;
}

bool EditorApp::CreateUi()
{
    std::string error;

    m_fontPath = "fonts/Roboto-Regular.ttf";

    if (!m_ui.Initialize(m_window, m_instance, m_fontPath, error))
    {
        PrintMessage(("ui: " + error).c_str(), this);
        return false;
    }

    PrintMessage("ui ready", this);
    return true;
}

void EditorApp::ResetCameraToScene()
{
    if (m_scene.hasCamera)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            m_cameraPosition[axis] = m_scene.cameraPosition[axis];
        }

        m_cameraYaw = m_scene.cameraYawDegrees * 0.01745329252f;
        m_cameraPitch = 0.0f;
        return;
    }

    float center[3] = { 0.0f, 0.0f, 0.0f };
    float extent = 10.0f;

    if (m_scene.hasBounds)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            center[axis] = (m_scene.boundsMin[axis] + m_scene.boundsMax[axis]) * 0.5f;
            extent = std::max(extent, m_scene.boundsMax[axis] - m_scene.boundsMin[axis]);
        }
    }

    m_cameraPosition[0] = center[0] + extent * 0.8f;
    m_cameraPosition[1] = center[1] - extent * 1.0f;
    m_cameraPosition[2] = center[2] + extent * 0.6f;

    const float dx = center[0] - m_cameraPosition[0];
    const float dy = center[1] - m_cameraPosition[1];
    const float dz = center[2] - m_cameraPosition[2];

    m_cameraYaw = std::atan2(dy, dx);
    m_cameraPitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy));
    m_cameraSpeed = std::max(64.0f, extent * 0.5f);
}

void EditorApp::UpdateCamera(float deltaSeconds)
{
    if (m_ui.WantsKeyboard())
    {
        return;
    }

    const float cosPitch = std::cos(m_cameraPitch);

    const float forward[3] = {
        cosPitch * std::cos(m_cameraYaw),
        cosPitch * std::sin(m_cameraYaw),
        std::sin(m_cameraPitch),
    };

    const float right[3] = {
        std::sin(m_cameraYaw),
        -std::cos(m_cameraYaw),
        0.0f,
    };

    float speed = m_cameraSpeed * deltaSeconds;

    if (m_keys[VK_SHIFT])
    {
        speed *= 4.0f;
    }

    float movement[3] = { 0.0f, 0.0f, 0.0f };

    const auto addAxis = [&](int key, const float *axis, float scale) {
        if (key < 256 && m_keys[key])
        {
            movement[0] += axis[0] * scale;
            movement[1] += axis[1] * scale;
            movement[2] += axis[2] * scale;
        }
    };

    addAxis('W', forward, speed);
    addAxis('S', forward, -speed);
    addAxis('D', right, speed);
    addAxis('A', right, -speed);

    if (m_keys['E'] || m_keys[VK_SPACE])
    {
        movement[2] += speed;
    }

    if (m_keys['Q'] || m_keys[VK_CONTROL])
    {
        movement[2] -= speed;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        m_cameraPosition[axis] += movement[axis];
    }
}

void EditorApp::UploadSun()
{
    const float elevation = m_sunElevation * 0.01745329252f;
    const float azimuth = m_sunAzimuth * 0.01745329252f;
    const float cosElevation = std::cos(elevation);

    const float toSun[3] = {
        cosElevation * std::cos(azimuth),
        cosElevation * std::sin(azimuth),
        std::sin(elevation),
    };

    QrDirectionalLightUploadInfo sun = {};
    sun.uniqueID = 2;
    sun.color = { m_sunIntensity, m_sunIntensity, m_sunIntensity };
    sun.direction = { -toSun[0], -toSun[1], -toSun[2] };
    sun.angularDiameterDegrees = m_sunAngularDiameter;

    const QrResult result = qrUploadDirectionalLight(m_instance, &sun);

    if (result != QR_SUCCESS)
    {
        static bool logged = false;

        if (!logged)
        {
            logged = true;
            PrintMessage(("sun: " + std::string(qrGetResultDescription(result))).c_str(), this);
        }
    }
}

void EditorApp::DrawPanel(float deltaSeconds)
{
    if (!m_showPanel)
    {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("QuakeRay Editor", &m_showPanel, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const float fps = deltaSeconds > 0.0f ? 1.0f / deltaSeconds : 0.0f;
        ImGui::Text("Frame  %.2f ms  (%.0f FPS)", m_frameTimeMs, fps);
        ImGui::Text("Drawable  %ux%u", m_clientWidth, m_clientHeight);

        ImGui::Separator();
        ImGui::TextUnformatted("Scene");
        ImGui::Text("Vertices  %zu", m_scene.vertices.size());
        ImGui::Text("Triangles %zu", m_scene.indices.size() / 3);
        ImGui::Text("Surfaces  %zu  Textures %zu", m_scene.surfaces.size(),
                    m_scene.textures.size());

        if (m_scene.hasBounds)
        {
            ImGui::Text("Bounds    %.1f %.1f %.1f .. %.1f %.1f %.1f",
                        m_scene.boundsMin[0], m_scene.boundsMin[1], m_scene.boundsMin[2],
                        m_scene.boundsMax[0], m_scene.boundsMax[1], m_scene.boundsMax[2]);
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Camera");
        ImGui::Text("%.1f  %.1f  %.1f", m_cameraPosition[0], m_cameraPosition[1],
                    m_cameraPosition[2]);
        ImGui::Text("yaw %.1f deg  pitch %.1f deg", m_cameraYaw * 57.29577951f,
                    m_cameraPitch * 57.29577951f);
        ImGui::SliderFloat("Fly speed", &m_cameraSpeed, 16.0f, 8192.0f, "%.0f u/s");

        if (ImGui::Button("Reset camera"))
        {
            ResetCameraToScene();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Sun");
        ImGui::SliderFloat("Intensity", &m_sunIntensity, 0.0f, 64.0f, "%.2f");
        ImGui::SliderFloat("Azimuth", &m_sunAzimuth, 0.0f, 360.0f, "%.0f deg");
        ImGui::SliderFloat("Elevation", &m_sunElevation, -89.0f, 89.0f, "%.0f deg");
        ImGui::SliderFloat("Angular size", &m_sunAngularDiameter, 0.1f, 8.0f, "%.2f deg");

        ImGui::Separator();
        ImGui::TextUnformatted("RMB drag look  WASD move  Q/E down/up  Shift x4");
        ImGui::TextUnformatted("F1 panel  Ctrl+O reset camera");
    }

    ImGui::End();
}

int EditorApp::Run(int showCommand)
{
    m_startTime = std::chrono::steady_clock::now();
    m_scenePath = "editor_scene.obj";
    m_shaderFolder = "shaders";
    m_blueNoisePath = "textures/BlueNoise_LDR_RGBA_128.png";

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv != nullptr)
    {
        for (int i = 1; i < argc; i++)
        {
            const std::wstring argument = argv[i];

            if (i + 1 < argc && argument == L"--scene")
            {
                m_scenePath = Utf8FromWide(argv[++i]);
            }
            else if (i + 1 < argc && argument == L"--shaders")
            {
                m_shaderFolder = Utf8FromWide(argv[++i]);
            }
            else if (i + 1 < argc && argument == L"--blue-noise")
            {
                m_blueNoisePath = Utf8FromWide(argv[++i]);
            }
            else if (i + 1 < argc && argument == L"--gamedir")
            {
                m_gamePath = Utf8FromWide(argv[++i]);
            }
        }

        LocalFree(argv);
    }

    NormalizeFolder(m_shaderFolder);

    if (!CreateWindowInstance(showCommand))
    {
        return 1;
    }

    if (!CreateRenderer())
    {
        return 1;
    }

    CreateMaterial();

    if (!LoadScene())
    {
        return 1;
    }

    if (!UploadScene())
    {
        return 1;
    }

    ResetCameraToScene();
    CreateUi();

    m_lastFrameTime = std::chrono::steady_clock::now();

    MSG message = {};

    while (m_running)
    {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);

            if (message.message == WM_QUIT)
            {
                m_running = false;
                break;
            }
        }

        if (!m_running)
        {
            break;
        }

        Frame();
    }

    Shutdown();

    if (IsWindow(m_window))
    {
        DestroyWindow(m_window);
    }

    return int(message.wParam);
}

void EditorApp::Shutdown()
{
    m_ui.Shutdown();

    if (m_instance != nullptr)
    {
        for (QrMaterial &material : m_textureMaterials)
        {
            if (material != QR_NO_MATERIAL)
            {
                qrDestroyMaterial(m_instance, material);
                material = QR_NO_MATERIAL;
            }
        }
    }

    m_textureMaterials.clear();

    if (m_material != QR_NO_MATERIAL && m_instance != nullptr)
    {
        qrDestroyMaterial(m_instance, m_material);
        m_material = QR_NO_MATERIAL;
    }

    if (m_instance != nullptr)
    {
        qrDestroyInstance(m_instance);
        m_instance = nullptr;
    }
}
