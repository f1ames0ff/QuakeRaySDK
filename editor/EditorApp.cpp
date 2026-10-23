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

// RT_LIGHT_Emit numbers map lights from UINT16_MAX upwards and keeps the editor's world
// geometry IDs (1..N) out of the way.
constexpr uint64_t kFirstLightUniqueID = uint64_t(UINT16_MAX);

// Every light the editor loads is a light of the map itself, which the game registers through
// RT_ClusterLightReachStatic(): rt_light_reach_static (25 m at QUAKEUNIT_IN_METERS 0.025, i.e.
// 1000 units) rather than the rt_light_reach_dynamic 10 m a moving light is held to. The top-up
// pass is handed the same number, so the reach survives its clamp unchanged.
constexpr float kLightReachUnits = 1000.0f;
constexpr float kLightTopUpReachUnits = 1000.0f;

uint64_t LightUniqueID(size_t index)
{
    return kFirstLightUniqueID + uint64_t(index);
}

// The editor's own lights start past the whole range a map could ever fill, so adding or
// removing one can never collide with a map light - the way the game numbers custom lights
// from UINT32_MAX + 1 while map lights come from UINT16_MAX.
constexpr uint64_t kFirstCustomLightUniqueID = kFirstLightUniqueID + 0x10000;

uint64_t CustomLightUniqueID(size_t index)
{
    return kFirstCustomLightUniqueID + uint64_t(index);
}

// The light styles rt_custom_style_names names, in the same order: the index is the engine's
// own d_lightstylevalue[] slot, so style 3 is the candle the map's `style` key also selects.
const char *const kEditorStyleNames[] = {
    "steady",        "flicker",      "slow strong pulse", "candle",       "fast strobe",
    "gentle pulse",  "flicker B",    "candle B",          "candle C",     "slow strobe",
    "fluorescent",   "slow pulse",
};

constexpr int32_t kEditorStyleCount = int32_t(sizeof(kEditorStyleNames) / sizeof(kEditorStyleNames[0]));

// -1 is the Entity tab's NONE: the game's own combo offers it for a map light that must stay
// unmodulated, so the editor has to name it rather than clamp it to steady.
const char *EditorStyleName(int32_t style)
{
    if (style < 0 || style >= kEditorStyleCount)
    {
        return "none";
    }

    return kEditorStyleNames[style];
}

// A light the editor invents starts where the game's Custom tab starts: RT_CUSTOM_RADIUS_DEFAULT
// (0.4 m, 16 units at 40 units per metre) and the 200 the map's `light` key defaults to.
constexpr float kEditorNewLightRadiusUnits = 0.4f * 40.0f;
constexpr float kEditorNewLightIntensity = 200.0f;

// A light is picked when the ray passes this close to its origin, the way QRE_LightUnderCrosshair
// accepts a light the crosshair is near rather than exactly on.
constexpr float kLightPickRadiusUnits = 24.0f;

// The game's QRE_GIZMO_LEN: the drawn length of one axis arrow. The drag divides the frozen
// screen direction by the pixels this length spans, so dragging a whole arrow moves the light by
// exactly one arrow length whatever the distance to the camera.
constexpr float kGizmoLengthUnits = 12.0f;

// The game's grab tolerance around an axis line, in pixels. A click this close to one grabs the
// axis instead of picking the surface behind it.
constexpr float kGizmoGrabPixels = 8.0f;

// The length of the arm a drag adds its movement to, as a fraction of one arrow: the game draws
// its arrow heads at 28% of the arrow back from the tip.
constexpr float kGizmoHeadLength = 0.28f;

// A drag against this tolerance counts as a click rather than a move, so a press that does not
// travel leaves the light where the panel typed it.
constexpr float kGizmoClickPixels = 2.0f;

// QRE_UpdateLightDrag leaves the grabbed light this far in front of whatever the ray hits, and
// hangs it this far ahead of the camera when the ray hits nothing at all.
constexpr float kGrabSurfaceOffsetUnits = 8.0f;
constexpr float kGrabAirDistanceUnits = 128.0f;

// Distance from a point to a segment, in the pixels the gizmo is hit-tested in.
float DistanceToSegment(float px, float py, float x0, float y0, float x1, float y1)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float lengthSquared = dx * dx + dy * dy;
    float t = 0.0f;

    if (lengthSquared > 0.0f)
    {
        t = std::clamp(((px - x0) * dx + (py - y0) * dy) / lengthSquared, 0.0f, 1.0f);
    }

    const float closestX = x0 + dx * t;
    const float closestY = y0 + dy * t;

    return std::sqrt((px - closestX) * (px - closestX) + (py - closestY) * (py - closestY));
}

// The vertical field of view the frame is drawn with. Picking and the selection overlay invert it,
// so they must not carry their own copy.
constexpr float kEditorFovYRadians = 1.0471975512f;

// Moller-Trumbore, without backface culling: a BSP face is picked from either side.
bool RayTriangle(const float origin[3], const float direction[3], const float a[3], const float b[3],
                 const float c[3], float &outDistance)
{
    const float edge1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float edge2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };

    const float pvec[3] = {
        direction[1] * edge2[2] - direction[2] * edge2[1],
        direction[2] * edge2[0] - direction[0] * edge2[2],
        direction[0] * edge2[1] - direction[1] * edge2[0],
    };

    const float determinant = edge1[0] * pvec[0] + edge1[1] * pvec[1] + edge1[2] * pvec[2];

    if (std::fabs(determinant) < 1e-9f)
    {
        return false;
    }

    const float inverse = 1.0f / determinant;
    const float tvec[3] = { origin[0] - a[0], origin[1] - a[1], origin[2] - a[2] };

    const float u = (tvec[0] * pvec[0] + tvec[1] * pvec[1] + tvec[2] * pvec[2]) * inverse;

    if (u < 0.0f || u > 1.0f)
    {
        return false;
    }

    const float qvec[3] = {
        tvec[1] * edge1[2] - tvec[2] * edge1[1],
        tvec[2] * edge1[0] - tvec[0] * edge1[2],
        tvec[0] * edge1[1] - tvec[1] * edge1[0],
    };

    const float v = (direction[0] * qvec[0] + direction[1] * qvec[1] + direction[2] * qvec[2]) * inverse;

    if (v < 0.0f || u + v > 1.0f)
    {
        return false;
    }

    const float distance = (edge2[0] * qvec[0] + edge2[1] * qvec[1] + edge2[2] * qvec[2]) * inverse;

    if (distance <= 0.0f)
    {
        return false;
    }

    outDistance = distance;
    return true;
}

const char *SurfaceKindName(EditorSurfaceKind kind)
{
    switch (kind)
    {
    case EditorSurfaceKind::Fence:
        return "fence";
    case EditorSurfaceKind::Water:
        return "water";
    case EditorSurfaceKind::Slime:
        return "slime";
    case EditorSurfaceKind::Lava:
        return "lava";
    case EditorSurfaceKind::Teleport:
        return "teleport";
    case EditorSurfaceKind::Sky:
        return "sky";
    default:
        return "opaque";
    }
}


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
    m_framesSinceSceneLoad = 0;
    m_lightStatsLogged = false;

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

    PrintMessage(("scene loaded: " + m_scenePath + " (" + std::to_string(m_scene.lights.size()) +
                  " lights)")
                     .c_str(),
                 this);

    m_materials.Rebuild(m_scene);
    m_selectedMaterial = m_materials.Empty() ? -1 : 0;
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

    return UploadStaticGeometry();
}

