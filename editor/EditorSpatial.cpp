#include "EditorSpatial.h"

#include <cmath>

namespace
{

void MetersPoint(float point[3])
{
    for (int axis = 0; axis < 3; axis++)
    {
        point[axis] = EditorMetersLength(point[axis]);
    }
}

// Radiant power is per square Quake unit, so it shrinks with the square of the length change --
// the same pairing the game's adapter applies to its own light colours.
void MetersRelativePower(QrFloat3D &power)
{
    const float meterArea = (1.0f / kEditorUnitsPerMeter) * (1.0f / kEditorUnitsPerMeter);

    for (float &channel : power.data)
    {
        channel *= meterArea;
    }
}

} // namespace

float EditorMetersLength(float units)
{
    return units / kEditorUnitsPerMeter;
}

float EditorMetersArea(float squareUnits)
{
    return squareUnits / (kEditorUnitsPerMeter * kEditorUnitsPerMeter);
}

float EditorMetersPerUnitRate(float perUnit)
{
    return perUnit * kEditorUnitsPerMeter;
}

void EditorMetersPoint(QrFloat3D &point)
{
    MetersPoint(point.data);
}

void EditorMetersTransmittance(QrFloat3D &color)
{
    // A transmittance over one Quake unit, raised to the length one meter covers: the game's own
    // adapter applies the same 1/metre -> 1/unit pair the other way round.
    for (float &channel : color.data)
    {
        channel = std::pow(channel, kEditorUnitsPerMeter);
    }
}

void EditorMetersGeometry(QrGeometryUploadInfo &upload, std::vector<QrVertex> &scratch)
{
    if (upload.pVertices == nullptr || upload.vertexCount == 0)
    {
        return;
    }

    scratch.assign(upload.pVertices, upload.pVertices + upload.vertexCount);

    for (QrVertex &vertex : scratch)
    {
        MetersPoint(vertex.position);
    }

    upload.pVertices = scratch.data();
}

void EditorMetersWorldLights(QrWorldLightsUploadInfo &upload, std::vector<QrFloat3D> &mins,
                             std::vector<QrFloat3D> &maxs)
{
    if (upload.pClusterMins == nullptr || upload.pClusterMaxs == nullptr ||
        upload.numClusters == 0)
    {
        return;
    }

    mins.assign(upload.pClusterMins, upload.pClusterMins + upload.numClusters);
    maxs.assign(upload.pClusterMaxs, upload.pClusterMaxs + upload.numClusters);

    for (size_t cluster = 0; cluster < mins.size(); cluster++)
    {
        EditorMetersPoint(mins[cluster]);
        EditorMetersPoint(maxs[cluster]);
    }

    upload.pClusterMins = mins.data();
    upload.pClusterMaxs = maxs.data();
}

void EditorMetersSphericalLight(QrSphericalLightUploadInfo &upload)
{
    EditorMetersPoint(upload.position);
    upload.radius = EditorMetersLength(upload.radius);
    MetersRelativePower(upload.color);
}

void EditorMetersSpotLight(QrSpotLightUploadInfo &upload)
{
    EditorMetersPoint(upload.position);
    upload.radius = EditorMetersLength(upload.radius);
    MetersRelativePower(upload.color);
}

void EditorMetersClusterLightSource(QrClusterLightSource &source)
{
    EditorMetersPoint(source.origin);
    source.reach = EditorMetersLength(source.reach);
    source.radius = EditorMetersLength(source.radius);
    // power stays as it is: it ranks a light against the frame's other lights and is the same
    // quantity the game hands over, which its adapter leaves alone too.
}

void EditorMetersAreaLight(QrTexturedAreaLightUploadInfo &upload)
{
    EditorMetersPoint(upload.A);
    EditorMetersPoint(upload.B);
    EditorMetersPoint(upload.C);
    upload.area = EditorMetersArea(upload.area);
}

QrResult EditorDrawFrame(QrInstance instance, const QrDrawFrameInfo *drawInfo)
{
    if (drawInfo == nullptr)
    {
        return qrDrawFrame(instance, drawInfo);
    }

    QrDrawFrameInfo converted = *drawInfo;
    MetersPoint(&converted.view[12]);
    converted.cameraNear = EditorMetersLength(converted.cameraNear);
    converted.cameraFar = EditorMetersLength(converted.cameraFar);
    converted.rayLength = EditorMetersLength(converted.rayLength);

    QrDrawFrameIlluminationParams illumination = {};

    if (drawInfo->pIlluminationParams != nullptr)
    {
        illumination = *drawInfo->pIlluminationParams;
        illumination.cellWorldSize = EditorMetersLength(illumination.cellWorldSize);
        illumination.sunBounceRange = EditorMetersLength(illumination.sunBounceRange);
        converted.pIlluminationParams = &illumination;
    }

    QrDrawFrameVolumetricParams volumetric = {};

    if (drawInfo->pVolumetricParams != nullptr)
    {
        volumetric = *drawInfo->pVolumetricParams;
        volumetric.volumetricFar = EditorMetersLength(volumetric.volumetricFar);
        volumetric.scaterring = EditorMetersPerUnitRate(volumetric.scaterring);
        converted.pVolumetricParams = &volumetric;
    }

    QrDrawFrameLevelFogParams levelFog = {};

    if (drawInfo->pLevelFogParams != nullptr)
    {
        levelFog = *drawInfo->pLevelFogParams;
        levelFog.density = EditorMetersPerUnitRate(levelFog.density);
        converted.pLevelFogParams = &levelFog;
    }

    QrDrawFrameReflectRefractParams reflectRefract = {};

    if (drawInfo->pReflectRefractParams != nullptr)
    {
        reflectRefract = *drawInfo->pReflectRefractParams;
        reflectRefract.waterWaveSpeed = EditorMetersLength(reflectRefract.waterWaveSpeed);
        reflectRefract.waterTextureAreaScale =
            EditorMetersLength(reflectRefract.waterTextureAreaScale);
        EditorMetersTransmittance(reflectRefract.waterColor);
        EditorMetersTransmittance(reflectRefract.acidColor);
        converted.pReflectRefractParams = &reflectRefract;
    }

    QrDrawFrameSkyParams sky = {};

    if (drawInfo->pSkyParams != nullptr)
    {
        sky = *drawInfo->pSkyParams;
        EditorMetersPoint(sky.skyViewerPosition);

        if (sky.skyType == QR_SKY_TYPE_PROCEDURAL)
        {
            // The cloud block rides in the otherwise unused rotation matrix: height and thickness
            // are slots 7 and 8 of the order QrDrawFrameSkyParams documents.
            float *clouds = &sky.skyCubemapRotationTransform.matrix[0][0];
            clouds[7] = EditorMetersLength(clouds[7]);
            clouds[8] = EditorMetersLength(clouds[8]);
        }

        converted.pSkyParams = &sky;
    }

    QrPostEffectsNearDofParams nearDof = {};

    if (drawInfo->postEffectParams.pNearDof != nullptr)
    {
        nearDof = *drawInfo->postEffectParams.pNearDof;
        nearDof.focusDistance = EditorMetersLength(nearDof.focusDistance);
        converted.postEffectParams.pNearDof = &nearDof;
    }

    return qrDrawFrame(instance, &converted);
}
