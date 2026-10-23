#include <qray/qray.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

void MakeMirrorXForwardFrame(float scale, float frame[16])
{
    std::memset(frame, 0, 16 * sizeof(float));

    frame[0] = -scale;
    frame[5] = scale;
    frame[10] = scale;
    frame[15] = 1.0f;
}

void MakeIdentityTransform(QrTransform &transform)
{
    std::memset(&transform, 0, sizeof(transform));
    transform.matrix[0][0] = 1.0f;
    transform.matrix[1][1] = 1.0f;
    transform.matrix[2][2] = 1.0f;
}

void MakeArbitraryCanonicalTransform(QrTransform &transform)
{
    const float angleA = 0.4363323129985824f;
    const float angleB = 0.20943951023931956f;
    const float ca = std::cos(angleA);
    const float sa = std::sin(angleA);
    const float cb = std::cos(angleB);
    const float sb = std::sin(angleB);

    const float rotation[3][3] = {
        { ca, sa * sb, sa * cb },
        { 0.0f, cb, -sb },
        { -sa, ca * sb, ca * cb },
    };
    const float axisScale[3] = { 1.6f, 0.7f, 1.2f };
    const float translation[3] = { 0.5f, -0.75f, 0.25f };

    std::memset(&transform, 0, sizeof(transform));

    for (int row = 0; row < 3; row++)
    {
        for (int column = 0; column < 3; column++)
        {
            transform.matrix[row][column] = rotation[row][column] * axisScale[column];
        }

        transform.matrix[row][3] = translation[row];
    }
}


// The host hands the renderer host world vertices plus a host matrix. The renderer
// traces with (scale * axis) * M_host, where axis is the host->scene linear part, and
// the already converted vertices come from (axis^T / scale) * v_canonical. For the
// canonical placement to survive that chain,
//     M_host = axis^T * M_canonical * axis,   t_host = axis^T * t_canonical / scale.
void MakeHostTransform(const float hostToSceneFrame[16], float hostScale, const QrTransform &canonical,
                       QrTransform &host)
{
    float axis[3][3] = {};

    for (int row = 0; row < 3; row++)
    {
        for (int column = 0; column < 3; column++)
        {
            axis[row][column] = hostToSceneFrame[column * 4 + row] / hostScale;
        }
    }

    std::memset(&host, 0, sizeof(host));

    for (int row = 0; row < 3; row++)
    {
        for (int column = 0; column < 3; column++)
        {
            float sum = 0.0f;

            for (int inner = 0; inner < 3; inner++)
            {
                float innerSum = 0.0f;

                for (int k = 0; k < 3; k++)
                {
                    innerSum += canonical.matrix[inner][k] * axis[k][column];
                }

                sum += axis[inner][row] * innerSum;
            }

            host.matrix[row][column] = sum;
        }

        float sum = 0.0f;

        for (int inner = 0; inner < 3; inner++)
        {
            sum += axis[inner][row] * canonical.matrix[inner][3];
        }

        host.matrix[row][3] = sum / hostScale;
    }
}

struct SkyOptions
{
    bool procedural = false;
    bool clouds = false;
    float cloudHeight = 400.0f;
    float cloudThickness = 120.0f;
};

void MakeVertexArray(const float positions[4][3], QrVertex vertices[4]);

struct SceneOptions
{
    QrTransform transform = {};
    bool opaque = true;
    bool water = false;
    bool acid = false;
    bool ui = false;
    bool particles = true;
    float waterWaveSpeed = 0.05f;
    float waterTextureAreaScale = 0.0025f;
    float acidDensity = 0.6f;
    QrFloat3D waterColor = { { 0.86f, 0.93f, 0.97f } };
    QrFloat3D acidColor = { { 0.81f, 0.98f, 0.77f } };
    // Pins eye adaptation at unit exposure. A twin comparison then measures the
    // scene conversion instead of the histogram of a stochastic frame, whose
    // sample noise shifts the adapted luminance across unit conventions.
    bool pinnedExposure = true;
};