bool EditorApp::UploadStaticGeometry()
{
    if (m_instance == nullptr)
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
        upload.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
        upload.vertexCount = uint32_t(surface.vertices.size());
        upload.pVertices = surface.vertices.data();
        upload.indexCount = uint32_t(surface.indices.size());
        upload.pIndices = surface.indices.data();
        upload.layerColors[0] = { 1.0f, 1.0f, 1.0f, 1.0f };
        upload.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
        upload.geomMaterial.layerMaterials[0] = SurfaceMaterial(surface);
        ApplyEditorMaterial(m_materials.For(surface), PassThroughTypeForSurface(surface.kind),
                            upload);
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
        ApplyEditorMaterial(EditorMaterialParams{}, QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE, upload);
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

bool EditorApp::UploadWorldClusters()
{
    const EditorWorldClusters &clusters = m_scene.worldClusters;

    if (clusters.numLeafs == 0 || clusters.mins.empty())
    {
        // An OBJ scene has no leaf table: the renderer stays without world clusters and the
        // map lights are lit as plain spherical lights.
        return true;
    }

    QrWorldLightsUploadInfo upload = {};
    upload.flags = 0;
    upload.numClusters = uint32_t(clusters.mins.size());
    upload.pClusterMins = clusters.mins.data();
    upload.pClusterMaxs = clusters.maxs.data();
    upload.pClusterFlags = clusters.flags.data();
    // No world light faces and no PVS rows: the editor uploads its own geometry, and without a
    // PVS the renderer treats every cluster as visible from every other one, which is what a
    // map without compiled vis needs. The cluster table alone is what the light lists read.
    upload.clusterGeneration = ++m_clusterGeneration;

    const QrResult result = qrUploadWorldLights(m_instance, &upload);

    if (result != QR_SUCCESS)
    {
        PrintMessage(("world clusters: " + std::string(qrGetResultDescription(result))).c_str(),
                     this);
        return false;
    }

    size_t resolved = 0;

    for (const EditorLight &light : m_scene.lights)
    {
        if (light.cluster != QR_CLUSTER_LIGHT_NO_CLUSTER)
        {
            resolved++;
        }
    }

    PrintMessage(("world clusters: " + std::to_string(upload.numClusters) +
                  " clusters, lights resolved " + std::to_string(resolved) + "/" +
                  std::to_string(m_scene.lights.size()))
                     .c_str(),
                 this);

    size_t unclustered = 0;
    size_t distinct = 0;
    uint32_t highest = 0;
    std::vector<uint8_t> seen(upload.numClusters, 0);

    for (const QrVertex &vertex : m_scene.vertices)
    {
        if (vertex.cluster == 0)
        {
            unclustered++;
            continue;
        }

        if (vertex.cluster < upload.numClusters && seen[vertex.cluster] == 0)
        {
            seen[vertex.cluster] = 1;
            distinct++;
        }

        highest = std::max(highest, vertex.cluster);
    }

    PrintMessage(("surface clusters: " + std::to_string(m_scene.vertices.size() - unclustered) +
                  "/" + std::to_string(m_scene.vertices.size()) + " vertices clustered, " +
                  std::to_string(distinct) + " distinct, max " + std::to_string(highest))
                     .c_str(),
                 this);

    return true;
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

    // A material edit is applied by re-uploading the static scene, which starts its own
    // command buffer and waits for the device -- so it has to run before the frame opens.
    // Waiting for the control to stop being held keeps a slider drag from rebuilding per frame.
    if (m_materialsChanged && !ImGui::IsAnyItemActive())
    {
        m_materialsChanged = false;

        if (UploadStaticGeometry())
        {
            PrintMessage("materials applied: static scene re-uploaded", this);
        }
    }

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

    // A grab follows the ray rather than the mouse: the camera can move under a still cursor, so
    // the light is re-aimed every frame the drag lasts, the way QRE_UpdateLightDrag runs per frame.
    UpdateLightGrab();

    UploadSun();
    UploadLights();
    UploadClusterLightSources();

    m_ui.BeginFrame(deltaSeconds, m_clientWidth, m_clientHeight);
    DrawPanel(deltaSeconds);
    DrawSelectionOverlay();
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
    draw.fovYRadians = kEditorFovYRadians;
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

    // The sun the viewport shows is the sun the sky and the shafts use, so the
    // disc, the scattering direction and the directional light cannot disagree.
    float toSun[3];
    SunDirection(toSun);

    BuildEditorFrameParams(m_settings, toSun, m_sunIntensity, eye, m_frameParams);
    draw.pSkyParams = &m_frameParams.sky;
    draw.pTonemappingParams = &m_frameParams.tonemap;
    draw.pVolumetricParams = &m_frameParams.volume;
    draw.pLevelFogParams = &m_frameParams.fog;

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

    if (!m_lightStatsLogged && ++m_framesSinceSceneLoad == 120)
    {
        m_lightStatsLogged = true;

        QrClusterLightStats clusterStats = {};

        if (qrGetClusterLightStats(m_instance, &clusterStats) == QR_SUCCESS)
        {
            char message[320];
            std::snprintf(message, sizeof(message),
                          "cluster lights: %u sources, %u grants, %u entries, %u denied, "
                          "%u clusters, %u full, %u tail, %u tailDropped, %u candidates max",
                          clusterStats.sources, clusterStats.grants, clusterStats.listEntries,
                          clusterStats.denied, clusterStats.clusters, clusterStats.fullClusters,
                          clusterStats.tailEntries, clusterStats.tailCandidatesDropped,
                          clusterStats.candidateMax);
            PrintMessage(message, this);
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

            UpdateGizmoDrag(float(x), float(y));
            UpdateLightGrab();
        }

        break;
    }

    case WM_LBUTTONDOWN:
    {
        if (!m_ui.WantsMouse())
        {
            // Three presses share the left button: on an axis arrow the gizmo slides the light
            // along that axis, on the light itself the light flies with the ray, and anywhere
            // else the press is a pick. The narrowest target wins, so the arrows are tested
            // before the marker they grow out of.
            bool handled = !m_looking && BeginGizmoDrag(m_ui.MouseX(), m_ui.MouseY());

            if (!handled)
            {
                handled = BeginLightGrab();
            }

            if (!handled)
            {
                Pick();
            }
        }

        break;
    }

    case WM_LBUTTONUP:
    {
        FinishGizmoDrag(true);
        FinishLightGrab(true);
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
        else if (wParam == VK_ESCAPE)
        {
            // The game's cancel: a drag in flight goes back to where it started instead of
            // committing a position the author pulled it to by accident.
            FinishGizmoDrag(false);
            FinishLightGrab(false);
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

        // A drag cannot survive the focus: the mouse-up that would commit it is delivered to
        // another window, so the light would stay glued to the last position the cursor had.
        FinishGizmoDrag(false);
        FinishLightGrab(false);
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

void EditorApp::SunDirection(float direction[3]) const
{
    const float elevation = m_sunElevation * 0.01745329252f;
    const float azimuth = m_sunAzimuth * 0.01745329252f;
    const float cosElevation = std::cos(elevation);

    direction[0] = cosElevation * std::cos(azimuth);
    direction[1] = cosElevation * std::sin(azimuth);
    direction[2] = std::sin(elevation);
}

void EditorApp::UploadSun()
{
    float toSun[3];
    SunDirection(toSun);

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

void EditorApp::UploadLight(const EditorLight &light, uint64_t uniqueID, QrResult &lastResult)
{
    // RT_LIGHT_Emit uploads a spot light when the entry's cone is on and its axis is not
    // zero, and a spherical one otherwise.
    const bool isSpot = light.spot && (light.direction[0] * light.direction[0] +
                                       light.direction[1] * light.direction[1] +
                                       light.direction[2] * light.direction[2]) > 0.0f;

    if (isSpot)
    {
        QrSpotLightUploadInfo info = {};
        info.uniqueID = uniqueID;
        info.color = { light.color[0], light.color[1], light.color[2] };
        info.position = { light.position[0], light.position[1], light.position[2] };
        info.direction = { light.direction[0], light.direction[1], light.direction[2] };
        info.radius = light.radius;
        info.angleInner = light.angleInner * 0.01745329252f;
        info.angleOuter = light.angleOuter * 0.01745329252f;

        lastResult = qrUploadSpotLight(m_instance, &info);
    }
    else
    {
        QrSphericalLightUploadInfo info = {};
        // RT_UploadAllElights numbers map lights from UINT16_MAX upwards and
        // keeps the editor's world geometry IDs (1..N) out of the way.
        info.uniqueID = uniqueID;
        info.color = { light.color[0], light.color[1], light.color[2] };
        info.position = { light.position[0], light.position[1], light.position[2] };
        info.radius = light.radius;

        lastResult = qrUploadSphericalLight(m_instance, &info);
    }
}

void EditorApp::UploadLights()
{
    QrResult lastResult = QR_SUCCESS;

    for (size_t index = 0; index < m_scene.lights.size(); index++)
    {
        UploadLight(m_scene.lights[index], LightUniqueID(index), lastResult);

        if (lastResult != QR_SUCCESS)
        {
            break;
        }
    }

    // The custom lights go in after the map's, like the second block of RT_UploadAllElights.
    for (size_t index = 0; index < m_customLights.size() && lastResult == QR_SUCCESS; index++)
    {
        UploadLight(m_customLights[index], CustomLightUniqueID(index), lastResult);
    }

    if (lastResult != QR_SUCCESS)
    {
        static bool logged = false;

        if (!logged)
        {
            logged = true;
            PrintMessage(("lights: " + std::string(qrGetResultDescription(lastResult))).c_str(),
                         this);
        }
    }
}

void EditorApp::LogLightCluster(const EditorLight &light, uint64_t uniqueID, const char *label)
{
    uint64_t uids[128] = {};
    uint32_t count = 0;

    if (qrGetClusterLightList(m_instance, light.cluster, uids, 128, &count) != QR_SUCCESS)
    {
        PrintMessage((std::string(label) + ": cluster " + std::to_string(light.cluster) +
                      " unreadable")
                         .c_str(),
                     this);
        return;
    }

    // A light its own cluster does not list cannot be shaded there, whatever it was authored to:
    // this is what says so, instead of a dark viewport that looks like a bad radiance value.
    bool listed = false;

    for (uint32_t slot = 0; slot < count; slot++)
    {
        listed = listed || uids[slot] == uniqueID;
    }

    PrintMessage((std::string(label) + ": cluster " + std::to_string(light.cluster) + " lists " +
                  std::to_string(count) + (listed ? " with it" : " without it") + ", radiance " +
                  std::to_string(light.color[0]) + " " + std::to_string(light.color[1]) + " " +
                  std::to_string(light.color[2]) + ", radius " + std::to_string(light.radius))
                     .c_str(),
                 this);
}

void EditorApp::UploadClusterLightSources()
{
    const size_t lightCount = m_scene.lights.size() + m_customLights.size();

    if (m_scene.worldClusters.numLeafs == 0 || lightCount == 0)
    {
        return;
    }

    std::vector<QrClusterLightSource> sources(lightCount);
    size_t index = 0;

    // Both lists are registered the same way: RT_LIGHT_Emit registers a map light with the
    // static reach and no cluster list, and the top-up pass hands it the clusters around its
    // own leaf. The custom lights are the same kind of light once they reach the renderer.
    const auto appendSource = [&](const EditorLight &light, uint64_t uniqueID) {
        sources[index].uniqueID = uniqueID;
        sources[index].origin = { light.position[0], light.position[1], light.position[2] };
        sources[index].cluster = light.cluster;
        sources[index].reach = kLightReachUnits;
        sources[index].radius = 0.0f;
        sources[index].clusterCount = 0;
        sources[index].pClusters = nullptr;
        sources[index].power = 0.0f;
        index++;
    };

    for (size_t mapIndex = 0; mapIndex < m_scene.lights.size(); mapIndex++)
    {
        appendSource(m_scene.lights[mapIndex], LightUniqueID(mapIndex));
    }

    for (size_t customIndex = 0; customIndex < m_customLights.size(); customIndex++)
    {
        appendSource(m_customLights[customIndex], CustomLightUniqueID(customIndex));
    }

    QrClusterLightSourcesUploadInfo info = {};
    info.numLights = uint32_t(sources.size());
    info.pLights = sources.data();
    info.topUpReach = kLightTopUpReachUnits;
    info.allowIncremental = 1;
    info.allowOverflow = 1;
    info.validate = 0;

    const QrResult result = qrUploadClusterLightSources(m_instance, &info);

    if (result != QR_SUCCESS)
    {
        static bool logged = false;

        if (!logged)
        {
            logged = true;
            PrintMessage(("cluster light sources: " + std::string(qrGetResultDescription(result)))
                             .c_str(),
                         this);
        }
    }
}

void EditorApp::ViewBasis(float side[3], float up[3], float forward[3]) const
{
    // The same basis LookAt builds the view matrix from, so a ray cast here and a vertex
    // projected with the same basis agree with what the frame draws.
    const float cosPitch = std::cos(m_cameraPitch);

    forward[0] = cosPitch * std::cos(m_cameraYaw);
    forward[1] = cosPitch * std::sin(m_cameraYaw);
    forward[2] = std::sin(m_cameraPitch);

    const float worldUp[3] = { 0.0f, 0.0f, 1.0f };

    side[0] = forward[1] * worldUp[2] - forward[2] * worldUp[1];
    side[1] = forward[2] * worldUp[0] - forward[0] * worldUp[2];
    side[2] = forward[0] * worldUp[1] - forward[1] * worldUp[0];

    const float sideLength = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);

    if (sideLength > 1e-6f)
    {
        side[0] /= sideLength;
        side[1] /= sideLength;
        side[2] /= sideLength;
    }
    else
    {
        side[0] = 1.0f;
        side[1] = 0.0f;
        side[2] = 0.0f;
    }

    up[0] = side[1] * forward[2] - side[2] * forward[1];
    up[1] = side[2] * forward[0] - side[0] * forward[2];
    up[2] = side[0] * forward[1] - side[1] * forward[0];
}

int32_t EditorApp::PickSurface(const float origin[3], const float direction[3], int32_t &outTriangle,
                               float &outDistance) const
{
    // Every triangle of the scene is tested rather than walking the BSP: a click happens once
    // per input event, and the surfaces already carry their own index lists.
    int32_t bestSurface = -1;
    int32_t bestTriangle = -1;
    float bestDistance = 0.0f;

    for (size_t s = 0; s < m_scene.surfaces.size(); s++)
    {
        const EditorSurface &surface = m_scene.surfaces[s];

        for (size_t i = 0; i + 2 < surface.indices.size(); i += 3)
        {
            const uint32_t ia = surface.indices[i + 0];
            const uint32_t ib = surface.indices[i + 1];
            const uint32_t ic = surface.indices[i + 2];

            if (ia >= surface.vertices.size() || ib >= surface.vertices.size() ||
                ic >= surface.vertices.size())
            {
                continue;
            }

            float distance = 0.0f;

            if (!RayTriangle(origin, direction, surface.vertices[ia].position,
                             surface.vertices[ib].position, surface.vertices[ic].position, distance))
            {
                continue;
            }

            if (bestSurface < 0 || distance < bestDistance)
            {
                bestSurface = int32_t(s);
                bestTriangle = int32_t(i / 3);
                bestDistance = distance;
            }
        }
    }

    outTriangle = bestTriangle;
    outDistance = bestDistance;
    return bestSurface;
}

namespace
{
// One light of either list, so the pick walks the map's lights and the editor's own in the
// same pass and reports which list the winner came from.
struct EditorLightRef
{
    const EditorLight *light = nullptr;
    bool custom = false;
    int32_t index = -1;
};

void CollectEditorLights(const std::vector<EditorLight> &mapLights,
                         const std::vector<EditorLight> &customLights,
                         std::vector<EditorLightRef> &outLights)
{
    outLights.clear();
    outLights.reserve(mapLights.size() + customLights.size());

    for (size_t index = 0; index < mapLights.size(); index++)
    {
        outLights.push_back({ &mapLights[index], false, int32_t(index) });
    }

    for (size_t index = 0; index < customLights.size(); index++)
    {
        outLights.push_back({ &customLights[index], true, int32_t(index) });
    }
}
} // namespace

int32_t EditorApp::PickLight(const float origin[3], const float direction[3], float &outDistance,
                             bool &outCustom) const
{
    // QRE_LightUnderCrosshair accepts the light nearest the crosshair ray rather than one exactly
    // under it, because a light is drawn as a small gizmo, not as a surface.
    int32_t bestLight = -1;
    bool bestCustom = false;
    float bestDistance = 0.0f;

    std::vector<EditorLightRef> lights;
    CollectEditorLights(m_scene.lights, m_customLights, lights);

    for (const EditorLightRef &candidate : lights)
    {
        const EditorLight &light = *candidate.light;
        const float toLight[3] = {
            light.position[0] - origin[0],
            light.position[1] - origin[1],
            light.position[2] - origin[2],
        };

        const float along = toLight[0] * direction[0] + toLight[1] * direction[1] +
                            toLight[2] * direction[2];

        if (along <= 0.0f)
        {
            continue;
        }

        const float perpendicular[3] = {
            toLight[0] - direction[0] * along,
            toLight[1] - direction[1] * along,
            toLight[2] - direction[2] * along,
        };

        const float distanceSquared = perpendicular[0] * perpendicular[0] +
                                      perpendicular[1] * perpendicular[1] +
                                      perpendicular[2] * perpendicular[2];

        if (distanceSquared > kLightPickRadiusUnits * kLightPickRadiusUnits)
        {
            continue;
        }

        if (bestLight < 0 || along < bestDistance)
        {
            bestLight = candidate.index;
            bestCustom = candidate.custom;
            bestDistance = along;
        }
    }

    outDistance = bestDistance;
    outCustom = bestCustom;
    return bestLight;
}

void EditorApp::ClearSelection()
{
    m_selectedSurface = -1;
    m_selectedTriangle = -1;
    m_selectedLight = -1;
    m_selectedCustomLight = -1;
    m_hasPickPoint = false;
    m_pickCluster = 0;
}

EditorLight *EditorApp::SelectedLight()
{
    if (m_selectedLight >= 0 && size_t(m_selectedLight) < m_scene.lights.size())
    {
        return &m_scene.lights[size_t(m_selectedLight)];
    }

    if (m_selectedCustomLight >= 0 && size_t(m_selectedCustomLight) < m_customLights.size())
    {
        return &m_customLights[size_t(m_selectedCustomLight)];
    }

    return nullptr;
}

std::string EditorApp::SelectedLightLabel() const
{
    if (m_selectedLight >= 0 && size_t(m_selectedLight) < m_scene.lights.size())
    {
        return "light " + std::to_string(m_selectedLight);
    }

    if (m_selectedCustomLight >= 0 && size_t(m_selectedCustomLight) < m_customLights.size())
    {
        return "custom " + std::to_string(m_selectedCustomLight);
    }

    return "no light";
}

uint64_t EditorApp::SelectedLightUniqueID() const
{
    if (m_selectedLight >= 0 && size_t(m_selectedLight) < m_scene.lights.size())
    {
        return LightUniqueID(size_t(m_selectedLight));
    }

    if (m_selectedCustomLight >= 0 && size_t(m_selectedCustomLight) < m_customLights.size())
    {
        return CustomLightUniqueID(size_t(m_selectedCustomLight));
    }

    return 0;
}

// A copy the author has to find again: the game editor steps a Duplicate 32 units up, which is
// just enough to see the second light next to the first.
constexpr float kDuplicateStepUnits = 32.0f;

// Stands a light off the surface it was cloned onto, so it does not sit inside the face.
constexpr float kCloneOffsetUnits = 8.0f;

void EditorApp::AddCustomLight(const EditorLight *source, LightSpawn spawn)
{
    EditorLight light;

    if (source != nullptr)
    {
        light = *source;
        // The copy is a light of its own: it keeps the authored colour, cone and style but gets
        // its own position slot, so the two can be aimed separately.
        light.className.clear();
    }
    else
    {
        SetEditorLightIntensity(light, kEditorNewLightIntensity);
        light.radius = kEditorNewLightRadiusUnits;
    }

    float ray[3] = {};
    float rayLength = 0.0f;

    if (spawn == LightSpawn::Clone && m_hasPickPoint)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            ray[axis] = m_pickPoint[axis] - m_cameraPosition[axis];
        }

        rayLength = std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
    }

    if (rayLength > 1e-4f)
    {
        // Clone lands on what the author looks at, aimed the same way as Place at pick: a copy
        // that kept the original's aim would light the space behind the picked face.
        for (int axis = 0; axis < 3; axis++)
        {
            ray[axis] /= rayLength;
            light.position[axis] = m_pickPoint[axis] - ray[axis] * kCloneOffsetUnits;
            light.direction[axis] = ray[axis];
        }
    }
    else if (spawn == LightSpawn::Duplicate && source != nullptr)
    {
        // A duplicate is not aimed at anything new, so it keeps the original's direction and
        // only steps out of it.
        light.position[2] += kDuplicateStepUnits;
    }
    else
    {
        // A fresh light lands where the camera looks: inside the map and in front of the
        // author, rather than at the world origin, where there is usually a wall.
        float side[3] = {};
        float up[3] = {};
        float forward[3] = {};
        ViewBasis(side, up, forward);

        constexpr float kSpawnDistanceUnits = 96.0f;

        for (int axis = 0; axis < 3; axis++)
        {
            light.position[axis] = m_cameraPosition[axis] + forward[axis] * kSpawnDistanceUnits;
        }
    }

    light.cluster = ResolveEditorLightCluster(m_scene, light.position);

    if (light.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER && m_pickCluster != 0)
    {
        light.cluster = m_pickCluster;
    }

    m_customLights.push_back(light);
    SelectLight(int32_t(m_customLights.size()) - 1, true);

    PrintMessage((SelectedLightLabel() + ": added at " + std::to_string(int32_t(light.position[0])) +
                  " " + std::to_string(int32_t(light.position[1])) + " " +
                  std::to_string(int32_t(light.position[2])) + " cluster " +
                  std::to_string(light.cluster))
                     .c_str(),
                 this);
}

void EditorApp::DuplicateSelectedLight()
{
    const EditorLight *selected = SelectedLight();

    if (selected == nullptr)
    {
        PrintMessage("duplicate: pick a light first", this);
        return;
    }

    AddCustomLight(selected, LightSpawn::Duplicate);
    PrintMessage((SelectedLightLabel() + ": duplicated").c_str(), this);
}

void EditorApp::CloneSelectedLight()
{
    const EditorLight *selected = SelectedLight();

    if (selected == nullptr)
    {
        PrintMessage("clone: pick a light first", this);
        return;
    }

    AddCustomLight(selected, LightSpawn::Clone);
    PrintMessage((SelectedLightLabel() + ": cloned").c_str(), this);
}

void EditorApp::RemoveSelectedCustomLight()
{
    if (m_selectedCustomLight < 0 || size_t(m_selectedCustomLight) >= m_customLights.size())
    {
        PrintMessage("remove: a map light belongs to the level, pick a custom one", this);
        return;
    }

    const std::string label = SelectedLightLabel();
    const size_t index = size_t(m_selectedCustomLight);

    m_customLights.erase(m_customLights.begin() + std::ptrdiff_t(index));
    // The lights past the removed one shift down, so an index that named one of them would now
    // silently name its neighbour: drop the selection rather than point at a different light.
    ClearSelection();

    PrintMessage((label + ": removed").c_str(), this);
}

void EditorApp::SelectLight(int32_t index, bool custom)
{
    const std::vector<EditorLight> &lights = custom ? m_customLights : m_scene.lights;

    if (index < 0 || size_t(index) >= lights.size())
    {
        return;
    }

    m_selectedLight = custom ? -1 : index;
    m_selectedCustomLight = custom ? index : -1;
    m_selectedSurface = -1;
    m_selectedTriangle = -1;

    const EditorLight &light = lights[size_t(index)];

    m_hasPickPoint = true;
    m_pickCluster = light.cluster;

    for (int axis = 0; axis < 3; axis++)
    {
        m_pickPoint[axis] = light.position[axis];
    }
}

void EditorApp::Pick()
{
    if (m_scene.surfaces.empty() && m_scene.lights.empty() && m_customLights.empty())
    {
        return;
    }

    float direction[3] = {};

    if (!CursorDirection(direction))
    {
        return;
    }

    int32_t triangle = -1;
    float surfaceDistance = 0.0f;
    const int32_t surface = PickSurface(m_cameraPosition, direction, triangle, surfaceDistance);
    float lightDistance = 0.0f;
    bool lightCustom = false;
    const int32_t light = PickLight(m_cameraPosition, direction, lightDistance, lightCustom);

    // A light in front of the surface hides it, exactly like a gizmo drawn over the wall behind.
    const bool lightWins = light >= 0 && (surface < 0 || lightDistance < surfaceDistance);

    m_selectedLight = (lightWins && !lightCustom) ? light : -1;
    m_selectedCustomLight = (lightWins && lightCustom) ? light : -1;
    m_selectedSurface = (!lightWins && surface >= 0) ? surface : -1;
    m_selectedTriangle = (!lightWins && surface >= 0) ? triangle : -1;
    m_pickCluster = 0;

    if (m_selectedSurface >= 0)
    {
        const EditorSurface &picked = m_scene.surfaces[size_t(m_selectedSurface)];
        const size_t base = size_t(m_selectedTriangle) * 3;

        // Picking a face also selects its material, so the Materials tab follows the
        // surface under the cursor the way the game editor's material mode does.
        const int32_t pickedMaterial = m_materials.IndexOf(picked.textureName);

        if (pickedMaterial >= 0)
        {
            m_selectedMaterial = pickedMaterial;
        }

        m_hasPickPoint = true;

        for (int axis = 0; axis < 3; axis++)
        {
            m_pickPoint[axis] = 0.0f;
        }

        for (size_t corner = 0; corner < 3 && base + corner < picked.indices.size(); corner++)
        {
            const QrVertex &vertex = picked.vertices[picked.indices[base + corner]];

            for (int axis = 0; axis < 3; axis++)
            {
                m_pickPoint[axis] += vertex.position[axis] / 3.0f;
            }

            m_pickCluster = vertex.cluster;
        }
    }
    else
    {
        if (EditorLight *picked = SelectedLight())
        {
            m_hasPickPoint = true;
            m_pickCluster = picked->cluster;

            for (int axis = 0; axis < 3; axis++)
            {
                m_pickPoint[axis] = picked->position[axis];
            }
        }
        else
        {
            m_hasPickPoint = false;
        }
    }

    if (m_selectedSurface < 0 && SelectedLight() == nullptr)
    {
        ClearSelection();
        PrintMessage("pick: nothing under the cursor", this);
        return;
    }

    if (m_selectedSurface < 0)
    {
        PrintMessage(("pick: " + SelectedLightLabel()).c_str(), this);
    }
    else
    {
        const EditorSurface &picked = m_scene.surfaces[size_t(m_selectedSurface)];

        PrintMessage(("pick: surface " + std::to_string(m_selectedSurface) + " triangle " +
                      std::to_string(m_selectedTriangle) + " texture " + picked.textureName +
                      " cluster " + std::to_string(m_pickCluster))
                         .c_str(),
                     this);
    }
}

void EditorApp::DrawSelectionOverlay()
{
    if (!m_hasPickPoint || m_clientWidth == 0 || m_clientHeight == 0)
    {
        return;
    }

    ImDrawList *drawList = ImGui::GetBackgroundDrawList();

    if (drawList == nullptr)
    {
        return;
    }

    const auto project = [this](const float point[3], float &outX, float &outY) -> bool {
        return ProjectPoint(point, outX, outY);
    };

    const ImU32 accent = IM_COL32(255, 170, 40, 255);
    const ImU32 marker = IM_COL32(90, 200, 255, 255);

    if (m_selectedSurface >= 0 && m_selectedSurface < int32_t(m_scene.surfaces.size()))
    {
        const EditorSurface &surface = m_scene.surfaces[size_t(m_selectedSurface)];
        const size_t base = size_t(m_selectedTriangle) * 3;

        if (base + 2 < surface.indices.size())
        {
            float screenX[3] = {};
            float screenY[3] = {};
            bool visible = true;

            for (size_t corner = 0; corner < 3; corner++)
            {
                const uint32_t index = surface.indices[base + corner];

                if (index >= surface.vertices.size() ||
                    !project(surface.vertices[index].position, screenX[corner], screenY[corner]))
                {
                    visible = false;
                    break;
                }
            }

            if (visible)
            {
                for (int corner = 0; corner < 3; corner++)
                {
                    const int next = (corner + 1) % 3;

                    drawList->AddLine(ImVec2(screenX[corner], screenY[corner]),
                                      ImVec2(screenX[next], screenY[next]), accent, 2.0f);
                }
            }
        }
    }

    const EditorLight *selected = SelectedLight();

    if (selected != nullptr)
    {
        const ImU32 axisColors[3] = {
            IM_COL32(255, 64, 64, 255),
            IM_COL32(64, 255, 64, 255),
            IM_COL32(64, 128, 255, 255),
        };

        float originX = 0.0f;
        float originY = 0.0f;

        if (project(selected->position, originX, originY))
        {
            // The game draws a grab-able arrow per axis on a custom light and only a marker on a
            // map light, because a map light is moved by grabbing it rather than by its gizmo.
            const bool draggable = m_selectedCustomLight >= 0;

            for (int axis = 0; axis < 3; axis++)
            {
                const float length = draggable ? kGizmoLengthUnits : 12.0f;
                const ImU32 colour = draggable ? axisColors[axis] : marker;

                float tip[3] = { selected->position[0], selected->position[1], selected->position[2] };
                tip[axis] += length;

                float tipX = 0.0f;
                float tipY = 0.0f;

                if (!project(tip, tipX, tipY))
                {
                    continue;
                }

                drawList->AddLine(ImVec2(originX, originY), ImVec2(tipX, tipY), colour,
                                  draggable ? 4.0f : 2.0f);

                if (!draggable)
                {
                    continue;
                }

                // The arrow head spans the next axis in the pair, so it is visible from the
                // side the arrow is seen rather than edge-on.
                float across[3] = { selected->position[0], selected->position[1],
                                    selected->position[2] };
                across[(axis + 1) % 3] += length * 0.2f;

                float back[3] = { tip[0], tip[1], tip[2] };
                back[axis] -= length * kGizmoHeadLength;

                for (int side = 0; side < 2; side++)
                {
                    float head[3] = { back[0], back[1], back[2] };

                    for (int component = 0; component < 3; component++)
                    {
                        head[component] += (across[component] - tip[component]) *
                                          (side == 0 ? 1.0f : -1.0f);
                    }

                    float headX = 0.0f;
                    float headY = 0.0f;

                    if (project(head, headX, headY))
                    {
                        drawList->AddLine(ImVec2(tipX, tipY), ImVec2(headX, headY), colour, 4.0f);
                    }
                }
            }

            if (draggable)
            {
                // The axis tip rectangles go to the log so a scripted drag knows where to press
                // without reading the viewport.
                std::string axes;

                for (int axis = 0; axis < 3; axis++)
                {
                    float tip[3] = { selected->position[0], selected->position[1],
                                     selected->position[2] };
                    tip[axis] += kGizmoLengthUnits;

                    float tipX = 0.0f;
                    float tipY = 0.0f;

                    if (!project(tip, tipX, tipY))
                    {
                        continue;
                    }

                    axes += " " + std::to_string(axis) + " " + std::to_string(int32_t(originX)) + "," +
                            std::to_string(int32_t(originY)) + " " +
                            std::to_string(int32_t(tipX)) + "," + std::to_string(int32_t(tipY));
                }

                const uint64_t layout = std::hash<std::string>{}(axes);

                // A drag re-projects the arrows every frame it moves; logging them then would
                // bury the one line that explains where the press has to land.
                if (!m_gizmoDragging && !m_grabDragging && m_loggedGizmoLayout != layout)
                {
                    m_loggedGizmoLayout = layout;
                    PrintMessage(("gizmo axes:" + axes).c_str(), this);
                }
            }
        }
    }
}

bool EditorApp::ProjectPoint(const float point[3], float &outX, float &outY) const
{
    if (m_clientWidth == 0 || m_clientHeight == 0)
    {
        return false;
    }

    float side[3] = {};
    float up[3] = {};
    float forward[3] = {};
    ViewBasis(side, up, forward);

    const float aspect = float(m_clientWidth) / float(m_clientHeight);
    const float tanHalfFov = std::tan(0.5f * kEditorFovYRadians);
    const float relative[3] = {
        point[0] - m_cameraPosition[0],
        point[1] - m_cameraPosition[1],
        point[2] - m_cameraPosition[2],
    };

    const float depth = relative[0] * forward[0] + relative[1] * forward[1] + relative[2] * forward[2];

    if (depth <= 1e-3f)
    {
        return false;
    }

    const float horizontal = relative[0] * side[0] + relative[1] * side[1] + relative[2] * side[2];
    const float vertical = relative[0] * up[0] + relative[1] * up[1] + relative[2] * up[2];

    const float ndcX = horizontal / (depth * tanHalfFov * aspect);
    const float ndcY = vertical / (depth * tanHalfFov);

    outX = (ndcX * 0.5f + 0.5f) * float(m_clientWidth);
    outY = (1.0f - (ndcY * 0.5f + 0.5f)) * float(m_clientHeight);
    return true;
}

bool EditorApp::BeginGizmoDrag(float cursorX, float cursorY)
{
    if (m_selectedCustomLight < 0 || m_selectedCustomLight >= int32_t(m_customLights.size()))
    {
        return false;
    }

    const EditorLight &light = m_customLights[size_t(m_selectedCustomLight)];
    float originX = 0.0f;
    float originY = 0.0f;

    if (!ProjectPoint(light.position, originX, originY))
    {
        return false;
    }

    // The origin belongs to no axis, so a press on the light itself stays a pick - which is what
    // re-selects a light the cursor is already near.
    const float fromOriginX = cursorX - originX;
    const float fromOriginY = cursorY - originY;

    if (std::sqrt(fromOriginX * fromOriginX + fromOriginY * fromOriginY) < kGizmoGrabPixels)
    {
        return false;
    }

    int32_t bestAxis = -1;
    float bestDistance = kGizmoGrabPixels;
    float bestDirection[2] = {};
    float bestPixelsPerUnit = 0.0f;

    for (int32_t axis = 0; axis < 3; axis++)
    {
        float tip[3] = { light.position[0], light.position[1], light.position[2] };
        tip[axis] += kGizmoLengthUnits;

        float tipX = 0.0f;
        float tipY = 0.0f;

        if (!ProjectPoint(tip, tipX, tipY))
        {
            continue;
        }

        const float dx = tipX - originX;
        const float dy = tipY - originY;
        const float pixelLength = std::sqrt(dx * dx + dy * dy);

        // An axis pointing at the camera projects to a few pixels: it cannot be grabbed, and it
        // cannot drive a drag either, so it is skipped the way QRE_GizmoAxisScreen rejects one
        // shorter than a pixel.
        if (pixelLength < kGizmoGrabPixels)
        {
            continue;
        }

        const float distance = DistanceToSegment(cursorX, cursorY, originX, originY, tipX, tipY);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestAxis = axis;
            bestDirection[0] = dx / pixelLength;
            bestDirection[1] = dy / pixelLength;
            bestPixelsPerUnit = pixelLength / kGizmoLengthUnits;
        }
    }

    if (bestAxis < 0)
    {
        // A press this far from every arrow is a pick, but the miss is logged so a scripted drag
        // can tell "I missed the gizmo" from "the gizmo is not responding".
        std::string distances;

        for (int32_t axis = 0; axis < 3; axis++)
        {
            float tip[3] = { light.position[0], light.position[1], light.position[2] };
            tip[axis] += kGizmoLengthUnits;

            float tipX = 0.0f;
            float tipY = 0.0f;

            if (!ProjectPoint(tip, tipX, tipY))
            {
                distances += " " + std::to_string(axis) + ":offscreen";
                continue;
            }

            distances += " " + std::to_string(axis) + ":" +
                         std::to_string(int32_t(DistanceToSegment(cursorX, cursorY, originX, originY,
                                                                 tipX, tipY)));
        }

        PrintMessage(("gizmo miss: cursor " + std::to_string(int32_t(cursorX)) + "," +
                      std::to_string(int32_t(cursorY)) + " origin " +
                      std::to_string(int32_t(originX)) + "," + std::to_string(int32_t(originY)) +
                      " distance" + distances)
                         .c_str(),
                     this);
        return false;
    }

    m_gizmoDragging = true;
    m_gizmoAxis = bestAxis;
    m_gizmoStart[0] = cursorX;
    m_gizmoStart[1] = cursorY;
    m_gizmoDirection[0] = bestDirection[0];
    m_gizmoDirection[1] = bestDirection[1];
    m_gizmoPixelsPerUnit = bestPixelsPerUnit;

    for (int axis = 0; axis < 3; axis++)
    {
        m_gizmoOrigin[axis] = light.position[axis];
    }

    PrintMessage(("gizmo: grab custom " + std::to_string(m_selectedCustomLight) + " axis " +
                  std::to_string(bestAxis) + " at " + std::to_string(int32_t(originX)) + "," +
                  std::to_string(int32_t(originY)) + " from " +
                  std::to_string(int32_t(m_gizmoOrigin[0])) + " " +
                  std::to_string(int32_t(m_gizmoOrigin[1])) + " " +
                  std::to_string(int32_t(m_gizmoOrigin[2])))
                     .c_str(),
                 this);
    return true;
}

void EditorApp::UpdateGizmoDrag(float cursorX, float cursorY)
{
    if (!m_gizmoDragging || m_selectedCustomLight < 0 ||
        m_selectedCustomLight >= int32_t(m_customLights.size()) || m_gizmoPixelsPerUnit <= 0.0f)
    {
        return;
    }

    EditorLight &light = m_customLights[size_t(m_selectedCustomLight)];

    // How far the cursor travelled along the axis' frozen screen direction, in world units.
    const float travelled = (cursorX - m_gizmoStart[0]) * m_gizmoDirection[0] +
                            (cursorY - m_gizmoStart[1]) * m_gizmoDirection[1];

    // Whole units, like the game's QRE_CustomGizmoMove: the light stays on the integer grid the
    // map's own entities sit on while the cursor moves by fractions of one arrow length.
    const float delta = std::round(travelled / m_gizmoPixelsPerUnit);

    for (int axis = 0; axis < 3; axis++)
    {
        light.position[axis] = m_gizmoOrigin[axis];
    }

    light.position[m_gizmoAxis] += delta;
    light.cluster = ResolveEditorLightCluster(m_scene, light.position);
}

void EditorApp::FinishGizmoDrag(bool kept)
{
    if (!m_gizmoDragging)
    {
        return;
    }

    m_gizmoDragging = false;

    if (m_selectedCustomLight < 0 || m_selectedCustomLight >= int32_t(m_customLights.size()))
    {
        return;
    }

    EditorLight &light = m_customLights[size_t(m_selectedCustomLight)];
    const std::string label = "custom " + std::to_string(m_selectedCustomLight);

    if (!kept)
    {
        // Escape or a lost button puts the light back where the press found it, the way
        // QRE_CustomGizmoCancel restores the origin.
        for (int axis = 0; axis < 3; axis++)
        {
            light.position[axis] = m_gizmoOrigin[axis];
        }

        light.cluster = ResolveEditorLightCluster(m_scene, light.position);
        PrintMessage((label + ": gizmo cancelled").c_str(), this);
        LogLightCluster(light, CustomLightUniqueID(size_t(m_selectedCustomLight)), label.c_str());
        return;
    }

    PrintMessage((label + ": gizmo moved to " + std::to_string(int32_t(light.position[0])) + " " +
                  std::to_string(int32_t(light.position[1])) + " " +
                  std::to_string(int32_t(light.position[2])) + " axis " +
                  std::to_string(m_gizmoAxis) + " cluster " + std::to_string(light.cluster))
                     .c_str(),
                 this);
    LogLightCluster(light, CustomLightUniqueID(size_t(m_selectedCustomLight)), label.c_str());
}

bool EditorApp::CursorDirection(float outDirection[3]) const
{
    if (m_clientWidth == 0 || m_clientHeight == 0)
    {
        return false;
    }

    float side[3] = {};
    float up[3] = {};
    float forward[3] = {};
    ViewBasis(side, up, forward);

    // A click while the mouse is captured comes from the crosshair, so it uses the centre of the
    // viewport; otherwise it comes from the cursor.
    float cursorX = float(m_clientWidth) * 0.5f;
    float cursorY = float(m_clientHeight) * 0.5f;

    if (!m_looking)
    {
        cursorX = m_ui.MouseX();
        cursorY = m_ui.MouseY();
    }

    const float aspect = float(m_clientWidth) / float(m_clientHeight);
    const float tanHalfFov = std::tan(0.5f * kEditorFovYRadians);
    const float ndcX = (cursorX / float(m_clientWidth)) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (cursorY / float(m_clientHeight)) * 2.0f;

    outDirection[0] = forward[0] + side[0] * (ndcX * tanHalfFov * aspect) + up[0] * (ndcY * tanHalfFov);
    outDirection[1] = forward[1] + side[1] * (ndcX * tanHalfFov * aspect) + up[1] * (ndcY * tanHalfFov);
    outDirection[2] = forward[2] + side[2] * (ndcX * tanHalfFov * aspect) + up[2] * (ndcY * tanHalfFov);

    const float length = std::sqrt(outDirection[0] * outDirection[0] +
                                   outDirection[1] * outDirection[1] +
                                   outDirection[2] * outDirection[2]);

    if (length <= 1e-6f)
    {
        return false;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        outDirection[axis] /= length;
    }

    return true;
}

bool EditorApp::BeginLightGrab()
{
    float direction[3] = {};

    if (!CursorDirection(direction))
    {
        return false;
    }

    float distance = 0.0f;
    bool custom = false;
    const int32_t index = PickLight(m_cameraPosition, direction, distance, custom);

    if (index < 0)
    {
        return false;
    }

    // Pressing the light is a press on the light, so it becomes the selection before the drag
    // moves it - the panel follows the light the author grabbed, not the one it had.
    SelectLight(index, custom);

    const EditorLight *light = SelectedLight();

    if (light == nullptr)
    {
        return false;
    }

    m_grabDragging = true;
    m_grabMoved = false;
    m_grabCustom = custom;
    m_grabIndex = index;
    m_grabStart[0] = m_ui.MouseX();
    m_grabStart[1] = m_ui.MouseY();

    for (int axis = 0; axis < 3; axis++)
    {
        m_grabOrigin[axis] = light->position[axis];
        m_grabLoggedPosition[axis] = light->position[axis];
    }

    PrintMessage(("grab: " + SelectedLightLabel() + " at " + std::to_string(int32_t(light->position[0])) +
                  " " + std::to_string(int32_t(light->position[1])) + " " +
                  std::to_string(int32_t(light->position[2])))
                     .c_str(),
                 this);
    return true;
}

void EditorApp::UpdateLightGrab()
{
    if (!m_grabDragging)
    {
        return;
    }

    if (!m_grabCustom && (m_grabIndex < 0 || m_grabIndex >= int32_t(m_scene.lights.size())))
    {
        return;
    }

    if (m_grabCustom && (m_grabIndex < 0 || m_grabIndex >= int32_t(m_customLights.size())))
    {
        return;
    }

    // A press that has not travelled yet is still a click, so it selects the light without moving
    // it: the game separates the two because a grab happens on a different button than a pick,
    // and a standalone editor has only the one the author presses.
    if (!m_looking)
    {
        const float travel = std::abs(m_ui.MouseX() - m_grabStart[0]) +
                             std::abs(m_ui.MouseY() - m_grabStart[1]);

        if (travel < kGizmoClickPixels)
        {
            return;
        }
    }

    float direction[3] = {};

    if (!CursorDirection(direction))
    {
        return;
    }

    m_grabMoved = true;

    int32_t triangle = -1;
    float surfaceDistance = 0.0f;
    const int32_t surface = PickSurface(m_cameraPosition, direction, triangle, surfaceDistance);

    float target[3] = {};

    if (surface >= 0)
    {
        // QRE_UpdateLightDrag puts the light 8 units in front of the surface under the ray,
        // along the ray - the same point as pick_impact - 8 * vpn.
        const float distance = surfaceDistance - kGrabSurfaceOffsetUnits;

        for (int axis = 0; axis < 3; axis++)
        {
            target[axis] = m_cameraPosition[axis] + direction[axis] * distance;
        }
    }
    else
    {
        for (int axis = 0; axis < 3; axis++)
        {
            target[axis] = m_cameraPosition[axis] + direction[axis] * kGrabAirDistanceUnits;
        }
    }

    EditorLight *light = SelectedLight();

    if (light == nullptr)
    {
        return;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        light->position[axis] = target[axis];
    }

    light->cluster = ResolveEditorLightCluster(m_scene, light->position);

    // The drag lands on a new position every frame it moves; the log carries the ones that
    // changed, so a scripted drag can read where the light ended up.
    float moved = 0.0f;

    for (int axis = 0; axis < 3; axis++)
    {
        moved = std::max(moved, std::abs(light->position[axis] - m_grabLoggedPosition[axis]));
    }

    if (moved >= 1.0f)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            m_grabLoggedPosition[axis] = light->position[axis];
        }

        PrintMessage((SelectedLightLabel() + ": drag to " + std::to_string(int32_t(light->position[0])) +
                      " " + std::to_string(int32_t(light->position[1])) + " " +
                      std::to_string(int32_t(light->position[2])) + " cluster " +
                      std::to_string(light->cluster))
                         .c_str(),
                     this);
    }
}

