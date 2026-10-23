#include "SpatialFrame.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qray
{

namespace
{

constexpr float kInternalSceneUnitsPerMeter = 40.0f;

int AxisIndex(QrAxis axis)
{
    return int(axis) % 3;
}

float AxisSign(QrAxis axis)
{
    return int(axis) < 3 ? 1.0f : -1.0f;
}

void MultiplyColumnMajor(const float *a, const float *b, float *out)
{
    for (int column = 0; column < 4; column++)
    {
        for (int row = 0; row < 4; row++)
        {
            float value = 0.0f;

            for (int k = 0; k < 4; k++)
            {
                value += a[k * 4 + row] * b[column * 4 + k];
            }

            out[column * 4 + row] = value;
        }
    }
}

}

SpatialFrame::SpatialFrame()
    : hostToScene{
          { 1.0f, 0.0f, 0.0f },
          { 0.0f, 1.0f, 0.0f },
          { 0.0f, 0.0f, 1.0f },
      }
    , sceneToHost{
          { 1.0f, 0.0f, 0.0f },
          { 0.0f, 1.0f, 0.0f },
          { 0.0f, 0.0f, 1.0f },
      }
    , scale(kInternalSceneUnitsPerMeter)
    , invScale(1.0f / kInternalSceneUnitsPerMeter)
    , frameDeterminant(1.0f)
    , identity(false)
    , mirrored(false)
{
}

bool SpatialFrame::Configure(const QrSpatialConvention &convention)
{
    if (convention.size != sizeof(QrSpatialConvention) ||
        convention.version != QR_SPATIAL_CONVENTION_VERSION ||
        convention.hostIsRightHanded > QR_TRUE ||
        convention.reserved0 != 0 ||
        convention.reserved[0] != 0 ||
        convention.reserved[1] != 0 ||
        convention.reserved[2] != 0 ||
        convention.reserved[3] != 0)
    {
        return false;
    }

    const QrAxis axes[3] = { convention.sceneXAxis, convention.sceneYAxis, convention.sceneZAxis };
    bool used[3] = {};
    float candidate[3][3] = {};

    for (int canonicalAxis = 0; canonicalAxis < 3; canonicalAxis++)
    {
        const QrAxis axis = axes[canonicalAxis];

        if (int(axis) < 0 || int(axis) > 5)
        {
            return false;
        }

        const int hostAxis = AxisIndex(axis);

        if (used[hostAxis])
        {
            return false;
        }

        used[hostAxis] = true;
        candidate[canonicalAxis][hostAxis] = AxisSign(axis);
    }

    const float determinant =
        candidate[0][0] * (candidate[1][1] * candidate[2][2] - candidate[1][2] * candidate[2][1]) -
        candidate[0][1] * (candidate[1][0] * candidate[2][2] - candidate[1][2] * candidate[2][0]) +
        candidate[0][2] * (candidate[1][0] * candidate[2][1] - candidate[1][1] * candidate[2][0]);

    if (std::abs(determinant) < 0.5f)
    {
        return false;
    }

    const bool mirrored = determinant < 0.0f;

    if ((convention.hostIsRightHanded == QR_TRUE) == mirrored)
    {
        return false;
    }

    const float candidateScale = kInternalSceneUnitsPerMeter;
    const float candidateInvScale = 1.0f / candidateScale;

    if (!std::isfinite(candidateInvScale))
    {
        return false;
    }
    bool candidateIdentity = candidateScale == 1.0f;

    for (int row = 0; row < 3 && candidateIdentity; row++)
    {
        for (int column = 0; column < 3; column++)
        {
            const float expected = row == column ? 1.0f : 0.0f;

            if (candidate[row][column] != expected)
            {
                candidateIdentity = false;
                break;
            }
        }
    }

    for (int sceneAxis = 0; sceneAxis < 3; sceneAxis++)
    {
        for (int hostAxis = 0; hostAxis < 3; hostAxis++)
        {
            hostToScene[sceneAxis][hostAxis] = candidate[sceneAxis][hostAxis];
            sceneToHost[hostAxis][sceneAxis] = candidate[sceneAxis][hostAxis];
        }
    }

    scale = candidateScale;
    invScale = candidateInvScale;
    frameDeterminant = determinant;
    identity = candidateIdentity;
    this->mirrored = mirrored;

    return true;
}

QrFloat3D SpatialFrame::Point(const QrFloat3D &value) const
{
    return { {
        scale * (hostToScene[0][0] * value.data[0] + hostToScene[0][1] * value.data[1] + hostToScene[0][2] * value.data[2]),
        scale * (hostToScene[1][0] * value.data[0] + hostToScene[1][1] * value.data[1] + hostToScene[1][2] * value.data[2]),
        scale * (hostToScene[2][0] * value.data[0] + hostToScene[2][1] * value.data[1] + hostToScene[2][2] * value.data[2]),
    } };
}

QrFloat3D SpatialFrame::Direction(const QrFloat3D &value) const
{
    return { {
        hostToScene[0][0] * value.data[0] + hostToScene[0][1] * value.data[1] + hostToScene[0][2] * value.data[2],
        hostToScene[1][0] * value.data[0] + hostToScene[1][1] * value.data[1] + hostToScene[1][2] * value.data[2],
        hostToScene[2][0] * value.data[0] + hostToScene[2][1] * value.data[1] + hostToScene[2][2] * value.data[2],
    } };
}

float SpatialFrame::Length(float value) const
{
    return value * scale;
}

float SpatialFrame::Area(float value) const
{
    return value * scale * scale;
}

float SpatialFrame::Transmittance(float value) const
{
    return std::max(std::pow(std::max(value, 0.0f), invScale), 0.000001f);
}

QrTransform SpatialFrame::Transform(const QrTransform &value) const
{
    QrTransform result = {};

    for (int row = 0; row < 3; row++)
    {
        for (int column = 0; column < 4; column++)
        {
            result.matrix[row][column] =
                scale * (hostToScene[row][0] * value.matrix[0][column] +
                         hostToScene[row][1] * value.matrix[1][column] +
                         hostToScene[row][2] * value.matrix[2][column]);
        }
    }

    return result;
}

QrGeometryUploadInfo SpatialFrame::Geometry(const QrGeometryUploadInfo &value) const
{
    QrGeometryUploadInfo result = value;
    result.transform = Transform(value.transform);

    if (value.passThroughType == QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT)
    {
        result.layerColors[2].data[1] = Length(value.layerColors[2].data[1]);
    }

    return result;
}

bool SpatialFrame::ReversesWinding(const QrTransform &value) const
{
    const float a = value.matrix[0][0];
    const float b = value.matrix[0][1];
    const float c = value.matrix[0][2];
    const float d = value.matrix[1][0];
    const float e = value.matrix[1][1];
    const float f = value.matrix[1][2];
    const float g = value.matrix[2][0];
    const float h = value.matrix[2][1];
    const float i = value.matrix[2][2];

    const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);

    return determinant * frameDeterminant < 0.0f;
}