struct SceneVertices
{
    QrVertex opaque[4];
    QrVertex water[4];
    QrVertex acid[4];
    QrFloat3D lightTriangle[3];
};

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

    bool UploadQuad(const QrTransform &transform, uint32_t uniqueID,
                    QrGeometryPassThroughType passThroughType, const QrVertex vertices[4],
                    const uint32_t indices[6])
    {
        QrGeometryUploadInfo geometry = {};
        geometry.uniqueID = uniqueID;
        geometry.flags = QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT;
        geometry.geomType = QR_GEOMETRY_TYPE_STATIC;
        geometry.passThroughType = passThroughType;
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
        geometry.transform = transform;

        return Check(qrUploadGeometry(instance, &geometry), "upload geometry");
    }

    bool UploadScene(const SceneVertices &vertices, const uint32_t indices[6], const SceneOptions &options,
                     const float sunDirection[3], const float pointPosition[3], const float pointNormal[3],
                     float pointRadius, const float pointColor[3], const float clusterMin[3],
                     const float clusterMax[3])
    {
        if (!Check(qrBeginStaticGeometries(instance), "begin static"))
        {
            return false;
        }

        if (options.opaque &&
            !UploadQuad(options.transform, 10, QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE, vertices.opaque,
                        indices))
        {
            return false;
        }

        if (options.water &&
            !UploadQuad(options.transform, 13, QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT,
                        vertices.water, indices))
        {
            return false;
        }

        if (options.acid &&
            !UploadQuad(options.transform, 14, QR_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT,
                        vertices.acid, indices))
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
        point.color = { { pointColor[0], pointColor[1], pointColor[2] } };
        point.position = { pointPosition[0], pointPosition[1], pointPosition[2] };
        point.radius = pointRadius;
        point.normal = { pointNormal[0], pointNormal[1], pointNormal[2] };

        // A polygonal light has no normal field, so its emitted side comes from the
        // vertex order alone; that makes it the one light type the frame conversion
        // has to reorder. It is uploaded unconditionally to keep that conversion on
        // the executed path, but it does not shade this frame: local lights are
        // sampled by the path-traced passes, which the twin skips without a TLAS.
        QrPolygonalLightUploadInfo polygonal = {};
        polygonal.uniqueID = 15;
        polygonal.color = { 2.0f, 1.4f, 0.7f };
        polygonal.positions[0] = vertices.lightTriangle[0];
        polygonal.positions[1] = vertices.lightTriangle[1];
        polygonal.positions[2] = vertices.lightTriangle[2];

        if (!Check(qrUploadDirectionalLight(instance, &sun), "upload sun") ||
            !Check(qrUploadSphericalLight(instance, &point), "upload point light") ||
            !Check(qrUploadPolygonalLight(instance, &polygonal), "upload polygon light") ||
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

        return true;
    }

    // Rasterized geometry and particles are frame-scoped uploads: the renderer drops
    // them outside a started frame, so they have to be re-sent every frame.
    bool UploadOverlays(const SceneOptions &options, const float particlePosition[3], float particleSize,
                        const float particleRight[3], const float particleUp[3])
    {
        if (options.particles)
        {
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
        }

        if (!options.ui)
        {
            return true;
        }

        QrVertex overlayVertices[4] = {};
        const float overlayPositions[4][3] = {
            { -1.0f, 0.4f, 0.5f },
            { -0.3f, 0.4f, 0.5f },
            { -0.3f, 1.0f, 0.5f },
            { -1.0f, 1.0f, 0.5f },
        };
        MakeVertexArray(overlayPositions, overlayVertices);

        // The swapchain overlay is drawn in clip space, so the spatial convention
        // deliberately does not touch it: identity view-projection for every host.
        const float clipSpace[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        const uint32_t overlayIndices[6] = { 0, 1, 2, 0, 2, 3 };

        QrRasterizedGeometryUploadInfo overlay = {};
        overlay.renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN;
        overlay.vertexCount = 4;
        overlay.pVertices = overlayVertices;
        overlay.indexCount = 6;
        overlay.pIndices = overlayIndices;
        MakeIdentityTransform(overlay.transform);
        overlay.color = { 1.0f, 0.15f, 0.15f, 1.0f };
        overlay.material = material;
        overlay.pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;

        return Check(qrUploadRasterizedGeometry(instance, &overlay, clipSpace, nullptr), "upload overlay");
    }

    bool Render(const SkyOptions &skyOptions, const SceneOptions &options, const float view[16],
                const float eye[3], const float up[3], float nearPlane, float farPlane, float rayLength,
                float fogDensity, const float particlePosition[3], float particleSize,
                const float particleRight[3], const float particleUp[3], RenderResult &result)
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
        sky.skyType = skyOptions.procedural ? QR_SKY_TYPE_PROCEDURAL : QR_SKY_TYPE_COLOR;
        sky.skyColorDefault = { 0.02f, 0.03f, 0.05f };
        sky.skyColorMultiplier = 1.0f;
        sky.skyColorSaturation = 1.0f;
        sky.sunDiscColor = { 1.0f, 0.95f, 0.85f };
        sky.sunDiscSize = 0.5f;
        sky.skyAmbientLod = 0.0f;
        sky.skyLightMultiplier = 1.0f;
        sky.skyNee = QR_TRUE;

        if (skyOptions.procedural)
        {
            float *clouds = &sky.skyCubemapRotationTransform.matrix[0][0];
            clouds[0] = 1.0f;
            clouds[1] = 1.0f;
            clouds[2] = 1.0f;
            clouds[3] = 0.5f;
            clouds[4] = 0.5f;
            clouds[5] = 0.02f;
            clouds[6] = skyOptions.clouds ? 1.0f : 0.0f;
            clouds[7] = skyOptions.cloudHeight;
            clouds[8] = skyOptions.cloudThickness;
            sky.skyCloudsQuality = 1;
        }

        QrDrawFrameReflectRefractParams reflectRefract = {};
        reflectRefract.maxReflectRefractDepth = 1;
        reflectRefract.indexOfRefractionWater = 1.33f;
        reflectRefract.waterWaveSpeed = options.waterWaveSpeed;
        reflectRefract.waterWaveNormalStrength = 1.0f;
        reflectRefract.waterWaveTextureDerivativesMultiplier = 1.0f;
        reflectRefract.waterTextureAreaScale = options.waterTextureAreaScale;
        reflectRefract.waterColor = options.waterColor;
        reflectRefract.acidColor = options.acidColor;
        reflectRefract.acidDensity = options.acidDensity;

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
        tonemapping.exposureHighPercentile = 100.0f;
        tonemapping.minAdaptedLuminance = options.pinnedExposure ? 0.18f : 0.001f;
        tonemapping.maxAdaptedLuminance = options.pinnedExposure ? 0.18f : 100.0f;
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
        draw.pReflectRefractParams = &reflectRefract;
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

            if (!UploadOverlays(options, particlePosition, particleSize, particleRight, particleUp))
            {
                return false;
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

void MakeIdentityFrame(float frame[16])
{
    std::memset(frame, 0, 16 * sizeof(float));
    frame[0] = 1.0f;
    frame[5] = 1.0f;
    frame[10] = 1.0f;
    frame[15] = 1.0f;
}

void MakeSceneVertices(const float canonicalToHost[16], SceneVertices &vertices)
{
    const float opaque[4][3] = {
        { -1.0f, -2.0f, 0.0f },
        { 3.0f, -2.0f, 0.0f },
        { 3.0f, 2.0f, 0.0f },
        { -1.0f, 2.0f, 0.0f },
    };
    const float water[4][3] = {
        { -3.0f, -3.0f, -1.2f },
        { 3.0f, -3.0f, -1.2f },
        { 3.0f, 3.0f, -1.2f },
        { -3.0f, 3.0f, -1.2f },
    };
    const float acid[4][3] = {
        { -1.5f, 2.6f, -1.0f },
        { 1.5f, 2.6f, -1.0f },
        { 1.5f, 2.6f, 2.0f },
        { -1.5f, 2.6f, 2.0f },
    };

    const float(*sources[3])[3] = { opaque, water, acid };
    QrVertex *targets[3] = { vertices.opaque, vertices.water, vertices.acid };

    for (int quad = 0; quad < 3; quad++)
    {
        float positions[4][3] = {};

        for (int i = 0; i < 4; i++)
        {
            TransformPoint(canonicalToHost, sources[quad][i], positions[i]);
        }

        MakeVertexArray(positions, targets[quad]);
    }

    const float light[3][3] = {
        { 0.5f, 0.0f, 1.5f },
        { 0.5f, 1.0f, 1.5f },
        { 1.5f, 0.0f, 1.5f },
    };

    for (int i = 0; i < 3; i++)
    {
        float position[3] = {};
        TransformPoint(canonicalToHost, light[i], position);
        vertices.lightTriangle[i] = { { position[0], position[1], position[2] } };
    }
}

struct ImageDifference
{
    uint32_t maxDifference = 0;
    double meanDifference = 0.0;
    uint32_t changedPixels = 0;
    uint32_t minX = 0;
    uint32_t minY = 0;
    uint32_t maxX = 0;
    uint32_t maxY = 0;
    uint32_t percentile = 0;
    double meanA = 0.0;
    double meanB = 0.0;
};

bool MeasureDifference(const RenderResult &a, const RenderResult &b, ImageDifference &result)
{
    if (a.width != b.width || a.height != b.height || a.rowPitch != b.rowPitch ||
        a.pixels.size() != b.pixels.size())
    {
        std::fprintf(stderr, "image extent mismatch\n");
        return false;
    }

    uint32_t maxDifference = 0;
    uint64_t totalDifference = 0;
    uint32_t changedPixels = 0;
    uint32_t minX = a.width;
    uint32_t minY = a.height;
    uint32_t maxX = 0;
    uint32_t maxY = 0;
    uint64_t histogram[256] = {};
    uint64_t sumA = 0;
    uint64_t sumB = 0;

    for (size_t pixel = 0; pixel + 3 < a.pixels.size(); pixel += 4)
    {
        bool changed = false;

        for (size_t channel = 0; channel < 4; channel++)
        {
            const uint32_t valueA = a.pixels[pixel + channel];
            const uint32_t valueB = b.pixels[pixel + channel];
            const uint32_t difference = uint32_t(std::abs(int(valueA) - int(valueB)));
            maxDifference = std::max(maxDifference, difference);
            totalDifference += difference;
            histogram[std::min(difference, 255u)]++;
            sumA += valueA;
            sumB += valueB;
            changed = changed || difference != 0;
        }

        if (changed)
        {
            const uint32_t x = uint32_t((pixel % a.rowPitch) / 4);
            const uint32_t y = uint32_t(pixel / a.rowPitch);
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }

        changedPixels += changed ? 1u : 0u;
    }

    uint32_t percentile = 0;

    if (!a.pixels.empty())
    {
        const uint64_t total = uint64_t(a.pixels.size());
        const uint64_t target = (total * 995 + 999) / 1000;
        uint64_t accumulated = 0;

        for (uint32_t bin = 0; bin < 256; bin++)
        {
            accumulated += histogram[bin];

            if (accumulated >= target)
            {
                percentile = bin;
                break;
            }
        }
    }

    result.maxDifference = maxDifference;
    result.meanDifference = double(totalDifference) / double(a.pixels.size());
    result.changedPixels = changedPixels;
    result.minX = changedPixels != 0 ? minX : 0;
    result.minY = changedPixels != 0 ? minY : 0;
    result.maxX = changedPixels != 0 ? maxX : 0;
    result.maxY = changedPixels != 0 ? maxY : 0;
    result.percentile = percentile;
    result.meanA = a.pixels.empty() ? 0.0 : double(sumA) / double(a.pixels.size());
    result.meanB = b.pixels.empty() ? 0.0 : double(sumB) / double(b.pixels.size());
    return true;
}

bool CompareImages(const char *what, const RenderResult &a, const RenderResult &b)
{
    ImageDifference difference = {};

    if (!MeasureDifference(a, b, difference))
    {
        return false;
    }

    // A failing pair is hard to reason about from summary numbers alone, so an
    // opted-in dump writes both frames for offline inspection.
    if (const char *dumpPrefix = std::getenv("QRAY_TWIN_DUMP"))
    {
        auto write = [&](const char *side, const RenderResult &image) {
            char path[512] = {};
            std::snprintf(path, sizeof(path), "%s_%s_%s.raw", dumpPrefix, what, side);
            FILE *file = std::fopen(path, "wb");

            if (file == nullptr)
            {
                return;
            }

            const uint32_t header[3] = { image.width, image.height, image.rowPitch };
            std::fwrite(header, sizeof(header), 1, file);

            for (uint32_t y = 0; y < image.height; y++)
            {
                std::fwrite(image.pixels.data() + size_t(y) * image.rowPitch, 1,
                            size_t(image.width) * 4, file);
            }

            std::fclose(file);
        };

        write("a", a);
        write("b", b);
    }

    std::printf("%s: max=%u mean=%.4f p99.5=%u gain=%.4f bbox=%u,%u-%u,%u changed=%u\n", what,
                difference.maxDifference, difference.meanDifference, difference.percentile,
                difference.meanA > 0.0 ? difference.meanB / difference.meanA : 1.0, difference.minX,
                difference.minY, difference.maxX, difference.maxY, difference.changedPixels);
    {
        uint64_t sumA = 0;
        uint64_t sumB = 0;
        if (!a.pixels.empty() && !b.pixels.empty())
        {
            for (size_t i = 0; i < a.pixels.size() && i < b.pixels.size(); i++)
            {
                sumA = sumA * 1315423911ull + a.pixels[i];
                sumB = sumB * 1315423911ull + b.pixels[i];
            }
        }
        std::printf("%s: hash-a=%016llx hash-b=%016llx\n", what, (unsigned long long)sumA,
                    (unsigned long long)sumB);
    }

    // A twin pair is the same world expressed through two host conventions, so the
    // canonical scene reaches the GPU with different float rounding and the renderer
    // is stochastic: the tonemapped frames differ by a few levels even when the
    // conversion is exact. The gate therefore bounds the shape of the difference
    // instead of demanding identical bytes - an unchanged mean with no global gain
    // shift rejects a flipped winding, a wrong light direction or a mirrored UV set,
    // while a tight percentile keeps a localized structural error from hiding in it.
    const double gain = difference.meanA > 0.0 ? difference.meanB / difference.meanA : 1.0;
    const bool same = difference.meanDifference <= 2.5 && difference.percentile <= 16 &&
                      std::abs(gain - 1.0) <= 0.01;

    if (!same)
    {
        std::fprintf(stderr, "%s diverged\n", what);
        return false;
    }

    return true;
}

// A feature that quietly stops contributing would make every equivalence check
// pass on an image it never reached, so each one is compared against the same
// scene with that feature switched off.
bool RequireContribution(const char *what, const RenderResult &withFeature, const RenderResult &withoutFeature)
{
    ImageDifference difference = {};

    if (!MeasureDifference(withFeature, withoutFeature, difference))
    {
        return false;
    }

    std::printf("%s contribution: pixels=%u max=%u\n", what, difference.changedPixels,
                difference.maxDifference);

    if (difference.changedPixels < 64)
    {
        std::fprintf(stderr, "%s does not reach the frame (%u changed pixels)\n", what,
                     difference.changedPixels);
        return false;
    }

    return true;
}

}

bool RunPass(HWND window, const std::string &shaderFolder, const std::string &blueNoisePath,
             const QrSpatialConvention &convention, const SceneVertices &vertices,
             const uint32_t indices[6], const SceneOptions &options, const SkyOptions &sky,
             const float view[16], const float eye[3], const float up[3], float nearPlane,
             float farPlane, float rayLength, float fogDensity, const float sun[3],
             const float pointPosition[3], const float pointNormal[3], float pointRadius,
             const float pointColor[3], const float clusterMin[3], const float clusterMax[3],
             const float particlePosition[3], float particleSize, const float particleRight[3],
             const float particleUp[3], RenderResult &result)
{
    TwinRenderer renderer(window, shaderFolder.c_str(), blueNoisePath.c_str());

    if (!renderer.Create())
    {
        return false;
    }

    if (!renderer.SetConvention(convention))
    {
        return false;
    }

    if (!renderer.CreateMaterial())
    {
        return false;
    }

    if (!renderer.UploadScene(vertices, indices, options, sun, pointPosition, pointNormal, pointRadius,
                              pointColor, clusterMin, clusterMax))
    {
        return false;
    }

    return renderer.Render(sky, options, view, eye, up, nearPlane, farPlane, rayLength, fogDensity,
                           particlePosition, particleSize, particleRight, particleUp, result);
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

    float identityFrame[16] = {};
    MakeIdentityFrame(identityFrame);

    SceneVertices identityVertices = {};
    MakeSceneVertices(identityFrame, identityVertices);

    QrTransform canonicalGeometryTransform = {};
    MakeArbitraryCanonicalTransform(canonicalGeometryTransform);


    const float canonicalWaterWaveSpeed = 0.05f;
    const float canonicalWaterTextureAreaScale = 0.0025f;
    const float canonicalWaterColor[3] = { 0.86f, 0.93f, 0.97f };
    const float canonicalAcidColor[3] = { 0.81f, 0.98f, 0.77f };
    const float canonicalAcidDensity = 0.6f;

    // Everything the reflect/refract and sky paths take is host-space. A host
    // declares the same artistic values in its own units, so the twin has to
    // derive them per convention exactly like a real host would.
    auto makeSceneOptions = [&](const float hostFrame[16], float hostScale,
                                const QrTransform &canonicalTransform) {
        SceneOptions options = {};
        MakeHostTransform(hostFrame, hostScale, canonicalTransform, options.transform);
        options.water = true;
        options.acid = true;
        options.ui = true;
        // Both are lengths in host units: the renderer multiplies them by the
        // frame scale, so the host must not pre-divide them.
        options.waterWaveSpeed = canonicalWaterWaveSpeed;
        options.waterTextureAreaScale = canonicalWaterTextureAreaScale;
        // Colours are transmittance per host unit, so the host declares the same
        // artistic value it would use in its own units; the renderer raises them
        // to the scene unit through SpatialFrame::Transmittance.
        options.waterColor = { { canonicalWaterColor[0], canonicalWaterColor[1],
                                 canonicalWaterColor[2] } };
        options.acidColor = { { canonicalAcidColor[0], canonicalAcidColor[1],
                                canonicalAcidColor[2] } };
        options.acidDensity = canonicalAcidDensity;
        return options;
    };

    auto makeSkyOptions = [](bool clouds, float hostScale) {
        SkyOptions sky = {};
        sky.procedural = true;
        sky.clouds = clouds;
        sky.cloudHeight = 400.0f / hostScale;
        sky.cloudThickness = 120.0f / hostScale;
        return sky;
    };

    const float canonicalEye[3] = { 0.0f, -5.0f, 3.0f };
    const float canonicalTarget[3] = { 0.0f, 0.0f, 0.0f };
    const float canonicalUp[3] = { 0.0f, 0.0f, 1.0f };
    const float canonicalSun[3] = { 0.2f, 0.3f, -1.0f };
    const float canonicalPointPosition[3] = { 0.0f, -2.0f, 4.0f };
    const float canonicalPointNormal[3] = { 0.0f, 0.0f, 1.0f };
    const float canonicalPointRadius = 2.0f;
    // Relative power: the renderer multiplies spherical/spot/polygonal light
    // color by the frame scale squared, so a host with its own units declares
    // the same scene power by dividing by that factor.
    const float canonicalPointColor[3] = { 20.0f, 12.0f, 8.0f };
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
    const float inverseArea = 1.0f / (scale * scale);
    float frame[16] = {};
    float inverseFrame[16] = {};
    MakeYUpFrame(scale, frame, inverseFrame);

    SceneVertices yUpVertices = {};
    MakeSceneVertices(inverseFrame, yUpVertices);

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
    const float yUpPointColor[3] = {
        canonicalPointColor[0] * inverseArea,
        canonicalPointColor[1] * inverseArea,
        canonicalPointColor[2] * inverseArea,
    };
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

    float mirrorFrame[16] = {};
    MakeMirrorXForwardFrame(scale, mirrorFrame);

    SceneVertices mirrorVertices = {};
    MakeSceneVertices(mirrorInverse, mirrorVertices);

    // Reflecting a position set reverses the orientation of every triangle, so a
    // host that mirrors its world also reverses each triangle's index order: that
    // is what keeps its own front faces front faces. The SDK undoes the
    // reflection for its right-handed scene, so the mirrored pair must match the
    // identity frame exactly.
    uint32_t mirrorIndices[6] = {};

    for (int index = 0; index + 2 < 6; index += 3)
    {
        mirrorIndices[index] = indices[index];
        mirrorIndices[index + 1] = indices[index + 2];
        mirrorIndices[index + 2] = indices[index + 1];
    }

    // The polygonal light obeys the same rule: its emitted side is the authored
    // vertex order, which a reflected copy has to reverse as well.
    std::swap(mirrorVertices.lightTriangle[1], mirrorVertices.lightTriangle[2]);

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
    const float mirrorPointColor[3] = {
        canonicalPointColor[0] * inverseArea,
        canonicalPointColor[1] * inverseArea,
        canonicalPointColor[2] * inverseArea,
    };
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

    SceneOptions identityOptions = makeSceneOptions(identityFrame, 1.0f, canonicalGeometryTransform);
    SceneOptions yUpOptions = makeSceneOptions(frame, scale, canonicalGeometryTransform);
    SceneOptions mirrorOptions = makeSceneOptions(mirrorFrame, scale, canonicalGeometryTransform);

    SkyOptions identitySky = makeSkyOptions(true, 1.0f);
    SkyOptions yUpSky = makeSkyOptions(true, scale);
    SkyOptions mirrorSky = makeSkyOptions(true, scale);

    auto runIdentity = [&](const SceneOptions &options, const SkyOptions &sky, RenderResult &result) {
        return RunPass(window, shaderFolder, blueNoisePath, identity, identityVertices, indices, options,
                       sky, canonicalView, canonicalEye, canonicalUp, 0.1f, 100.0f, 100.0f,
                       canonicalFogDensity, canonicalSun, canonicalPointPosition, canonicalPointNormal,
                       canonicalPointRadius, canonicalPointColor, canonicalClusterMin, canonicalClusterMax,
                       canonicalParticlePosition, canonicalParticleSize, canonicalParticleRight,
                       canonicalParticleUp, result);
    };

    RenderResult identityResult;
    RenderResult yUpResult;
    RenderResult mirrorResult;

    if (!runIdentity(identityOptions, identitySky, identityResult))
    {
        DestroyWindow(window);
        return 1;
    }

    auto runYUp = [&](const SceneOptions &options, const SkyOptions &sky, RenderResult &result) {
        return RunPass(window, shaderFolder, blueNoisePath, yUp, yUpVertices, indices, options, sky,
                       yUpView, yUpEye, yUpUp, 0.1f / scale, 100.0f / scale, 100.0f / scale,
                       yUpFogDensity, yUpSun, yUpPointPosition, yUpPointNormal, yUpPointRadius,
                       yUpPointColor, yUpClusterMin, yUpClusterMax, yUpParticlePosition,
                       yUpParticleSize, yUpParticleRight, yUpParticleUp, result);
    };

    auto runMirror = [&](const SceneOptions &options, const SkyOptions &sky, RenderResult &result) {
        return RunPass(window, shaderFolder, blueNoisePath, mirror, mirrorVertices, mirrorIndices, options,
                       sky, mirrorView, mirrorEye, mirrorUp, 0.1f / scale, 100.0f / scale,
                       100.0f / scale, mirrorFogDensity, mirrorSun, mirrorPointPosition,
                       mirrorPointNormal, mirrorPointRadius, mirrorPointColor, mirrorClusterMin,
                       mirrorClusterMax, mirrorParticlePosition, mirrorParticleSize, mirrorParticleRight,
                       mirrorParticleUp, result);
    };

    if (!runYUp(yUpOptions, yUpSky, yUpResult))
    {
        DestroyWindow(window);
        return 1;
    }

    if (!runMirror(mirrorOptions, mirrorSky, mirrorResult))
    {
        DestroyWindow(window);
        return 1;
    }

    // Equivalence over the identity scene already covers every enabled feature
    // (clouds, water, acid, particles, UI). The gates below prove each optional
    // path actually reaches the frame, so the equivalence is not vacuous.
    SceneOptions noWater = identityOptions;
    noWater.water = false;

    SceneOptions noAcid = identityOptions;
    noAcid.acid = false;

    SceneOptions noUi = identityOptions;
    noUi.ui = false;

    SceneOptions emptyScene = identityOptions;
    emptyScene.opaque = false;
    emptyScene.water = false;
    emptyScene.acid = false;
    emptyScene.ui = false;
    emptyScene.particles = false;


    const SkyOptions noClouds = makeSkyOptions(false, 1.0f);

    RenderResult noWaterResult;
    RenderResult noAcidResult;
    RenderResult noUiResult;
    RenderResult noCloudsResult;
    RenderResult emptySceneResult;

    bool gates = true;

    if (!runIdentity(noWater, identitySky, noWaterResult) ||
        !runIdentity(noAcid, identitySky, noAcidResult) ||
        !runIdentity(noUi, identitySky, noUiResult) ||
        !runIdentity(identityOptions, noClouds, noCloudsResult) ||
        !runIdentity(emptyScene, noClouds, emptySceneResult))
    {
        DestroyWindow(window);
        return 1;
    }

    gates = RequireContribution("clouds", identityResult, noCloudsResult) && gates;
    gates = RequireContribution("water", identityResult, noWaterResult) && gates;
    gates = RequireContribution("acid", identityResult, noAcidResult) && gates;
    gates = RequireContribution("ui", identityResult, noUiResult) && gates;
    gates = RequireContribution("geometry", identityResult, emptySceneResult) && gates;


    const bool yUpSame = CompareImages("y-up-meters", identityResult, yUpResult);
    const bool mirrorSame = CompareImages("mirrored", identityResult, mirrorResult);

    // A mirrored host flips the frame handedness, so every feature that carries its
    // own orientation has to be flipped with it. Naming the offending subsystem is
    // the difference between a usable failure and a bare image diff, so on a
    // mismatched mirror the pair is re-compared with each optional feature removed.
    if (!mirrorSame)
    {
        auto probeMirror = [&](const char *name, bool particles, bool water, bool acid, bool ui) {
            SceneOptions identityVariant = identityOptions;
            identityVariant.particles = particles;
            identityVariant.water = water;
            identityVariant.acid = acid;
            identityVariant.ui = ui;

            SceneOptions mirrorVariant = mirrorOptions;
            mirrorVariant.particles = particles;
            mirrorVariant.water = water;
            mirrorVariant.acid = acid;
            mirrorVariant.ui = ui;

            RenderResult identityProbe;
            RenderResult mirrorProbe;

            if (runIdentity(identityVariant, identitySky, identityProbe) &&
                runMirror(mirrorVariant, mirrorSky, mirrorProbe))
            {
                CompareImages(name, identityProbe, mirrorProbe);
            }
        };

        probeMirror("mirror-without-particles", false, true, true, true);
        probeMirror("mirror-without-water", true, false, true, true);
        probeMirror("mirror-without-acid", true, true, false, true);
        probeMirror("mirror-without-ui", true, true, true, false);
        probeMirror("mirror-geometry-only", false, false, false, false);
    }

    DestroyWindow(window);
    return yUpSame && mirrorSame && gates ? 0 : 1;
}