void EditorApp::FinishLightGrab(bool kept)
{
    if (!m_grabDragging)
    {
        return;
    }

    m_grabDragging = false;

    EditorLight *light = SelectedLight();

    if (light == nullptr)
    {
        return;
    }

    const std::string label = SelectedLightLabel();

    if (!kept && m_grabMoved)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            light->position[axis] = m_grabOrigin[axis];
        }

        light->cluster = ResolveEditorLightCluster(m_scene, light->position);
        PrintMessage((label + ": drag cancelled").c_str(), this);
        LogLightCluster(*light, SelectedLightUniqueID(), label.c_str());
        return;
    }

    if (!m_grabMoved)
    {
        return;
    }

    PrintMessage((label + ": dropped at " + std::to_string(int32_t(light->position[0])) + " " +
                  std::to_string(int32_t(light->position[1])) + " " +
                  std::to_string(int32_t(light->position[2])) + " cluster " +
                  std::to_string(light->cluster))
                     .c_str(),
                 this);
    LogLightCluster(*light, SelectedLightUniqueID(), label.c_str());
}

void EditorApp::DrawLightList()
{
    const bool hasSelection = SelectedLight() != nullptr;
    ImGui::Text("Map lights     %d", int32_t(m_scene.lights.size()));
    ImGui::Text("Custom lights  %d", int32_t(m_customLights.size()));

    {
        if (!m_hasPickPoint)
        {
            ImGui::TextUnformatted("Nothing picked: a light would land in front of the camera");
        }

        // Button rects are logged below so a scripted session can press them by name.
        std::string rects;

        auto appendRect = [&rects](const char *name) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();

            rects += " ";
            rects += name;
            rects += " " + std::to_string(int32_t(min.x)) + "," + std::to_string(int32_t(min.y)) + " " +
                     std::to_string(int32_t(max.x)) + "," + std::to_string(int32_t(max.y));
        };

        if (ImGui::Button("Add light"))
        {
            AddCustomLight(nullptr, LightSpawn::Camera);
        }

        appendRect("add");

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasSelection);

        if (ImGui::Button("Duplicate"))
        {
            DuplicateSelectedLight();
        }

        appendRect("duplicate");

        ImGui::SameLine();

        if (ImGui::Button("Clone"))
        {
            CloneSelectedLight();
        }

        appendRect("clone");

        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_selectedCustomLight < 0);

        if (ImGui::Button("Remove"))
        {
            RemoveSelectedCustomLight();
        }

        appendRect("remove");

        ImGui::EndDisabled();

        auto appendRowRect = [&appendRect](int32_t index) {
            const std::string name = "row" + std::to_string(index);
            appendRect(name.c_str());
        };

        ImGui::Separator();

        auto drawRow = [this, &appendRowRect](int32_t index, bool custom) {
            const std::vector<EditorLight> &lights = custom ? m_customLights : m_scene.lights;
            const EditorLight &light = lights[size_t(index)];

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            const std::string rowLabel = (custom ? "custom " : "light ") + std::to_string(index);
            const bool selected = custom ? m_selectedCustomLight == index : m_selectedLight == index;

            if (ImGui::Selectable(rowLabel.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
            {
                SelectLight(index, custom);
                PrintMessage((SelectedLightLabel() + ": selected from the list").c_str(), this);
            }

            if (custom && index < 8)
            {
                appendRowRect(index);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%.0f %.0f %.0f", light.position[0], light.position[1], light.position[2]);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", light.radius);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(EditorStyleName(light.style));
            ImGui::TableNextColumn();
            ImGui::Text("%u", light.cluster);
        };

        auto drawTable = [&drawRow](const char *id, const char *childId, float height,
                                    const std::vector<EditorLight> &lights, bool custom) {
            if (!ImGui::BeginChild(childId, ImVec2(0.0f, height), ImGuiChildFlags_Borders))
            {
                ImGui::EndChild();
                return;
            }

            const ImGuiTableFlags tableFlags =
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;

            if (ImGui::BeginTable(id, 5, tableFlags))
            {
                ImGui::TableSetupColumn("Light");
                ImGui::TableSetupColumn("Position");
                ImGui::TableSetupColumn("Radius");
                ImGui::TableSetupColumn("Style");
                ImGui::TableSetupColumn("Cluster");
                ImGui::TableHeadersRow();

                // A map carries hundreds of lights; only the visible rows are submitted.
                ImGuiListClipper clipper;
                clipper.Begin(int32_t(lights.size()));

                while (clipper.Step())
                {
                    for (int32_t index = clipper.DisplayStart; index < clipper.DisplayEnd; index++)
                    {
                        drawRow(index, custom);
                    }
                }

                ImGui::EndTable();
            }

            ImGui::EndChild();
        };

        ImGui::TextUnformatted("Map");
        drawTable("MapLights", "MapLightsChild", 190.0f, m_scene.lights, false);

        ImGui::TextUnformatted("Custom");
        drawTable("CustomLights", "CustomLightsChild", 150.0f, m_customLights, true);

        const uint64_t layout = std::hash<std::string>{}(rects) | 1u;

        if (m_loggedLightListLayout != layout)
        {
            m_loggedLightListLayout = layout;
            PrintMessage(("light list rect:" + rects).c_str(), this);
        }
    }
}