QrMatrix3D SpatialFrame::CubemapRotation(const QrMatrix3D &value) const
{
    QrMatrix3D result = {};

    for (int row = 0; row < 3; row++)
    {
        for (int column = 0; column < 3; column++)
        {
            result.matrix[row][column] =
                value.matrix[row][0] * hostToScene[column][0] +
                value.matrix[row][1] * hostToScene[column][1] +
                value.matrix[row][2] * hostToScene[column][2];
        }
    }

    return result;
}

void SpatialFrame::Aabb(const QrFloat3D &minValue, const QrFloat3D &maxValue,
                        QrFloat3D &outMin, QrFloat3D &outMax) const
{
    for (int sceneAxis = 0; sceneAxis < 3; sceneAxis++)
    {
        int hostAxis = 0;
        float sign = 1.0f;

        for (int candidate = 0; candidate < 3; candidate++)
        {
            if (hostToScene[sceneAxis][candidate] != 0.0f)
            {
                hostAxis = candidate;
                sign = hostToScene[sceneAxis][candidate];
                break;
            }
        }

        if (sign > 0.0f)
        {
            outMin.data[sceneAxis] = scale * minValue.data[hostAxis];
            outMax.data[sceneAxis] = scale * maxValue.data[hostAxis];
        }
        else
        {
            outMin.data[sceneAxis] = scale * -maxValue.data[hostAxis];
            outMax.data[sceneAxis] = scale * -minValue.data[hostAxis];
        }
    }
}

uint32_t SpatialFrame::SoftFace(uint32_t value) const
{
    if (value < 1 || value > 6)
    {
        return value;
    }

    const uint32_t hostAxis = (value - 1) / 2;
    const bool hostMax = (value & 1) == 0;

    for (uint32_t sceneAxis = 0; sceneAxis < 3; sceneAxis++)
    {
        if (hostToScene[sceneAxis][hostAxis] != 0.0f)
        {
            return 1 + sceneAxis * 2 + (hostMax ? 1 : 0);
        }
    }

    return value;
}

void SpatialFrame::View(const float source[16], float destination[16]) const
{
    for (int row = 0; row < 3; row++)
    {
        for (int column = 0; column < 3; column++)
        {
            destination[column * 4 + row] =
                source[0 * 4 + row] * hostToScene[column][0] +
                source[1 * 4 + row] * hostToScene[column][1] +
                source[2 * 4 + row] * hostToScene[column][2];
        }

        destination[12 + row] = scale * source[12 + row];
    }

    destination[3] = 0.0f;
    destination[7] = 0.0f;
    destination[11] = 0.0f;
    destination[15] = 1.0f;
}

