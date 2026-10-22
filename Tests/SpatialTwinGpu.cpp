#include <qray/qray.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 360;

struct FileBlob
{
    std::vector<uint8_t> bytes;
};

void PrintMessage(const char *message, void *)
{
    if (message != nullptr)
    {
        std::fprintf(stderr, "%s\n", message);
    }
}

void OpenFile(const char *path, void *, const void **data, uint32_t *size, void **fileHandle)
{
    *data = nullptr;
    *size = 0;
    *fileHandle = nullptr;

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

void CloseFile(void *fileHandle, void *)
{
    delete static_cast<FileBlob *>(fileHandle);
}

bool Check(QrResult result, const char *what)
{
    if (result != QR_SUCCESS)
    {
        std::fprintf(stderr, "%s: %s\n", what, qrGetResultDescription(result));
        return false;
    }

    return true;
}

float Dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Cross(const float a[3], const float b[3], float result[3])
{
    result[0] = a[1] * b[2] - a[2] * b[1];
    result[1] = a[2] * b[0] - a[0] * b[2];
    result[2] = a[0] * b[1] - a[1] * b[0];
}

void Normalize(float value[3])
{
    const float length = std::sqrt(Dot(value, value));

    if (length > 0.000001f)
    {
        value[0] /= length;
        value[1] /= length;
        value[2] /= length;
    }
}

void LookAt(const float eye[3], const float target[3], const float up[3], bool rightHanded, float view[16])
{
    float forward[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
    Normalize(forward);

    float side[3] = {};
    Cross(forward, up, side);

    if (!rightHanded)
    {
        side[0] = -side[0];
        side[1] = -side[1];
        side[2] = -side[2];
    }

    Normalize(side);

    float correctedUp[3] = {};

    if (rightHanded)
    {
        Cross(side, forward, correctedUp);
    }
    else
    {
        Cross(forward, side, correctedUp);
    }

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

    view[12] = -Dot(side, eye);
    view[13] = -Dot(correctedUp, eye);
    view[14] = Dot(forward, eye);
    view[15] = 1.0f;
}

void TransformPoint(const float matrix[16], const float point[3], float out[3])
{
    for (int row = 0; row < 3; row++)
    {
        out[row] = matrix[0 * 4 + row] * point[0] +
                   matrix[1 * 4 + row] * point[1] +
                   matrix[2 * 4 + row] * point[2] +
                   matrix[3 * 4 + row];
    }
}

void TransformDirection(const float matrix[16], const float direction[3], float out[3])
{
    for (int row = 0; row < 3; row++)
    {
        out[row] = matrix[0 * 4 + row] * direction[0] +
                   matrix[1 * 4 + row] * direction[1] +
                   matrix[2 * 4 + row] * direction[2];
    }
}

void MakeYUpFrame(float scale, float frame[16], float inverse[16])
{
    const float rotation[3][3] = {
        { 1.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, -1.0f },
        { 0.0f, 1.0f, 0.0f },
    };

    std::memset(frame, 0, 16 * sizeof(float));
    std::memset(inverse, 0, 16 * sizeof(float));

    for (int row = 0; row < 3; row++)
    {
        for (int column = 0; column < 3; column++)
        {
            frame[column * 4 + row] = scale * rotation[row][column];
            inverse[column * 4 + row] = rotation[column][row] / scale;
        }
    }

    frame[15] = 1.0f;
    inverse[15] = 1.0f;
}

void MakeMirrorXFrame(float scale, float inverse[16])
{
    std::memset(inverse, 0, 16 * sizeof(float));

    inverse[0] = -1.0f / scale;
    inverse[5] = 1.0f / scale;
    inverse[10] = 1.0f / scale;
    inverse[15] = 1.0f;
}

struct RenderResult
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowPitch = 0;
};

class TwinRenderer
{
public:
    TwinRenderer(HWND window, const char *shaderFolder, const char *blueNoisePath)
        : window(window)
        , shaderFolder(shaderFolder)
        , blueNoisePath(blueNoisePath)
    {
    }

    ~TwinRenderer()
    {
        if (instance != nullptr)
        {
            qrDestroyInstance(instance);
        }
    }

    bool Create()
    {
        QrWin32SurfaceCreateInfo surface = {
            .hinstance = GetModuleHandleW(nullptr),
            .hwnd = window,
        };

        QrInstanceCreateInfo info = {};
        info.pAppName = "QuakeRay spatial twin";
        info.pAppGUID = "8aeb2a91-6d74-4d32-9a84-0c6a5e4d2a11";
        info.pWin32SurfaceInfo = &surface;
        info.pfnPrint = PrintMessage;
        info.pfnOpenFile = OpenFile;
        info.pfnCloseFile = CloseFile;
        info.pShaderFolderPath = shaderFolder.c_str();
        info.pBlueNoiseFilePath = blueNoisePath.c_str();
        info.primaryRaysMaxAlbedoLayers = 2;
        info.indirectIlluminationMaxAlbedoLayers = 1;
        info.rayCullBackFacingTriangles = 1;
        info.allowGeometryWithSkyFlag = 1;
        info.rasterizedMaxVertexCount = 1u << 20;
        info.rasterizedMaxIndexCount = 1u << 21;
        info.rasterizedVertexColorGamma = 1;
        info.rasterizedSkyCubemapSize = 64;
        info.maxTextureCount = 1024;
        info.textureSamplerForceMinificationFilterLinear = 1;
        info.textureSamplerForceNormalMapFilterLinear = 1;
        info.originalAlbedoAlphaTextureIsSRGB = 1;
        info.originalRoughnessMetallicEmissionTextureIsSRGB = 0;
        info.originalNormalTextureIsSRGB = 0;
        info.overridenAlbedoAlphaTextureIsSRGB = 1;
        info.overridenRoughnessMetallicEmissionTextureIsSRGB = 0;
        info.overridenNormalTextureIsSRGB = 0;

        if (!Check(qrCreateInstance(&info, &instance), "create instance"))
        {
            return false;
        }

        return true;
    }

    bool SetConvention(const QrSpatialConvention &convention)
    {
        return Check(qrSetSpatialConvention(instance, &convention), "set spatial convention");
    }

    bool CreateMaterial()
    {
        const uint8_t albedo[16] = {
            220, 190, 150, 255, 90, 80, 70, 255,
            90, 80, 70, 255, 220, 190, 150, 255,
        };

        QrMaterialCreateInfo info = {};
        info.size = { 2, 2 };
        info.textures.pDataAlbedoAlpha = albedo;
        info.filter = QR_SAMPLER_FILTER_NEAREST;
        info.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;

        return Check(qrCreateMaterial(instance, &info, &material), "create material");
    }

    bool UploadScene(const QrVertex vertices[4], const uint32_t indices[6], const float sunDirection[3],
                     const float pointPosition[3], const float pointNormal[3], float pointRadius,
                     const float clusterMin[3], const float clusterMax[3],
                     const float particlePosition[3], float particleSize,
                     const float particleRight[3], const float particleUp[3])
    {
        QrGeometryUploadInfo geometry = {};
        geometry.uniqueID = 10;
        geometry.flags = QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT;
        geometry.geomType = QR_GEOMETRY_TYPE_STATIC;
        geometry.passThroughType = QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
        geometry.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
        geometry.vertexCount = 4;
        geometry.pVertices = vertices;
        geometry.indexCount = 6;
        geometry.pIndices = indices;
        geometry.layerColors[0] = { 1.0f, 1.0f, 1.0f, 1.0f };
        geometry.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
        geometry.geomMaterial.layerMaterials[0] = material;
        geometry.defaultRoughness = 0.8f;
        geometry.defaultMetallicity = 0.0f;
        geometry.defaultEmission = 0.0f;
        geometry.transform.matrix[0][0] = 1.0f;
        geometry.transform.matrix[1][1] = 1.0f;
        geometry.transform.matrix[2][2] = 1.0f;

        if (!Check(qrBeginStaticGeometries(instance), "begin static") ||
            !Check(qrUploadGeometry(instance, &geometry), "upload geometry"))
        {
            return false;
        }

        QrDirectionalLightUploadInfo sun = {};
        sun.uniqueID = 11;
        sun.color = { 3.0f, 3.0f, 3.0f };
        sun.direction = { sunDirection[0], sunDirection[1], sunDirection[2] };
        sun.angularDiameterDegrees = 0.5f;

        QrSphericalLightUploadInfo point = {};
        point.uniqueID = 12;
        point.color = { 20.0f, 12.0f, 8.0f };
        point.position = { pointPosition[0], pointPosition[1], pointPosition[2] };
        point.radius = pointRadius;
        point.normal = { pointNormal[0], pointNormal[1], pointNormal[2] };

        if (!Check(qrUploadDirectionalLight(instance, &sun), "upload sun") ||
            !Check(qrUploadSphericalLight(instance, &point), "upload point light") ||
            !Check(qrSubmitStaticGeometries(instance), "submit static"))
        {
            return false;
        }

        const QrFloat3D boundsMin = { { clusterMin[0], clusterMin[1], clusterMin[2] } };
        const QrFloat3D boundsMax = { { clusterMax[0], clusterMax[1], clusterMax[2] } };
        const uint8_t clusterFlags = 0;

        QrWorldLightsUploadInfo world = {};
        world.numClusters = 1;
        world.pClusterMins = &boundsMin;
        world.pClusterMaxs = &boundsMax;
        world.pClusterFlags = &clusterFlags;
        world.clusterGeneration = 1;

        if (!Check(qrUploadWorldLights(instance, &world), "upload world lights"))
        {
            return false;
        }

        QrParticlePoint particle = {};
        particle.position[0] = particlePosition[0];
        particle.position[1] = particlePosition[1];
        particle.position[2] = particlePosition[2];
        particle.packedColor = 0x80FFFFFFu;
        particle.size = particleSize;
        particle.cluster = 0xFFFFFFFFu;

        QrParticleUploadInfo particles = {};
        particles.pPoints = &particle;
        particles.count = 1;
        particles.material = material;
        particles.pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE |
                                   QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST |
                                   QR_RASTERIZED_GEOMETRY_STATE_PARTICLE |
                                   QR_RASTERIZED_GEOMETRY_STATE_PARTICLE_SPRITE;
        particles.smokeLook = { { 0.0f, 1.0f, 10.0f, 0.0f } };
        particles.viewRight = { { particleRight[0], particleRight[1], particleRight[2] } };
        particles.viewUp = { { particleUp[0], particleUp[1], particleUp[2] } };

        if (!Check(qrUploadParticles(instance, &particles), "upload particles"))
        {
            return false;
        }

        return true;
    }

    bool Render(const float view[16], const float eye[3], const float up[3], float nearPlane,
                float farPlane, float rayLength, float fogDensity, RenderResult &result)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowPitch = 0;
        uint64_t bufferSize = 0;
        bool sized = false;

        QrDrawFrameRenderResolutionParams resolution = {};
        resolution.upscaleTechnique = QR_RENDER_UPSCALE_TECHNIQUE_NEAREST;
        resolution.sharpenTechnique = QR_RENDER_SHARPEN_TECHNIQUE_NONE;
        resolution.resolutionMode = QR_RENDER_RESOLUTION_MODE_CUSTOM;

        QrDrawFrameSkyParams sky = {};
        sky.skyType = QR_SKY_TYPE_COLOR;
        sky.skyColorDefault = { 0.02f, 0.03f, 0.05f };
        sky.skyColorMultiplier = 1.0f;
        sky.skyColorSaturation = 1.0f;

        QrDrawFrameLevelFogParams levelFog = {};
        levelFog.color = { 0.02f, 0.03f, 0.05f };
        levelFog.density = fogDensity;
        levelFog.skyBlend = 0.0f;

        QrDrawFrameTonemappingParams tonemapping = {};
        tonemapping.minLogLuminance = -8.0f;
        tonemapping.maxLogLuminance = 4.0f;
        tonemapping.luminanceWhitePoint = 1.0f;
        tonemapping.exposureBias = 0.0f;
        tonemapping.tonemapPower = 1.0f;
        tonemapping.highlightDesaturation = 0.0f;
        tonemapping.chromaPreservation = 1.0f;
        tonemapping.exposureSpeedUp = 1.0f;
        tonemapping.exposureSpeedDown = 1.0f;
        tonemapping.exposureLowPercentile = 0.0f;
        tonemapping.exposureHighPercentile = 1.0f;
        tonemapping.minAdaptedLuminance = 0.001f;
        tonemapping.maxAdaptedLuminance = 100.0f;
        tonemapping.tonemapType = 0;

        QrDrawFrameIlluminationParams illumination = {};
        illumination.neeLightSamples = 1;
        illumination.giBounceRays = 0.0f;
        illumination.denoiserEnabled = 0;

        QrDrawFrameInfo draw = {};
        std::memcpy(draw.view, view, 16 * sizeof(float));
        draw.worldUpVector = { { up[0], up[1], up[2] } };
        draw.fovYRadians = 1.0471975512f;
        draw.cameraNear = nearPlane;
        draw.cameraFar = farPlane;
        draw.rayLength = rayLength;
        draw.rayCullMaskWorld = QR_DRAW_FRAME_RAY_CULL_WORLD_0_BIT | QR_DRAW_FRAME_RAY_CULL_SKY_BIT;
        draw.currentTime = 1.0;
        draw.disableEyeAdaptation = 1;
        draw.forceAntiFirefly = 1;
        draw.pRenderResolutionParams = &resolution;
        draw.pSkyParams = &sky;
        draw.pLevelFogParams = &levelFog;
        draw.pTonemappingParams = &tonemapping;
        draw.pIlluminationParams = &illumination;

        uint64_t requestId = 0;
        bool requested = false;

        for (uint32_t frame = 0; frame < 12; frame++)
        {
            QrStartFrameInfo start = {};
            start.presentMode = QR_PRESENT_MODE_IMMEDIATE;
            start.maxFrameLatency = 1;
            start.hasFocus = 1;

            if (!Check(qrStartFrame(instance, &start), "start frame"))
            {
                return false;
            }

            if (!sized)
            {
                if (!Check(qrGetFrameReadbackRequirements(instance, QR_FRAME_READBACK_FORMAT_RGBA8_UNORM, QR_TRUE,
                                                          &width, &height, &rowPitch, &bufferSize),
                           "readback requirements"))
                {
                    return false;
                }

                result.pixels.resize(size_t(bufferSize));
                result.width = width;
                result.height = height;
                result.rowPitch = rowPitch;
                resolution.customRenderSize = { width, height };
                sized = true;
            }

            if (!requested)
            {
                QrFrameReadbackInfo readback = {};
                readback.width = width;
                readback.height = height;
                readback.rowPitch = rowPitch;
                readback.bufferSize = bufferSize;
                readback.format = QR_FRAME_READBACK_FORMAT_RGBA8_UNORM;
                readback.includeUi = QR_TRUE;
                readback.pDst = result.pixels.data();
                readback.frameId = QR_FRAME_ID_LATEST;

                QrResult requestResult = qrRequestFrameReadback(instance, &readback, &requestId);

                if (requestResult == QR_SUCCESS)
                {
                    requested = true;
                }
                else if (requestResult != QR_READBACK_BUSY)
                {
                    return Check(requestResult, "request readback");
                }
            }

            if (!Check(qrDrawFrame(instance, &draw), "draw frame"))
            {
                return false;
            }

            QrBool32 done = QR_FALSE;

            if (requested && Check(qrGetFrameReadbackStatus(instance, requestId, &done), "readback status") && done)
            {
                return true;
            }
        }

        std::fprintf(stderr, "readback did not complete\n");
        return false;
    }

private:
    HWND window = nullptr;
    std::string shaderFolder;
    std::string blueNoisePath;
    QrInstance instance = nullptr;
    QrMaterial material = QR_NO_MATERIAL;
};