namespace
{
    // Opens a tab and records its rect straight away: ImGui has just submitted the tab's own item,
    // so this is the only place the rect is still readable.
    bool BeginLoggedTab(const char *label, ImGuiTabItemFlags flags, std::string &rects)
    {
        const bool open = ImGui::BeginTabItem(label, nullptr, flags);

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();

        rects += " ";
        rects += label;
        rects += " " + std::to_string(int32_t(min.x)) + "," + std::to_string(int32_t(min.y)) + " " +
                 std::to_string(int32_t(max.x)) + "," + std::to_string(int32_t(max.y));

        return open;
    }
}

void EditorApp::DrawPanel(float deltaSeconds)
{
    if (!m_showPanel)
    {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380.0f, 660.0f), ImGuiCond_FirstUseEver);

    // Tabs keep every group reachable: the panel is a fixed height now that there are
    // more sections than fit on a 720p screen at once.
    if (ImGui::Begin("QuakeRay Editor", &m_showPanel, ImGuiWindowFlags_NoCollapse))
    {
        const float fps = deltaSeconds > 0.0f ? 1.0f / deltaSeconds : 0.0f;
        ImGui::Text("Frame  %.2f ms  (%.0f FPS)", m_frameTimeMs, fps);
        ImGui::Text("Drawable  %ux%u", m_clientWidth, m_clientHeight);

        // Tab rects are logged so a scripted session can click a neighbouring tab instead of
        // guessing where ImGui laid the bar out.
        std::string tabRects;

        if (ImGui::BeginTabBar("EditorTabs"))
        {
            // --tab <name> opens that tab on the first frame only, so later clicks behave as usual.
            auto tabFlags = [this](const char *name) -> ImGuiTabItemFlags {
                if (m_initialTabApplied || m_initialTab != name)
                {
                    return ImGuiTabItemFlags_None;
                }

                m_initialTabApplied = true;
                return ImGuiTabItemFlags_SetSelected;
            };

            if (BeginLoggedTab("Scene", tabFlags("Scene"), tabRects))
            {
                // The selection, light and sun blocks are taller than a 720p window once the
                // panel is fixed at 660, so the tab scrolls and the key hints stay visible.
                const float footerHeight =
                    ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
                ImGui::BeginChild("SceneParams", ImVec2(0.0f, -footerHeight), false);

                ImGui::Separator();
                ImGui::TextUnformatted("Scene");
                ImGui::Text("Vertices  %zu", m_scene.vertices.size());
                ImGui::Text("Triangles %zu", m_scene.indices.size() / 3);
                ImGui::Text("Surfaces  %zu  Textures %zu", m_scene.surfaces.size(),
                            m_scene.textures.size());
                ImGui::Text("Lights    %zu", m_scene.lights.size());

                QrClusterLightStats clusterStats = {};

                if (qrGetClusterLightStats(m_instance, &clusterStats) == QR_SUCCESS)
                {
                    ImGui::Text("Clusters  %u  sources %u", clusterStats.clusters, clusterStats.sources);
                    ImGui::Text("Grants    %u  entries %u  denied %u", clusterStats.grants,
                                clusterStats.listEntries, clusterStats.denied);
                }

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
                ImGui::TextUnformatted("Selection");

                if (m_selectedSurface >= 0 && m_selectedSurface < int32_t(m_scene.surfaces.size()))
                {
                    const EditorSurface &surface = m_scene.surfaces[size_t(m_selectedSurface)];

                    ImGui::Text("Surface   %d  triangle %d", m_selectedSurface, m_selectedTriangle);
                    ImGui::Text("Texture   %s", surface.textureName.c_str());
                    ImGui::Text("Kind      %s", SurfaceKindName(surface.kind));
                    ImGui::Text("Cluster   %u", m_pickCluster);
                }
                else if (EditorLight *selected = SelectedLight())
                {
                    EditorLight &light = *selected;
                    const std::string selectionLabel = SelectedLightLabel();
                    const uint64_t selectionID = SelectedLightUniqueID();

                    ImGui::Text("%s  %s", selectionLabel.c_str(),
                                light.className.empty() ? "light" : light.className.c_str());
                    ImGui::Text("Position  %.1f %.1f %.1f", light.position[0], light.position[1],
                                light.position[2]);
                    ImGui::Text("Radiance  %.0f %.0f %.0f", light.color[0], light.color[1], light.color[2]);
                    ImGui::Text("Cluster   %s (%u)",
                                light.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER ? "none" : "resolved",
                                light.cluster);

                    // Authoring mirrors the map entity keys the game honours: `light` (which becomes
                    // the radiance through kEditorLightIntensityScale) and rt_elight_radius.
                    //
                    // Every widget's item rect is collected here so a scripted session can click a
                    // field without reading pixels; see `panel rect` below.
                    std::string rects;

                    auto appendRect = [&rects](const char *name) {
                        const ImVec2 min = ImGui::GetItemRectMin();
                        const ImVec2 max = ImGui::GetItemRectMax();

                        rects += " ";
                        rects += name;
                        rects += " " + std::to_string(int32_t(min.x)) + "," + std::to_string(int32_t(min.y)) +
                                 " " + std::to_string(int32_t(max.x)) + "," +
                                 std::to_string(int32_t(max.y));
                    };

                    // Style is the map's `style` key. The preview keeps full brightness because a
                    // standalone editor has no lightstyle feed to modulate it, so this is authored
                    // metadata: it names the style the light will animate with once saved.
                    // A map light also offers NONE, which is what the game's Entity tab shows.
                    // The popup's item rects are logged while it is open, so a scripted session can
                    // pick a style without reading pixels.
                    std::string styleItems;

                    if (ImGui::BeginCombo("Light style##light", EditorStyleName(light.style)))
                    {
                        for (int32_t style = m_selectedLight >= 0 ? -1 : 0; style < kEditorStyleCount;
                             style++)
                        {
                            const bool current = style == light.style;

                            if (ImGui::Selectable(EditorStyleName(style), current))
                            {
                                light.style = style;
                                PrintMessage((selectionLabel + ": style " + std::to_string(style) + " " +
                                              EditorStyleName(style))
                                                 .c_str(),
                                             this);
                            }

                            const ImVec2 itemMin = ImGui::GetItemRectMin();
                            const ImVec2 itemMax = ImGui::GetItemRectMax();

                            styleItems += " " + std::to_string(style) + " " +
                                          std::to_string(int32_t(itemMin.x)) + "," +
                                          std::to_string(int32_t(itemMin.y)) + " " +
                                          std::to_string(int32_t(itemMax.x)) + "," +
                                          std::to_string(int32_t(itemMax.y));

                            if (current)
                            {
                                ImGui::SetItemDefaultFocus();
                            }
                        }

                        ImGui::EndCombo();
                    }

                    if (!styleItems.empty())
                    {
                        const uint64_t styleLayout = std::hash<std::string>{}(styleItems);

                        if (m_loggedStyleLayout != styleLayout)
                        {
                            m_loggedStyleLayout = styleLayout;
                            PrintMessage(("style items:" + styleItems).c_str(), this);
                        }
                    }

                    appendRect("style");

                    float intensity = light.intensity;
                    bool intensityDrawn = ImGui::SliderFloat("Light key##light", &intensity, 0.0f, 800.0f,
                                                             "%.0f");

                    appendRect("lightkey");

                    if (intensityDrawn)
                    {
                        SetEditorLightIntensity(light, intensity);
                        PrintMessage((selectionLabel + ": light key " +
                                      std::to_string(int32_t(intensity)))
                                         .c_str(),
                                     this);
                    }

                    const bool radiusDrawn = ImGui::SliderFloat("Radius##light", &light.radius, 0.0f, 256.0f,
                                                                "%.1f units");

                    appendRect("radius");

                    if (radiusDrawn)
                    {
                        PrintMessage((selectionLabel + ": radius " +
                                      std::to_string(int32_t(light.radius)))
                                         .c_str(),
                                     this);
                    }

                    const bool colourDrawn = ImGui::ColorEdit3("Colour##light", light.tint);

                    appendRect("colour");

                    if (colourDrawn)
                    {
                        SetEditorLightTint(light, light.tint);
                        PrintMessage((selectionLabel + ": colour " +
                                      std::to_string(light.tint[0]) + " " + std::to_string(light.tint[1]) +
                                      " " + std::to_string(light.tint[2]))
                                         .c_str(),
                                     this);
                    }

                    const bool positionDrawn =
                        ImGui::DragFloat3("Position##light", light.position, 1.0f, -16384.0f, 16384.0f,
                                          "%.1f units");

                    appendRect("position");

                    if (positionDrawn)
                    {
                        light.cluster = ResolveEditorLightCluster(m_scene, light.position);
                        PrintMessage((selectionLabel + ": position " +
                                      std::to_string(int32_t(light.position[0])) + " " +
                                      std::to_string(int32_t(light.position[1])) + " " +
                                      std::to_string(int32_t(light.position[2])) + " cluster " +
                                      std::to_string(light.cluster))
                                         .c_str(),
                                     this);
                    }

                    // Aim and drop: the light lands just off the picked face on the camera side,
                    // so it sits in the room instead of inside the wall.
                    ImGui::BeginDisabled(!m_hasPickPoint);

                    if (ImGui::Button("Place at pick##light") && m_hasPickPoint)
                    {
                        float ray[3] = { m_pickPoint[0] - m_cameraPosition[0],
                                         m_pickPoint[1] - m_cameraPosition[1],
                                         m_pickPoint[2] - m_cameraPosition[2] };
                        const float rayLength =
                            std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);

                        if (rayLength > 1e-4f)
                        {
                            // Stand off the surface along the pick ray so the light does not sit
                            // inside the face it lights, but by the same short step whatever its
                            // radius: a radius-sized step walks a large light out of the map,
                            // where it resolves to no cluster and shades nothing at all.
                            constexpr float kPlaceOffsetUnits = 8.0f;

                            // The cone is aimed down the pick ray, at what was picked: a spot
                            // that faced the camera would light the space behind it and leave
                            // the surface the author clicked on dark.
                            for (int axis = 0; axis < 3; axis++)
                            {
                                ray[axis] /= rayLength;
                                light.position[axis] = m_pickPoint[axis] - ray[axis] * kPlaceOffsetUnits;
                                light.direction[axis] = ray[axis];
                            }

                            light.cluster = ResolveEditorLightCluster(m_scene, light.position);

                            // A light that fell outside every leaf still belongs to what was
                            // picked: the surface's own cluster is where its list has to be.
                            if (light.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER)
                            {
                                light.cluster = m_pickCluster;
                            }

                            PrintMessage((selectionLabel + ": placed at " +
                                          std::to_string(int32_t(light.position[0])) + " " +
                                          std::to_string(int32_t(light.position[1])) + " " +
                                          std::to_string(int32_t(light.position[2])) + " cluster " +
                                          std::to_string(light.cluster))
                                             .c_str(),
                                         this);
                            LogLightCluster(light, selectionID, selectionLabel.c_str());
                        }
                    }

                    ImGui::EndDisabled();
                    appendRect("place");

                    // The `spot` key and its cone, uploaded through qrUploadSpotLight so the
                    // light stops being a point source.
                    const bool spotDrawn = ImGui::Checkbox("Spot##light", &light.spot);

                    appendRect("spot");

                    if (spotDrawn)
                    {
                        PrintMessage((selectionLabel + ": spot " +
                                      (light.spot ? "on" : "off"))
                                         .c_str(),
                                     this);
                        LogLightCluster(light, selectionID, selectionLabel.c_str());
                    }

                    if (light.spot)
                    {
                        // Three separate fields so each axis can be pointed at on its own - a
                        // DragFloat3 packs them into one item rect and hides the axis under the
                        // pointer, which makes both authoring and scripted checks guesswork.
                        ImGui::TextUnformatted("Spot dir");

                        const float fieldWidth =
                            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) /
                            3.0f;

                        bool directionDrawn = false;

                        for (int axis = 0; axis < 3; axis++)
                        {
                            const char *ids[3] = {"##dirx", "##diry", "##dirz"};

                            if (axis > 0)
                            {
                                ImGui::SameLine();
                            }

                            ImGui::SetNextItemWidth(fieldWidth);
                            directionDrawn |=
                                ImGui::DragFloat(ids[axis], &light.direction[axis], 0.05f, -1.0f, 1.0f,
                                                 "%.2f");
                            appendRect(axis == 0 ? "dirx" : (axis == 1 ? "diry" : "dirz"));
                        }

                        if (directionDrawn)
                        {
                            PrintMessage((selectionLabel + ": direction " +
                                          std::to_string(light.direction[0]) + " " +
                                          std::to_string(light.direction[1]) + " " +
                                          std::to_string(light.direction[2]))
                                             .c_str(),
                                         this);
                        }

                        bool coneDrawn = ImGui::SliderFloat("Inner deg##light", &light.angleInner, 0.0f,
                                                            90.0f, "%.1f deg");

                        appendRect("inner");

                        coneDrawn |= ImGui::SliderFloat("Outer deg##light", &light.angleOuter, 0.0f, 90.0f,
                                                        "%.1f deg");

                        appendRect("outer");

                        if (coneDrawn)
                        {
                            ClampEditorLightCone(light);
                            PrintMessage((selectionLabel + ": cone " +
                                          std::to_string(int32_t(light.angleInner)) + ".." +
                                          std::to_string(int32_t(light.angleOuter)) + " deg")
                                             .c_str(),
                                         this);
                        }
                    }

                    {
                        const uint64_t layout = std::hash<std::string>{}(rects);
                        const uint64_t key = layout | 1u;

                        if (m_loggedLightLayout != key)
                        {
                            m_loggedLightLayout = key;
                            PrintMessage(("panel rect:" + rects + " scroll " +
                                          std::to_string(int32_t(ImGui::GetScrollY())))
                                             .c_str(),
                                         this);
                        }
                    }
                }
                else
                {
                    ImGui::TextUnformatted("Nothing picked - LMB in the viewport");
                }

                if (m_hasPickPoint)
                {
                    ImGui::Text("At        %.1f %.1f %.1f", m_pickPoint[0], m_pickPoint[1], m_pickPoint[2]);
                }

                if (ImGui::Button("Clear selection"))
                {
                    ClearSelection();
                }

                ImGui::Separator();
                ImGui::TextUnformatted("Sun");
                ImGui::SliderFloat("Intensity", &m_sunIntensity, 0.0f, 64.0f, "%.2f");
                ImGui::SliderFloat("Azimuth", &m_sunAzimuth, 0.0f, 360.0f, "%.0f deg");
                ImGui::SliderFloat("Elevation", &m_sunElevation, -89.0f, 89.0f, "%.0f deg");
                ImGui::SliderFloat("Angular size", &m_sunAngularDiameter, 0.1f, 8.0f, "%.2f deg");

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (BeginLoggedTab("Entities", tabFlags("Entities"), tabRects))
            {
                DrawLightList();
                ImGui::EndTabItem();
            }

            if (BeginLoggedTab("Render", tabFlags("Render"), tabRects))
            {
                DrawGlobalSettings();
                ImGui::EndTabItem();
            }

            if (BeginLoggedTab("Materials", tabFlags("Materials"), tabRects))
            {
                DrawMaterials();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        const uint64_t tabLayout = std::hash<std::string>{}(tabRects) | 1u;

        if (m_loggedTabLayout != tabLayout)
        {
            m_loggedTabLayout = tabLayout;
            PrintMessage(("panel tabs:" + tabRects).c_str(), this);
        }

        ImGui::Separator();
        ImGui::TextUnformatted("RMB drag look  WASD move  Q/E down/up  Shift x4");
        ImGui::TextUnformatted("F1 panel  Ctrl+O reset camera");
    }

    ImGui::End();
}

