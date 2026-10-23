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

// A light is picked when the ray passes this close to its origin, the way QRE_LightUnderCrosshair
// accepts a light the crosshair is near rather than exactly on.
constexpr float kLightPickRadiusUnits = 24.0f;

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
        }

        break;
    }

    case WM_LBUTTONDOWN:
    {
        if (!m_ui.WantsMouse())
        {
            Pick();
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

void EditorApp::UploadLights()
{
    QrResult lastResult = QR_SUCCESS;

    for (size_t index = 0; index < m_scene.lights.size(); index++)
    {
        const EditorLight &light = m_scene.lights[index];

        QrSphericalLightUploadInfo info = {};
        // RT_UploadAllElights numbers map lights from UINT16_MAX upwards and
        // keeps the editor's world geometry IDs (1..N) out of the way.
        info.uniqueID = LightUniqueID(index);
        info.color = { light.color[0], light.color[1], light.color[2] };
        info.position = { light.position[0], light.position[1], light.position[2] };
        info.radius = light.radius;

        lastResult = qrUploadSphericalLight(m_instance, &info);

        if (lastResult != QR_SUCCESS)
        {
            break;
        }
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

void EditorApp::UploadClusterLightSources()
{
    if (m_scene.worldClusters.numLeafs == 0 || m_scene.lights.empty())
    {
        return;
    }

    std::vector<QrClusterLightSource> sources(m_scene.lights.size());

    for (size_t index = 0; index < m_scene.lights.size(); index++)
    {
        const EditorLight &light = m_scene.lights[index];

        sources[index].uniqueID = LightUniqueID(index);
        sources[index].origin = { light.position[0], light.position[1], light.position[2] };
        sources[index].cluster = light.cluster;
        // RT_LIGHT_Emit registers a map light with the static reach and no cluster list: the
        // top-up pass is what hands it the clusters around its own leaf.
        sources[index].reach = kLightReachUnits;
        sources[index].radius = 0.0f;
        sources[index].clusterCount = 0;
        sources[index].pClusters = nullptr;
        sources[index].power = 0.0f;
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

int32_t EditorApp::PickLight(const float origin[3], const float direction[3], float &outDistance) const
{
    // QRE_LightUnderCrosshair accepts the light nearest the crosshair ray rather than one exactly
    // under it, because a light is drawn as a small gizmo, not as a surface.
    int32_t bestLight = -1;
    float bestDistance = 0.0f;

    for (size_t index = 0; index < m_scene.lights.size(); index++)
    {
        const EditorLight &light = m_scene.lights[index];
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
            bestLight = int32_t(index);
            bestDistance = along;
        }
    }

    outDistance = bestDistance;
    return bestLight;
}

void EditorApp::ClearSelection()
{
    m_selectedSurface = -1;
    m_selectedTriangle = -1;
    m_selectedLight = -1;
    m_hasPickPoint = false;
    m_pickCluster = 0;
}

void EditorApp::Pick()
{
    if (m_scene.surfaces.empty() && m_scene.lights.empty())
    {
        return;
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

    if (m_clientWidth == 0 || m_clientHeight == 0)
    {
        return;
    }

    const float aspect = float(m_clientWidth) / float(m_clientHeight);
    const float tanHalfFov = std::tan(0.5f * kEditorFovYRadians);
    const float ndcX = (cursorX / float(m_clientWidth)) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (cursorY / float(m_clientHeight)) * 2.0f;

    float direction[3] = {
        forward[0] + side[0] * (ndcX * tanHalfFov * aspect) + up[0] * (ndcY * tanHalfFov),
        forward[1] + side[1] * (ndcX * tanHalfFov * aspect) + up[1] * (ndcY * tanHalfFov),
        forward[2] + side[2] * (ndcX * tanHalfFov * aspect) + up[2] * (ndcY * tanHalfFov),
    };

    const float directionLength = std::sqrt(direction[0] * direction[0] +
                                            direction[1] * direction[1] +
                                            direction[2] * direction[2]);

    if (directionLength <= 1e-6f)
    {
        return;
    }

    direction[0] /= directionLength;
    direction[1] /= directionLength;
    direction[2] /= directionLength;

    int32_t triangle = -1;
    float surfaceDistance = 0.0f;
    const int32_t surface = PickSurface(m_cameraPosition, direction, triangle, surfaceDistance);
    float lightDistance = 0.0f;
    const int32_t light = PickLight(m_cameraPosition, direction, lightDistance);

    // A light in front of the surface hides it, exactly like a gizmo drawn over the wall behind.
    const bool lightWins = light >= 0 && (surface < 0 || lightDistance < surfaceDistance);

    m_selectedLight = lightWins ? light : -1;
    m_selectedSurface = (!lightWins && surface >= 0) ? surface : -1;
    m_selectedTriangle = (!lightWins && surface >= 0) ? triangle : -1;
    m_pickCluster = 0;

    if (m_selectedSurface >= 0)
    {
        const EditorSurface &picked = m_scene.surfaces[size_t(m_selectedSurface)];
        const size_t base = size_t(m_selectedTriangle) * 3;

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
    else if (m_selectedLight >= 0)
    {
        const EditorLight &picked = m_scene.lights[size_t(m_selectedLight)];

        m_hasPickPoint = true;
        m_pickCluster = picked.cluster;

        for (int axis = 0; axis < 3; axis++)
        {
            m_pickPoint[axis] = picked.position[axis];
        }
    }
    else
    {
        m_hasPickPoint = false;
    }

    if (m_selectedSurface < 0 && m_selectedLight < 0)
    {
        ClearSelection();
        PrintMessage("pick: nothing under the cursor", this);
        return;
    }

    if (m_selectedLight >= 0)
    {
        PrintMessage(("pick: light " + std::to_string(m_selectedLight)).c_str(), this);
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

    float side[3] = {};
    float up[3] = {};
    float forward[3] = {};
    ViewBasis(side, up, forward);

    const float aspect = float(m_clientWidth) / float(m_clientHeight);
    const float tanHalfFov = std::tan(0.5f * kEditorFovYRadians);

    // Projects a world point the same way ViewBasis casts a ray, so the outline lands on the
    // geometry the click resolved. Returns false when the point is behind the camera.
    const auto project = [&](const float point[3], float &outX, float &outY) -> bool {
        const float relative[3] = {
            point[0] - m_cameraPosition[0],
            point[1] - m_cameraPosition[1],
            point[2] - m_cameraPosition[2],
        };

        const float depth = relative[0] * forward[0] + relative[1] * forward[1] +
                            relative[2] * forward[2];

        if (depth <= 1e-3f)
        {
            return false;
        }

        const float horizontal = relative[0] * side[0] + relative[1] * side[1] +
                                 relative[2] * side[2];
        const float vertical = relative[0] * up[0] + relative[1] * up[1] + relative[2] * up[2];

        const float ndcX = horizontal / (depth * tanHalfFov * aspect);
        const float ndcY = vertical / (depth * tanHalfFov);

        outX = (ndcX * 0.5f + 0.5f) * float(m_clientWidth);
        outY = (1.0f - (ndcY * 0.5f + 0.5f)) * float(m_clientHeight);
        return true;
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

    if (m_selectedLight >= 0 && m_selectedLight < int32_t(m_scene.lights.size()))
    {
        // A three-axis cross at the light's origin stands in for the gizmo the game editor draws
        // with its own overlay path, which the SDK does not expose.
        const EditorLight &light = m_scene.lights[size_t(m_selectedLight)];
        const float extent = 12.0f;

        for (int axis = 0; axis < 3; axis++)
        {
            float from[3] = { light.position[0], light.position[1], light.position[2] };
            float to[3] = { light.position[0], light.position[1], light.position[2] };

            from[axis] -= extent;
            to[axis] += extent;

            float fromX = 0.0f;
            float fromY = 0.0f;
            float toX = 0.0f;
            float toY = 0.0f;

            if (project(from, fromX, fromY) && project(to, toX, toY))
            {
                drawList->AddLine(ImVec2(fromX, fromY), ImVec2(toX, toY), marker, 2.0f);
            }
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
        else if (m_selectedLight >= 0 && m_selectedLight < int32_t(m_scene.lights.size()))
        {
            EditorLight &light = m_scene.lights[size_t(m_selectedLight)];

            ImGui::Text("Light     %d  %s", m_selectedLight,
                        light.className.empty() ? "light" : light.className.c_str());
            ImGui::Text("Position  %.1f %.1f %.1f", light.position[0], light.position[1],
                        light.position[2]);
            ImGui::Text("Radiance  %.0f %.0f %.0f", light.color[0], light.color[1], light.color[2]);
            ImGui::Text("Cluster   %s (%u)",
                        light.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER ? "none" : "resolved",
                        light.cluster);

            // Authoring mirrors the map entity keys the game honours: `light` (which becomes
            // the radiance through kEditorLightIntensityScale) and rt_elight_radius.
            float intensity = light.intensity;

            if (ImGui::SliderFloat("Light key##light", &intensity, 0.0f, 800.0f, "%.0f"))
            {
                SetEditorLightIntensity(light, intensity);
                PrintMessage(("light " + std::to_string(m_selectedLight) + ": light key " +
                              std::to_string(int32_t(intensity)))
                                 .c_str(),
                             this);
            }

            if (ImGui::SliderFloat("Radius##light", &light.radius, 0.0f, 256.0f, "%.1f units"))
            {
                PrintMessage(("light " + std::to_string(m_selectedLight) + ": radius " +
                              std::to_string(int32_t(light.radius)))
                                 .c_str(),
                             this);
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
