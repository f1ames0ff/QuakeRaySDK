// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

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
    for (int row = 0; row < 3; row++)
    {
        upload.transform.matrix[row][3] = EditorMetersLength(upload.transform.matrix[row][3]);
    }

    if (upload.passThroughType == QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT)
    {
        upload.layerColors[2].data[1] = EditorMetersLength(upload.layerColors[2].data[1]);
    }

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

void EditorMetersParticleBurst(QrParticleBurstInfo &burst)
{
    EditorMetersPoint(burst.origin);
    burst.originJitterRadius = EditorMetersLength(burst.originJitterRadius);
    burst.speedMin = EditorMetersLength(burst.speedMin);
    burst.speedMax = EditorMetersLength(burst.speedMax);
    burst.sizeStartMin = EditorMetersLength(burst.sizeStartMin);
    burst.sizeStartMax = EditorMetersLength(burst.sizeStartMax);
}

void EditorMetersParticleEmitter(QrParticleEmitterInfo &emitter)
{
    EditorMetersParticleBurst(emitter.burst);
}

void EditorMetersVectorFieldVolume(QrVectorFieldVolume &volume)
{
    for (int row = 0; row < 3; row++)
    {
        for (int col = 0; col < 4; col++)
        {
            volume.worldFromLocal.matrix[row][col] =
                EditorMetersLength(volume.worldFromLocal.matrix[row][col]);
        }
    }
    if (volume.kind != QR_VECTOR_FIELD_KIND_LUT_3D)
    {
        volume.intensity = EditorMetersLength(volume.intensity);
    }
    volume.maxSpeed = EditorMetersLength(volume.maxSpeed);
    volume.curlFrequency = EditorMetersPerUnitRate(volume.curlFrequency);
    volume.curlScrollSpeed = EditorMetersLength(volume.curlScrollSpeed);
}

void EditorMetersFgaVectors(std::vector<float> &vectorsXYZ)
{
    for (float &v : vectorsXYZ)
    {
        v = EditorMetersLength(v);
    }
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