void EditorApp::DrawGlobalSettings()
{
    EditorRenderSettings &settings = m_settings;

    ImGui::Separator();

    // Every group is open by default and the whole block scrolls, so the panel
    // stays inside the viewport however many settings the game editor grows.
    const float available = ImGui::GetIO().DisplaySize.y * 0.45f;
    const float scrollHeight = available < 520.0f ? available : 520.0f;

    if (!ImGui::BeginChild("GlobalScroll", ImVec2(360.0f, scrollHeight)))
    {
        ImGui::EndChild();
        return;
    }

    const ImGuiTreeNodeFlags openGroup = ImGuiTreeNodeFlags_DefaultOpen;

    if (ImGui::CollapsingHeader("Global", openGroup))
    {
        ImGui::SliderFloat("Brightness", &settings.brightness, 0.0f, 3.0f, "%.2f");

        const char *skyTypes[] = { "Colour", "Cubemap", "Rasterized", "Procedural" };
        ImGui::Combo("Sky type", &settings.skyType, skyTypes, IM_ARRAYSIZE(skyTypes));

        ImGui::SliderFloat("Sky scale", &settings.skyScale, 0.0f, 8.0f, "%.2f");
        ImGui::SliderFloat("Sky brightness", &settings.skyBrightness, 0.0f, 10.0f, "%.2f");
        ImGui::ColorEdit3("Sky colour", settings.skyColor);
        ImGui::SliderFloat("Sky light mult", &settings.skyLightMultiplier, 0.0f, 8.0f, "%.2f");
        ImGui::SliderInt("Sky ambient lod", &settings.skyAmbientLod, 0, 10);
        ImGui::Checkbox("Sky nee", &settings.skyNee);
    }

    if (ImGui::CollapsingHeader("Clouds", openGroup))
    {
        ImGui::Checkbox("Clouds##enabled", &settings.clouds);
        ImGui::ColorEdit3("Cloud colour", settings.cloudsColor);
        ImGui::SliderFloat("Cloud alpha", &settings.cloudsAlpha, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Coverage", &settings.cloudsCoverage, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Density", &settings.cloudsDensity, 0.0f, 10.0f, "%.2f");
        ImGui::SliderFloat("Speed", &settings.cloudsSpeed, 0.0f, 4.0f, "%.2f");
        ImGui::SliderInt("Quality", &settings.cloudsQuality, 0, QR_SKY_CLOUDS_MAX_QUALITY);
        ImGui::SliderFloat("Height", &settings.cloudsHeight, 1.0f, 300000.0f, "%.0f units");
        ImGui::SliderFloat("Thickness", &settings.cloudsThickness, 1.0f, 300000.0f, "%.0f units");
    }

    if (ImGui::CollapsingHeader("Sun disc", openGroup))
    {
        ImGui::SliderFloat("Disc scale", &settings.sunDiscScale, 0.0f, 20.0f, "%.2f");
        ImGui::ColorEdit3("Disc colour", settings.sunColor);
        ImGui::SliderFloat("Disc size", &settings.sunDiscSize, 0.0f, 10.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("God rays", openGroup))
    {
        ImGui::Checkbox("God rays##enabled", &settings.godRays);
        ImGui::SliderFloat("Shaft strength", &settings.godRaysIntensity, 0.0f, 4.0f, "%.2f");
        ImGui::SliderInt("Shaft quality", &settings.godRaysQuality, 0, 3);
    }

    if (ImGui::CollapsingHeader("Volumetric fog", openGroup))
    {
        const char *volumeTypes[] = { "Off", "Depth based", "Full" };
        ImGui::Combo("Volume type", &settings.volumeType, volumeTypes, IM_ARRAYSIZE(volumeTypes));
        ImGui::SliderFloat("Scattering", &settings.volumeScatter, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Ambient", &settings.volumeAmbient, 0.0f, 8.0f, "%.2f");
        ImGui::SliderFloat("Far", &settings.volumeFar, 0.0f, 4000.0f, "%.0f units");
        ImGui::SliderFloat("Light intensity", &settings.volumeLightIntensity, 0.0f, 1000.0f, "%.0f");
        ImGui::SliderFloat("Asymmetry", &settings.volumeAsymmetry, -0.99f, 0.99f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Tonemap", openGroup))
    {
        ImGui::SliderFloat("White point", &settings.tonemapWhite, 1.0f, 40.0f, "%.2f");
        ImGui::SliderFloat("Chroma", &settings.tonemapChroma, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Desaturation", &settings.tonemapDesat, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Max log", &settings.tonemapMaxLog, -4.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("Min log", &settings.tonemapMinLog, -12.0f, 0.0f, "%.2f");
        ImGui::SliderInt("Tonemap type", &settings.tonemapType, 0, 4);
    }

    if (ImGui::CollapsingHeader("Level fog", openGroup))
    {
        ImGui::ColorEdit3("Fog colour", settings.fogColor);
        ImGui::SliderFloat("Density", &settings.fogDensity, 0.0f, 1.0f, "%.4f");
        ImGui::SliderFloat("Sky blend", &settings.fogSkyBlend, 0.0f, 1.0f, "%.2f");
    }

    ImGui::EndChild();
}

void EditorApp::DrawMaterials()
{
    ImGui::Separator();
    ImGui::TextUnformatted("Materials");

    if (m_materials.Empty())
    {
        ImGui::TextUnformatted("No surfaces loaded");
        return;
    }

    // One entry per texture name, sorted. The list scrolls on its own so the
    // parameter sections below stay reachable on a short screen.
    const float availableListHeight = ImGui::GetIO().DisplaySize.y * 0.22f;
    const float listHeight = availableListHeight < 160.0f ? availableListHeight : 160.0f;

    if (ImGui::BeginChild("MaterialList", ImVec2(360.0f, listHeight), true))
    {
        for (size_t i = 0; i < m_materials.Count(); i++)
        {
            const bool selected = int32_t(i) == m_selectedMaterial;

            if (ImGui::Selectable(m_materials.NameAt(i).c_str(), selected))
            {
                m_selectedMaterial = int32_t(i);
            }
        }
    }

    ImGui::EndChild();

    if (m_selectedMaterial < 0 || size_t(m_selectedMaterial) >= m_materials.Count())
    {
        m_selectedMaterial = -1;
        return;
    }

    // The sections scroll on their own so the panel stays inside the viewport on a
    // short screen, however many parameters a material grows.
    if (!ImGui::BeginChild("MaterialParams", ImVec2(0.0f, 0.0f)))
    {
        ImGui::EndChild();
        return;
    }

        EditorMaterialParams &material = m_materials.At(size_t(m_selectedMaterial));
        bool changed = false;

        if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen))
        {
            changed |= ImGui::SliderFloat("Scale U##tex", &material.textureScaleU, 0.05f, 16.0f, "%.2f");
            changed |= ImGui::SliderFloat("Scale V##tex", &material.textureScaleV, 0.05f, 16.0f, "%.2f");
            changed |= ImGui::SliderFloat("Offset U##tex", &material.textureOffsetU, -8.0f, 8.0f, "%.2f");
            changed |= ImGui::SliderFloat("Offset V##tex", &material.textureOffsetV, -8.0f, 8.0f, "%.2f");

            if (ImGui::Button("Reset texture transform"))
            {
                material.textureScaleU = 1.0f;
                material.textureScaleV = 1.0f;
                material.textureOffsetU = 0.0f;
                material.textureOffsetV = 0.0f;
                changed = true;
            }
        }

        if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // roughness_override at 0 means "no override" in the game editor, so the
            // checkbox owns whether a value is sent at all.
            bool roughnessOverride = material.roughnessOverride >= 0.0f;

            if (ImGui::Checkbox("Roughness override", &roughnessOverride))
            {
                material.roughnessOverride = roughnessOverride ? kEditorDefaultRoughness : -1.0f;
                changed = true;
            }

            if (roughnessOverride)
            {
                changed |= ImGui::SliderFloat("Roughness##surf", &material.roughnessOverride, 0.0f, 1.0f,
                                              "%.3f");
            }

            changed |= ImGui::SliderFloat("Metalness##surf", &material.metalnessFactor, 0.0f, 1.0f,
                                          "%.3f");
            changed |= ImGui::SliderFloat("Base factor##surf", &material.baseFactor, 0.0f, 4.0f, "%.2f");

            int32_t type = int32_t(material.type);
            const char *materialTypes[] = { "None", "Glass", "Water" };

            if (ImGui::Combo("Material##surf", &type, materialTypes, IM_ARRAYSIZE(materialTypes)))
            {
                material.type = EditorMaterialType(type);
                changed = true;
            }
        }

        if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen))
        {
            changed |= ImGui::Checkbox("Exact normals##model", &material.exactNormals);
        }

        if (ImGui::CollapsingHeader("Luma", ImGuiTreeNodeFlags_DefaultOpen))
        {
            changed |= ImGui::SliderFloat("Emissive factor##luma", &material.emissiveFactor, 0.0f, 1.0f,
                                          "%.3f");
            changed |= ImGui::Checkbox("Luma##luma", &material.luma);
        }

    if (changed)
    {
        m_materialsChanged = true;
    }

    ImGui::EndChild();
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
            else if (i + 1 < argc && argument == L"--tab")
            {
                m_initialTab = Utf8FromWide(argv[++i]);
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

    if (!UploadWorldClusters())
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
