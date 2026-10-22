#include <qray/qray.h>

#include "SpatialFrame.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{

bool NearlyEqual(float a, float b, float epsilon = 0.00001f)
{
    return std::fabs(a - b) <= epsilon;
}

bool Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
    }

    return condition;
}

QrSpatialConvention MakeConvention(QrAxis x, QrAxis y, QrAxis z, float unitsPerMeter)
{
    return {
        sizeof(QrSpatialConvention),
        QR_SPATIAL_CONVENTION_VERSION,
        x,
        y,
        z,
        QR_TRUE,
        unitsPerMeter,
        {},
    };
}

}

int main()
{
    if (!Check(std::strcmp(QR_API_VERSION, "1.05.0000") == 0, "unexpected QR_API_VERSION") ||
        !Check(sizeof(QrSpatialConvention) == 44, "unexpected QrSpatialConvention size") ||
        !Check(offsetof(QrSpatialConvention, unitsPerMeter) == 24, "unexpected QrSpatialConvention layout"))
    {
        return 1;
    }

    qray::SpatialFrame identity;
    if (!Check(identity.Configure(MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_POSITIVE_Y, QR_AXIS_POSITIVE_Z,
                                                QR_SCENE_UNITS_PER_METER)),
               "identity convention rejected") ||
        !Check(identity.IsIdentity(), "identity convention is not identity"))
    {
        return 1;
    }

    const QrFloat3D identityPoint = identity.Point({ { 1.0f, 2.0f, 3.0f } });
    if (!Check(identityPoint.data[0] == 1.0f && identityPoint.data[1] == 2.0f && identityPoint.data[2] == 3.0f,
               "identity point changed"))
    {
        return 1;
    }

    qray::SpatialFrame upY;
    if (!Check(upY.Configure(MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_NEGATIVE_Z, QR_AXIS_POSITIVE_Y, 1.0f)),
               "Y-up convention rejected") ||
        !Check(!upY.IsIdentity(), "Y-up convention marked identity") ||
        !Check(NearlyEqual(upY.Scale(), QR_SCENE_UNITS_PER_METER), "unexpected scale"))
    {
        return 1;
    }

    const QrFloat3D point = upY.Point({ { 1.0f, 2.0f, 3.0f } });
    if (!Check(NearlyEqual(point.data[0], 40.0f) &&
               NearlyEqual(point.data[1], -120.0f) &&
               NearlyEqual(point.data[2], 80.0f),
               "Y-up point conversion failed") ||
        !Check(NearlyEqual(upY.Area(2.0f), 3200.0f), "area conversion failed") ||
        !Check(upY.SoftFace(5) == 3, "softface conversion failed"))
    {
        return 1;
    }

    const QrFloat3D fogA = upY.Point({ { 0.0f, 0.0f, 1.0f } });
    const QrFloat3D fogB = upY.Point({ { 0.0f, 0.0f, 0.0f } });
    if (!Check(NearlyEqual(fogA.data[1], -40.0f) &&
               NearlyEqual(fogB.data[1], 0.0f) &&
               upY.SoftFace(5) == 3,
               "fog conversion failed"))
    {
        return 1;
    }

    QrFloat3D aabbMin = {};
    QrFloat3D aabbMax = {};
    upY.Aabb({ { -1.0f, -2.0f, -3.0f } }, { { 4.0f, 5.0f, 6.0f } }, aabbMin, aabbMax);
    if (!Check(NearlyEqual(aabbMin.data[0], -40.0f) && NearlyEqual(aabbMax.data[0], 160.0f) &&
               NearlyEqual(aabbMin.data[1], -240.0f) && NearlyEqual(aabbMax.data[1], 120.0f) &&
               NearlyEqual(aabbMin.data[2], -80.0f) && NearlyEqual(aabbMax.data[2], 200.0f),
               "AABB conversion failed"))
    {
        return 1;
    }

    QrTransform transform = {};
    transform.matrix[0][0] = 1.0f;
    transform.matrix[1][1] = 1.0f;
    transform.matrix[2][2] = 1.0f;
    transform.matrix[0][3] = 1.0f;
    transform.matrix[1][3] = 2.0f;
    transform.matrix[2][3] = 3.0f;

    const QrTransform convertedTransform = upY.Transform(transform);
    if (!Check(NearlyEqual(convertedTransform.matrix[0][3], 40.0f) &&
               NearlyEqual(convertedTransform.matrix[1][3], -120.0f) &&
               NearlyEqual(convertedTransform.matrix[2][3], 80.0f),
               "transform conversion failed"))
    {
        return 1;
    }

    float view[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 10.0f, 1.0f,
    };
    float convertedView[16] = {};
    upY.View(view, convertedView);

    if (!Check(NearlyEqual(convertedView[12], 0.0f) &&
               NearlyEqual(convertedView[13], 0.0f) &&
               NearlyEqual(convertedView[14], 400.0f),
               "view conversion failed"))
    {
        return 1;
    }

    float identityProjection[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    float convertedProjection[16] = {};
    upY.ViewProjection(identityProjection, convertedProjection);

    if (!Check(NearlyEqual(convertedProjection[0], 0.025f) &&
               NearlyEqual(convertedProjection[5], 0.0f) &&
               NearlyEqual(convertedProjection[6], -0.025f),
               "view-projection conversion failed"))
    {
        return 1;
    }

    QrMatrix3D cubemap = {};
    cubemap.matrix[0][0] = 1.0f;
    cubemap.matrix[1][1] = 1.0f;
    cubemap.matrix[2][2] = 1.0f;
    const QrMatrix3D convertedCubemap = upY.CubemapRotation(cubemap);
    if (!Check(NearlyEqual(convertedCubemap.matrix[0][0], 1.0f) &&
               NearlyEqual(convertedCubemap.matrix[1][2], 1.0f) &&
               NearlyEqual(convertedCubemap.matrix[2][1], -1.0f),
               "cubemap rotation conversion failed"))
    {
        return 1;
    }

    QrDrawFrameInfo drawInfo = {};
    QrDrawFrameSkyParams sky = {};
    QrDrawFrameReflectRefractParams reflect = {};
    sky.skyType = QR_SKY_TYPE_PROCEDURAL;
    float *cloudSettings = &sky.skyCubemapRotationTransform.matrix[0][0];
    cloudSettings[7] = 100.0f;
    cloudSettings[8] = 50.0f;
    reflect.waterColor = { { 0.25f, 0.5f, 0.75f } };
    reflect.acidDensity = 25.0f;
    drawInfo.pSkyParams = &sky;
    drawInfo.pReflectRefractParams = &reflect;
    drawInfo.cameraNear = 0.1f;
    drawInfo.cameraFar = 100.0f;
    drawInfo.rayLength = 200.0f;

    qray::SpatialFrameDrawStorage storage = {};
    ConvertDrawFrame(upY, drawInfo, storage);

    const float *convertedCloudSettings = &storage.sky.skyCubemapRotationTransform.matrix[0][0];

    if (!Check(NearlyEqual(cloudSettings[7], 100.0f), "source cloud settings changed") ||
        !Check(NearlyEqual(storage.info.cameraFar, 4000.0f), "near/far conversion failed") ||
        !Check(NearlyEqual(storage.info.rayLength, 8000.0f), "ray length conversion failed") ||
        !Check(storage.info.pSkyParams != nullptr, "sky params missing") ||
        !Check(storage.info.pReflectRefractParams != nullptr, "reflect params missing") ||
        !Check(NearlyEqual(storage.reflectRefract.acidDensity, 25.0f), "acid density changed") ||
        !Check(NearlyEqual(convertedCloudSettings[7], 4000.0f) &&
               NearlyEqual(convertedCloudSettings[8], 2000.0f),
               "cloud altitude conversion failed"))
    {
        return 1;
    }

    if (!Check(upY.Configure(MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_POSITIVE_X, QR_AXIS_POSITIVE_Z, 1.0f)) == false,
               "duplicate axes accepted") ||
        !Check(upY.Configure(MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_POSITIVE_Y, QR_AXIS_POSITIVE_Z, 0.0f)) == false,
               "zero units accepted"))
    {
        return 1;
    }

    const QrFloat3D pointAfterFailure = upY.Point({ { 1.0f, 2.0f, 3.0f } });
    if (!Check(NearlyEqual(pointAfterFailure.data[0], point.data[0]) &&
               NearlyEqual(pointAfterFailure.data[1], point.data[1]) &&
               NearlyEqual(pointAfterFailure.data[2], point.data[2]),
               "failed convention changed the active frame"))
    {
        return 1;
    }

    if (!Check(upY.Configure(MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_NEGATIVE_Y, QR_AXIS_POSITIVE_Z, 1.0f)) == false,
               "reflected permutation accepted"))
    {
        return 1;
    }

    QrSpatialConvention leftHanded = MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_POSITIVE_Y, QR_AXIS_POSITIVE_Z, 1.0f);
    leftHanded.hostIsRightHanded = QR_FALSE;
    if (!Check(upY.Configure(leftHanded) == false, "left-handed convention accepted"))
    {
        return 1;
    }

    const QrFloat3D pointAfterReflection = upY.Point({ { 1.0f, 2.0f, 3.0f } });
    if (!Check(NearlyEqual(pointAfterReflection.data[0], point.data[0]) &&
               NearlyEqual(pointAfterReflection.data[1], point.data[1]) &&
               NearlyEqual(pointAfterReflection.data[2], point.data[2]),
               "failed reflected convention changed the active frame"))
    {
        return 1;
    }

    std::puts("spatial contract tests passed");
    return 0;
}