void MakeQuad(float canonicalVertices[4][3], uint32_t indices[6])
{
    const float vertices[4][3] = {
        { -1.0f, -2.0f, 0.0f },
        { 3.0f, -2.0f, 0.0f },
        { 3.0f, 2.0f, 0.0f },
        { -1.0f, 2.0f, 0.0f },
    };

    for (int i = 0; i < 4; i++)
    {
        canonicalVertices[i][0] = vertices[i][0];
        canonicalVertices[i][1] = vertices[i][1];
        canonicalVertices[i][2] = vertices[i][2];
    }

    const uint32_t source[6] = { 0, 1, 2, 0, 2, 3 };
    std::memcpy(indices, source, sizeof(source));
}

void MakeVertexArray(const float positions[4][3], QrVertex vertices[4])
{
    for (int i = 0; i < 4; i++)
    {
        vertices[i] = {};
        vertices[i].position[0] = positions[i][0];
        vertices[i].position[1] = positions[i][1];
        vertices[i].position[2] = positions[i][2];
        vertices[i].normal[2] = 1.0f;
        vertices[i].packedColor = 0xFFFFFFFFu;
    }
}

bool CompareImages(const RenderResult &a, const RenderResult &b)
{
    if (a.width != b.width || a.height != b.height || a.rowPitch != b.rowPitch ||
        a.pixels.size() != b.pixels.size())
    {
        std::fprintf(stderr, "image extent mismatch\n");
        return false;
    }

    uint32_t maxDifference = 0;
    uint64_t totalDifference = 0;

    for (size_t i = 0; i < a.pixels.size(); i++)
    {
        const uint32_t difference = uint32_t(std::abs(int(a.pixels[i]) - int(b.pixels[i])));
        maxDifference = std::max(maxDifference, difference);
        totalDifference += difference;
    }

    const double meanDifference = double(totalDifference) / double(a.pixels.size());
    std::printf("spatial twin image difference: max=%u mean=%.4f\n", maxDifference, meanDifference);

    if (maxDifference > 3 || meanDifference > 0.05)
    {
        std::fprintf(stderr, "spatial twin images diverged\n");
        return false;
    }

    return true;
}

}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <shader-dir> <texture-dir>\n", argv[0]);
        return 1;
    }

    std::string shaderFolder = argv[1];
    if (!shaderFolder.empty() && shaderFolder.back() != '/' && shaderFolder.back() != '\\')
    {
        shaderFolder.push_back('\\');
    }

    const std::string blueNoisePath = std::string(argv[2]) + "\\BlueNoise_LDR_RGBA_128.png";

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"QuakeRaySpatialTwin";

    if (RegisterClassExW(&windowClass) == 0)
    {
        std::fprintf(stderr, "register window class failed\n");
        return 1;
    }

    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"QuakeRay spatial twin",
                                  WS_OVERLAPPEDWINDOW, 0, 0, kWidth, kHeight,
                                  nullptr, nullptr, windowClass.hInstance, nullptr);

    if (window == nullptr)
    {
        std::fprintf(stderr, "create window failed\n");
        return 1;
    }

    ShowWindow(window, SW_SHOWNOACTIVATE);
    SetWindowPos(window, nullptr, -32000, -32000, kWidth, kHeight, SWP_NOZORDER | SWP_NOACTIVATE);

    float canonicalPositions[4][3] = {};
    uint32_t indices[6] = {};
    MakeQuad(canonicalPositions, indices);

    QrVertex canonicalVertices[4] = {};
    MakeVertexArray(canonicalPositions, canonicalVertices);

    const float canonicalEye[3] = { 0.0f, -5.0f, 3.0f };
    const float canonicalTarget[3] = { 0.0f, 0.0f, 0.0f };
    const float canonicalUp[3] = { 0.0f, 0.0f, 1.0f };
    const float canonicalSun[3] = { 0.2f, 0.3f, -1.0f };
    const float canonicalPointPosition[3] = { 0.0f, -2.0f, 4.0f };
    const float canonicalPointNormal[3] = { 0.0f, 0.0f, 1.0f };
    const float canonicalPointRadius = 2.0f;
    const float canonicalFogDensity = 0.01f;
    const float canonicalClusterMin[3] = { -10.0f, -10.0f, -10.0f };
    const float canonicalClusterMax[3] = { 10.0f, 10.0f, 10.0f };
    const float canonicalParticlePosition[3] = { 1.0f, 0.0f, 1.0f };
    const float canonicalParticleRight[3] = { 1.0f, 0.0f, 0.0f };
    const float canonicalParticleUp[3] = { 0.0f, 0.0f, 1.0f };
    const float canonicalParticleSize = 1.0f;

    float canonicalView[16] = {};
    LookAt(canonicalEye, canonicalTarget, canonicalUp, true, canonicalView);

    QrSpatialConvention identity = {
        sizeof(QrSpatialConvention), QR_SPATIAL_CONVENTION_VERSION,
        QR_AXIS_POSITIVE_X, QR_AXIS_POSITIVE_Y, QR_AXIS_POSITIVE_Z,
        QR_TRUE, QR_SCENE_UNITS_PER_METER, {},
    };

    QrSpatialConvention yUp = {
        sizeof(QrSpatialConvention), QR_SPATIAL_CONVENTION_VERSION,
        QR_AXIS_POSITIVE_X, QR_AXIS_NEGATIVE_Z, QR_AXIS_POSITIVE_Y,
        QR_TRUE, 1.0f, {},
    };

    const float scale = QR_SCENE_UNITS_PER_METER;
    float frame[16] = {};
    float inverseFrame[16] = {};
    MakeYUpFrame(scale, frame, inverseFrame);

    float yUpPositions[4][3] = {};
    for (int i = 0; i < 4; i++)
    {
        TransformPoint(inverseFrame, canonicalPositions[i], yUpPositions[i]);
    }

    QrVertex yUpVertices[4] = {};
    MakeVertexArray(yUpPositions, yUpVertices);

    float yUpEye[3] = {};
    float yUpTarget[3] = {};
    float yUpUp[3] = {};
    TransformPoint(inverseFrame, canonicalEye, yUpEye);
    TransformPoint(inverseFrame, canonicalTarget, yUpTarget);
    TransformDirection(inverseFrame, canonicalUp, yUpUp);
    Normalize(yUpUp);

    float yUpView[16] = {};
    LookAt(yUpEye, yUpTarget, yUpUp, true, yUpView);

    float yUpSun[3] = {};
    TransformDirection(inverseFrame, canonicalSun, yUpSun);
    Normalize(yUpSun);

    float yUpPointPosition[3] = {};
    float yUpPointNormal[3] = {};
    TransformPoint(inverseFrame, canonicalPointPosition, yUpPointPosition);
    TransformDirection(inverseFrame, canonicalPointNormal, yUpPointNormal);
    Normalize(yUpPointNormal);
    const float yUpPointRadius = canonicalPointRadius / scale;
    const float yUpFogDensity = canonicalFogDensity * scale;
    float yUpClusterMin[3] = {};
    float yUpClusterMax[3] = {};
    TransformPoint(inverseFrame, canonicalClusterMin, yUpClusterMin);
    TransformPoint(inverseFrame, canonicalClusterMax, yUpClusterMax);

    float yUpParticlePosition[3] = {};
    float yUpParticleRight[3] = {};
    float yUpParticleUp[3] = {};
    TransformPoint(inverseFrame, canonicalParticlePosition, yUpParticlePosition);
    TransformDirection(inverseFrame, canonicalParticleRight, yUpParticleRight);
    TransformDirection(inverseFrame, canonicalParticleUp, yUpParticleUp);
    Normalize(yUpParticleRight);
    Normalize(yUpParticleUp);
    const float yUpParticleSize = canonicalParticleSize / scale;

    QrSpatialConvention mirror = identity;
    mirror.sceneXAxis = QR_AXIS_NEGATIVE_X;
    mirror.hostIsRightHanded = QR_FALSE;

    float mirrorInverse[16] = {};
    MakeMirrorXFrame(scale, mirrorInverse);

    float mirrorPositions[4][3] = {};
    for (int i = 0; i < 4; i++)
    {
        TransformPoint(mirrorInverse, canonicalPositions[i], mirrorPositions[i]);
    }

    QrVertex mirrorVertices[4] = {};
    MakeVertexArray(mirrorPositions, mirrorVertices);

    uint32_t mirrorIndices[6] = {};
    std::memcpy(mirrorIndices, indices, sizeof(mirrorIndices));
    std::swap(mirrorIndices[1], mirrorIndices[2]);
    std::swap(mirrorIndices[4], mirrorIndices[5]);

    float mirrorEye[3] = {};
    float mirrorTarget[3] = {};
    float mirrorUp[3] = {};
    TransformPoint(mirrorInverse, canonicalEye, mirrorEye);
    TransformPoint(mirrorInverse, canonicalTarget, mirrorTarget);
    TransformDirection(mirrorInverse, canonicalUp, mirrorUp);
    Normalize(mirrorUp);

    float mirrorView[16] = {};
    LookAt(mirrorEye, mirrorTarget, mirrorUp, false, mirrorView);

    float mirrorSun[3] = {};
    TransformDirection(mirrorInverse, canonicalSun, mirrorSun);
    Normalize(mirrorSun);

    float mirrorPointPosition[3] = {};
    float mirrorPointNormal[3] = {};
    TransformPoint(mirrorInverse, canonicalPointPosition, mirrorPointPosition);
    TransformDirection(mirrorInverse, canonicalPointNormal, mirrorPointNormal);
    Normalize(mirrorPointNormal);

    const float mirrorPointRadius = canonicalPointRadius / scale;
    const float mirrorFogDensity = canonicalFogDensity * scale;
    float mirrorClusterMin[3] = {};
    float mirrorClusterMax[3] = {};
    TransformPoint(mirrorInverse, canonicalClusterMin, mirrorClusterMin);
    TransformPoint(mirrorInverse, canonicalClusterMax, mirrorClusterMax);

    float mirrorParticlePosition[3] = {};
    float mirrorParticleRight[3] = {};
    float mirrorParticleUp[3] = {};
    TransformPoint(mirrorInverse, canonicalParticlePosition, mirrorParticlePosition);
    TransformDirection(mirrorInverse, canonicalParticleRight, mirrorParticleRight);
    TransformDirection(mirrorInverse, canonicalParticleUp, mirrorParticleUp);
    Normalize(mirrorParticleRight);
    Normalize(mirrorParticleUp);
    const float mirrorParticleSize = canonicalParticleSize / scale;

    RenderResult identityResult;
    RenderResult yUpResult;
    RenderResult mirrorResult;

    {
        TwinRenderer renderer(window, shaderFolder.c_str(), blueNoisePath.c_str());

        if (!renderer.Create() || !renderer.SetConvention(identity) || !renderer.CreateMaterial() ||
            !renderer.UploadScene(canonicalVertices, indices, canonicalSun, canonicalPointPosition,
                                  canonicalPointNormal, canonicalPointRadius,
                                  canonicalClusterMin, canonicalClusterMax,
                                  canonicalParticlePosition, canonicalParticleSize,
                                  canonicalParticleRight, canonicalParticleUp) ||
            !renderer.Render(canonicalView, canonicalEye, canonicalUp, 0.1f, 100.0f, 100.0f,
                             canonicalFogDensity, identityResult))
        {
            DestroyWindow(window);
            return 1;
        }
    }

    {
        TwinRenderer renderer(window, shaderFolder.c_str(), blueNoisePath.c_str());

        if (!renderer.Create() || !renderer.SetConvention(yUp) || !renderer.CreateMaterial() ||
            !renderer.UploadScene(yUpVertices, indices, yUpSun, yUpPointPosition, yUpPointNormal,
                                  yUpPointRadius, yUpClusterMin, yUpClusterMax,
                                  yUpParticlePosition, yUpParticleSize,
                                  yUpParticleRight, yUpParticleUp) ||
            !renderer.Render(yUpView, yUpEye, yUpUp, 0.1f / scale, 100.0f / scale,
                             100.0f / scale, yUpFogDensity, yUpResult))
        {
            DestroyWindow(window);
            return 1;
        }
    }

    {
        TwinRenderer renderer(window, shaderFolder.c_str(), blueNoisePath.c_str());

        if (!renderer.Create() || !renderer.SetConvention(mirror) || !renderer.CreateMaterial() ||
            !renderer.UploadScene(mirrorVertices, mirrorIndices, mirrorSun, mirrorPointPosition,
                                  mirrorPointNormal, mirrorPointRadius,
                                  mirrorClusterMin, mirrorClusterMax,
                                  mirrorParticlePosition, mirrorParticleSize,
                                  mirrorParticleRight, mirrorParticleUp) ||
            !renderer.Render(mirrorView, mirrorEye, mirrorUp, 0.1f / scale, 100.0f / scale,
                             100.0f / scale, mirrorFogDensity, mirrorResult))
        {
            DestroyWindow(window);
            return 1;
        }
    }

    const bool yUpSame = CompareImages(identityResult, yUpResult);
    const bool mirrorSame = CompareImages(identityResult, mirrorResult);

    DestroyWindow(window);
    return yUpSame && mirrorSame ? 0 : 1;
}