void SpatialFrame::ViewProjection(const float source[16], float destination[16]) const
{
    float inverseFrame[16] = {};

    for (int row = 0; row < 3; row++)
    {
        for (int column = 0; column < 3; column++)
        {
            inverseFrame[column * 4 + row] = invScale * hostToScene[column][row];
        }
    }

    inverseFrame[15] = 1.0f;

    MultiplyColumnMajor(source, inverseFrame, destination);
}

void ConvertDrawFrame(const SpatialFrame &frame, const QrDrawFrameInfo &source,
                      SpatialFrameDrawStorage &storage)
{
    storage.info = source;

    frame.View(source.view, storage.info.view);
    storage.info.cameraNear = frame.Length(source.cameraNear);
    storage.info.cameraFar = frame.Length(source.cameraFar);
    storage.info.rayLength = frame.Length(source.rayLength);

    storage.info.worldUpVector = { { 0.0f, 0.0f, 1.0f } };

    if (source.pIlluminationParams != nullptr)
    {
        storage.illumination = *source.pIlluminationParams;
        storage.illumination.cellWorldSize = frame.Length(storage.illumination.cellWorldSize);
        storage.illumination.sunBounceRange = frame.Length(storage.illumination.sunBounceRange);
        storage.info.pIlluminationParams = &storage.illumination;
    }

    if (source.pVolumetricParams != nullptr)
    {
        storage.volumetric = *source.pVolumetricParams;
        storage.volumetric.volumetricFar = frame.Length(storage.volumetric.volumetricFar);
        storage.volumetric.sourceDirection = frame.Direction(storage.volumetric.sourceDirection);
        storage.volumetric.scaterring *= frame.InvScale();
        storage.info.pVolumetricParams = &storage.volumetric;
    }

    if (source.pLevelFogParams != nullptr)
    {
        storage.levelFog = *source.pLevelFogParams;
        storage.levelFog.density *= frame.InvScale();
        storage.info.pLevelFogParams = &storage.levelFog;
    }

    if (source.pReflectRefractParams != nullptr)
    {
        storage.reflectRefract = *source.pReflectRefractParams;
        storage.reflectRefract.waterWaveSpeed *= frame.Scale();
        storage.reflectRefract.waterTextureAreaScale *= frame.Scale();
        storage.reflectRefract.waterColor.data[0] = frame.Transmittance(storage.reflectRefract.waterColor.data[0]);
        storage.reflectRefract.waterColor.data[1] = frame.Transmittance(storage.reflectRefract.waterColor.data[1]);
        storage.reflectRefract.waterColor.data[2] = frame.Transmittance(storage.reflectRefract.waterColor.data[2]);
        storage.reflectRefract.acidColor.data[0] = frame.Transmittance(storage.reflectRefract.acidColor.data[0]);
        storage.reflectRefract.acidColor.data[1] = frame.Transmittance(storage.reflectRefract.acidColor.data[1]);
        storage.reflectRefract.acidColor.data[2] = frame.Transmittance(storage.reflectRefract.acidColor.data[2]);
        storage.info.pReflectRefractParams = &storage.reflectRefract;
    }

    if (source.pSkyParams != nullptr)
    {
        storage.sky = *source.pSkyParams;
        storage.sky.skyViewerPosition = frame.Point(storage.sky.skyViewerPosition);
        storage.sky.godRaysSkyDirection = frame.Direction(storage.sky.godRaysSkyDirection);

        if (storage.sky.skyType == QR_SKY_TYPE_CUBEMAP)
        {
            storage.sky.skyCubemapRotationTransform = frame.CubemapRotation(storage.sky.skyCubemapRotationTransform);
        }
        else if (storage.sky.skyType == QR_SKY_TYPE_PROCEDURAL)
        {
            float *cloudSettings = &storage.sky.skyCubemapRotationTransform.matrix[0][0];
            cloudSettings[7] = frame.Length(cloudSettings[7]);
            cloudSettings[8] = frame.Length(cloudSettings[8]);
        }

        storage.info.pSkyParams = &storage.sky;
    }

    storage.postEffects = source.postEffectParams;
    storage.info.postEffectParams = storage.postEffects;

    if (source.postEffectParams.pNearDof != nullptr)
    {
        storage.nearDof = *source.postEffectParams.pNearDof;
        storage.nearDof.focusDistance = frame.Length(storage.nearDof.focusDistance);
        storage.info.postEffectParams.pNearDof = &storage.nearDof;
    }
}

}
