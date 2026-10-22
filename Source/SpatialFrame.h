#pragma once

#include <qray/qray.h>

namespace qray
{

class SpatialFrame
{
public:
    SpatialFrame();

    bool Configure(const QrSpatialConvention &convention);

    bool IsIdentity() const { return identity; }
    bool IsMirrored() const { return mirrored; }
    float Scale() const { return scale; }
    float InvScale() const { return invScale; }

    QrFloat3D Point(const QrFloat3D &value) const;
    QrFloat3D Direction(const QrFloat3D &value) const;
    float Length(float value) const;
    float Area(float value) const;
    float Transmittance(float value) const;

    QrTransform Transform(const QrTransform &value) const;
    QrMatrix3D CubemapRotation(const QrMatrix3D &value) const;

    void Aabb(const QrFloat3D &minValue, const QrFloat3D &maxValue,
              QrFloat3D &outMin, QrFloat3D &outMax) const;
    uint32_t SoftFace(uint32_t value) const;

    void View(const float source[16], float destination[16]) const;
    void ViewProjection(const float source[16], float destination[16]) const;

private:
    float hostToScene[3][3];
    float sceneToHost[3][3];
    float scale;
    float invScale;
    bool identity;
    bool mirrored;
};

struct SpatialFrameDrawStorage
{
    QrDrawFrameInfo info;
    QrDrawFrameIlluminationParams illumination;
    QrDrawFrameVolumetricParams volumetric;
    QrDrawFrameLevelFogParams levelFog;
    QrDrawFrameReflectRefractParams reflectRefract;
    QrDrawFrameSkyParams sky;
    QrDrawFramePostEffectsParams postEffects;
    QrPostEffectsNearDofParams nearDof;
};

void ConvertDrawFrame(const SpatialFrame &frame, const QrDrawFrameInfo &source,
                      SpatialFrameDrawStorage &storage);

}
