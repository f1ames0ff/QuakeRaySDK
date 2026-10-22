#include "EditorApp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

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

bool EditorApp::LoadScene()
{
    std::string error;

    if (!LoadEditorScene(m_scenePath, m_scene, error))
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

    const QrResult beginResult = qrBeginStaticGeometries(m_instance);

    if (beginResult != QR_SUCCESS)
    {
        PrintMessage(("begin static scene: " + std::string(qrGetResultDescription(beginResult))).c_str(), this);
        return false;
    }

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
    PrintMessage(("geometry upload: " + std::string(qrGetResultDescription(uploadResult))).c_str(), this);

    if (uploadResult != QR_SUCCESS)
    {
        return false;
    }

    QrDirectionalLightUploadInfo sun = {};
    sun.uniqueID = 2;
    sun.color = { 8.0f, 8.0f, 8.0f };
    sun.direction = { 0.3f, 0.5f, -0.8f };
    sun.angularDiameterDegrees = 0.5f;

    const QrResult lightResult = qrUploadDirectionalLight(m_instance, &sun);
    PrintMessage(("sun upload: " + std::string(qrGetResultDescription(lightResult))).c_str(), this);

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

    float eye[3] = { 4.0f, -6.0f, 3.0f };
    float target[3] = { 0.0f, 0.0f, 1.0f };
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    float sceneExtent = 10.0f;

    if (m_scene.hasBounds)
    {
        float center[3] = {};

        for (int axis = 0; axis < 3; axis++)
        {
            center[axis] = (m_scene.boundsMin[axis] + m_scene.boundsMax[axis]) * 0.5f;
            sceneExtent = std::max(sceneExtent,
                                   m_scene.boundsMax[axis] - m_scene.boundsMin[axis]);
        }

        if (m_scene.hasCamera)
        {
            for (int axis = 0; axis < 3; axis++)
            {
                eye[axis] = m_scene.cameraPosition[axis];
            }

            const float yaw = m_scene.cameraYawDegrees * 0.01745329252f;
            target[0] = eye[0] + std::cos(yaw) * 512.0f;
            target[1] = eye[1] + std::sin(yaw) * 512.0f;
            target[2] = eye[2];
        }
        else
        {
            eye[0] = center[0] + sceneExtent * 0.8f;
            eye[1] = center[1] - sceneExtent * 1.0f;
            eye[2] = center[2] + sceneExtent * 0.6f;

            for (int axis = 0; axis < 3; axis++)
            {
                target[axis] = center[axis];
            }
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
    if (m_instance != nullptr)
    {
        qrDestroyInstance(m_instance);
        m_instance = nullptr;
    }
}
