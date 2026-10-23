#include <qray/qray.h>

#include "ClusterCandidateRanking.h"
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

float LinearDeterminant(const QrTransform &value)
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

    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

QrTransform MakeLinearTransform(float xx, float xy, float xz, float yx, float yy, float yz,
                                float zx, float zy, float zz)
{
    QrTransform result = {};
    result.matrix[0][0] = xx;
    result.matrix[0][1] = xy;
    result.matrix[0][2] = xz;
    result.matrix[1][0] = yx;
    result.matrix[1][1] = yy;
    result.matrix[1][2] = yz;
    result.matrix[2][0] = zx;
    result.matrix[2][1] = zy;
    result.matrix[2][2] = zz;

    return result;
}

QrSpatialConvention MakeConvention(QrAxis x, QrAxis y, QrAxis z)
{
    return {
        sizeof(QrSpatialConvention),
        QR_SPATIAL_CONVENTION_VERSION,
        x,
        y,
        z,
        QR_TRUE,
        0,
        {},
    };
}

}

int main()
{
    if (!Check(std::strcmp(QR_API_VERSION, "1.06.0000") == 0, "unexpected QR_API_VERSION") ||
        !Check(sizeof(QrSpatialConvention) == 44, "unexpected QrSpatialConvention size") ||
        !Check(offsetof(QrSpatialConvention, reserved0) == 24, "unexpected QrSpatialConvention layout"))
    {
        return 1;
    }

    qray::SpatialFrame defaultIdentity;
    if (!Check(defaultIdentity.IsIdentity(), "default spatial frame is not identity") ||
        !Check(NearlyEqual(defaultIdentity.Scale(), 1.0f), "default spatial frame is not metric"))
    {
        return 1;
    }

    qray::SpatialFrame identity;
    if (!Check(identity.Configure(MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_POSITIVE_Y, QR_AXIS_POSITIVE_Z)),
               "identity convention rejected") ||
        !Check(identity.IsIdentity(), "identity convention did not stay identity") ||
        !Check(NearlyEqual(identity.Scale(), 1.0f), "identity convention did not stay metric"))
    {
        return 1;
    }

    const QrFloat3D identityPoint = identity.Point({ { 1.0f, 2.0f, 3.0f } });
    if (!Check(identityPoint.data[0] == 1.0f && identityPoint.data[1] == 2.0f &&
               identityPoint.data[2] == 3.0f, "identity point conversion failed"))
    {
        return 1;
    }

    qray::SpatialFrame upY;
    if (!Check(upY.Configure(MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_NEGATIVE_Z, QR_AXIS_POSITIVE_Y)),
               "Y-up convention rejected") ||
        !Check(!upY.IsIdentity(), "Y-up convention marked identity") ||
        !Check(NearlyEqual(upY.Scale(), 1.0f), "unexpected scale"))
    {
        return 1;
    }

    const QrFloat3D point = upY.Point({ { 1.0f, 2.0f, 3.0f } });
    if (!Check(NearlyEqual(point.data[0], 1.0f) &&
               NearlyEqual(point.data[1], -3.0f) &&
               NearlyEqual(point.data[2], 2.0f),
               "Y-up point conversion failed") ||
        !Check(NearlyEqual(upY.Area(2.0f), 2.0f), "area conversion failed") ||
        !Check(upY.SoftFace(5) == 3, "softface conversion failed"))
    {
        return 1;
    }

    const QrFloat3D fogA = upY.Point({ { 0.0f, 0.0f, 1.0f } });
    const QrFloat3D fogB = upY.Point({ { 0.0f, 0.0f, 0.0f } });
    if (!Check(NearlyEqual(fogA.data[1], -1.0f) &&
               NearlyEqual(fogB.data[1], 0.0f) &&
               upY.SoftFace(5) == 3,
               "fog conversion failed"))
    {
        return 1;
    }

    QrFloat3D aabbMin = {};
    QrFloat3D aabbMax = {};
    upY.Aabb({ { -1.0f, -2.0f, -3.0f } }, { { 4.0f, 5.0f, 6.0f } }, aabbMin, aabbMax);
    if (!Check(NearlyEqual(aabbMin.data[0], -1.0f) && NearlyEqual(aabbMax.data[0], 4.0f) &&
               NearlyEqual(aabbMin.data[1], -6.0f) && NearlyEqual(aabbMax.data[1], 3.0f) &&
               NearlyEqual(aabbMin.data[2], -2.0f) && NearlyEqual(aabbMax.data[2], 5.0f),
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
    if (!Check(NearlyEqual(convertedTransform.matrix[0][3], 1.0f) &&
               NearlyEqual(convertedTransform.matrix[1][3], -3.0f) &&
               NearlyEqual(convertedTransform.matrix[2][3], 2.0f),
               "transform conversion failed"))
    {
        return 1;
    }

    QrGeometryUploadInfo glassGeometry = {};
    glassGeometry.passThroughType = QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT;
    glassGeometry.transform = transform;
    glassGeometry.layerColors[2].data[0] = 1.5f;
    glassGeometry.layerColors[2].data[1] = 0.25f;
    const QrGeometryUploadInfo convertedGlass = upY.Geometry(glassGeometry);
    if (!Check(NearlyEqual(convertedGlass.layerColors[2].data[0], 1.5f) &&
               NearlyEqual(convertedGlass.layerColors[2].data[1], 0.25f) &&
               NearlyEqual(convertedGlass.transform.matrix[0][3], 1.0f),
               "glass thickness conversion failed"))
    {
        return 1;
    }

    glassGeometry.passThroughType = QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
    if (!Check(NearlyEqual(upY.Geometry(glassGeometry).layerColors[2].data[1], 0.25f),
               "non-glass material color was changed"))
    {
        return 1;
    }

    // The conversion is the same linear map the frame applies to points, so an
    // instance transform that is neither axis aligned nor uniform composes with
    // the frame instead of being rejected: the rotation about host Z lands in
    // scene rows (X, -Z, Y) and the scale stays per host axis.
    const float angle = 0.5235987755982988f;
    const float cosAngle = std::cos(angle);
    const float sinAngle = std::sin(angle);

    const QrTransform rotation = MakeLinearTransform(cosAngle, -sinAngle, 0.0f,
                                                     sinAngle, cosAngle, 0.0f,
                                                     0.0f, 0.0f, 1.0f);
    const QrTransform convertedRotation = upY.Transform(rotation);

    if (!Check(NearlyEqual(convertedRotation.matrix[0][0], cosAngle) &&
               NearlyEqual(convertedRotation.matrix[0][1], -sinAngle) &&
               NearlyEqual(convertedRotation.matrix[1][2], -1.0f) &&
               NearlyEqual(convertedRotation.matrix[2][0], sinAngle) &&
               NearlyEqual(convertedRotation.matrix[2][1], cosAngle),
               "arbitrary rotation conversion failed"))
    {
        return 1;
    }

    const QrTransform nonUniformScale = MakeLinearTransform(2.0f, 0.0f, 0.0f,
                                                            0.0f, 3.0f, 0.0f,
                                                            0.0f, 0.0f, 4.0f);
    const QrTransform convertedNonUniformScale = upY.Transform(nonUniformScale);

    if (!Check(NearlyEqual(convertedNonUniformScale.matrix[0][0], 2.0f) &&
               NearlyEqual(convertedNonUniformScale.matrix[1][2], -4.0f) &&
               NearlyEqual(convertedNonUniformScale.matrix[2][1], 3.0f),
               "non-uniform scale conversion failed"))
    {
        return 1;
    }

    if (!Check(LinearDeterminant(convertedRotation) > 0.0f &&
               LinearDeterminant(convertedNonUniformScale) > 0.0f,
               "orientation-preserving transform lost its orientation"))
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
               NearlyEqual(convertedView[14], 10.0f),
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

    if (!Check(NearlyEqual(convertedProjection[0], 1.0f) &&
               NearlyEqual(convertedProjection[5], 0.0f) &&
               NearlyEqual(convertedProjection[6], -1.0f),
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
    reflect.waterWaveSpeed = 2.0f;
    reflect.waterTextureAreaScale = 3.0f;
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
        !Check(NearlyEqual(storage.info.cameraFar, 100.0f), "near/far conversion failed") ||
        !Check(NearlyEqual(storage.info.rayLength, 200.0f), "ray length conversion failed") ||
        !Check(storage.info.pSkyParams != nullptr, "sky params missing") ||
        !Check(storage.info.pReflectRefractParams != nullptr, "reflect params missing") ||
        !Check(NearlyEqual(storage.reflectRefract.waterWaveSpeed, 2.0f), "water wave speed conversion failed") ||
        !Check(NearlyEqual(storage.reflectRefract.waterTextureAreaScale, 3.0f),
               "water texture area conversion failed") ||
        !Check(NearlyEqual(storage.reflectRefract.waterColor.data[0], 0.25f),
               "water transmittance conversion failed") ||
        !Check(NearlyEqual(storage.reflectRefract.acidDensity, 25.0f), "acid density changed") ||
        !Check(NearlyEqual(convertedCloudSettings[7], 100.0f) &&
               NearlyEqual(convertedCloudSettings[8], 50.0f),
               "cloud altitude conversion failed"))
    {
        return 1;
    }

    QrSpatialConvention invalidReserved = MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_POSITIVE_Y,
                                                         QR_AXIS_POSITIVE_Z);
    invalidReserved.reserved0 = 1;
    QrSpatialConvention oldConvention = MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_POSITIVE_Y,
                                                       QR_AXIS_POSITIVE_Z);
    oldConvention.version = 1;

    if (!Check(NearlyEqual(upY.Transmittance(0.0f), 0.000001f),
               "zero transmittance did not remain opaque") ||
        !Check(NearlyEqual(upY.Transmittance(0.000001f), 0.000001f),
               "small meter transmittance was clamped before unit conversion") ||
        !Check(NearlyEqual(upY.Transmittance(0.25f), 0.25f),
               "meter transmittance was not pass-through") ||
        !Check(upY.Configure(MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_POSITIVE_X, QR_AXIS_POSITIVE_Z)) == false,
               "duplicate axes accepted") ||
        !Check(!upY.Configure(invalidReserved), "nonzero reserved field accepted") ||
        !Check(!upY.Configure(oldConvention), "old spatial convention version accepted"))
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

    if (!Check(upY.Configure(MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_NEGATIVE_Y, QR_AXIS_POSITIVE_Z)) == false,
               "reflected permutation accepted"))
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

    qray::SpatialFrame mirroredFrame;
    QrSpatialConvention leftHanded = MakeConvention(QR_AXIS_POSITIVE_X, QR_AXIS_NEGATIVE_Y, QR_AXIS_POSITIVE_Z);
    leftHanded.hostIsRightHanded = QR_FALSE;
    if (!Check(mirroredFrame.Configure(leftHanded), "left-handed convention rejected") ||
        !Check(mirroredFrame.IsMirrored(), "left-handed convention not mirrored"))
    {
        return 1;
    }

    const QrFloat3D mirroredPoint = mirroredFrame.Point({ { 1.0f, 2.0f, 3.0f } });
    if (!Check(NearlyEqual(mirroredPoint.data[0], 1.0f) &&
               NearlyEqual(mirroredPoint.data[1], -2.0f) &&
               NearlyEqual(mirroredPoint.data[2], 3.0f),
               "left-handed point conversion failed"))
    {
        return 1;
    }

    // The winding rule composes the geometry transform with the frame: the host's
    // authored order survives only when the two flips agree in sign.
    QrTransform identityTransform = {};
    identityTransform.matrix[0][0] = 1.0f;
    identityTransform.matrix[1][1] = 1.0f;
    identityTransform.matrix[2][2] = 1.0f;

    QrTransform reflectedTransform = identityTransform;
    reflectedTransform.matrix[0][0] = -1.0f;

    if (!Check(!upY.ReversesWinding(identityTransform), "right-handed frame reversed an authored order"))
    {
        return 1;
    }

    if (!Check(mirroredFrame.ReversesWinding(identityTransform), "mirrored frame kept an authored order"))
    {
        return 1;
    }

    if (!Check(!mirroredFrame.ReversesWinding(reflectedTransform),
               "mirrored frame and reflected transform did not cancel"))
    {
        return 1;
    }

    // Any negative determinant counts, not only an axis reflection: a host may
    // mirror one instance by scaling an axis, and the frame still owns its own
    // flip, so the two signs decide the order together.
    const QrTransform mirroredInstance = MakeLinearTransform(-2.0f, 0.0f, 0.0f,
                                                            0.0f, 3.0f, 0.0f,
                                                            0.0f, 0.0f, 4.0f);

    if (!Check(upY.ReversesWinding(mirroredInstance),
               "right-handed frame kept the order of a mirrored instance"))
    {
        return 1;
    }

    if (!Check(!mirroredFrame.ReversesWinding(mirroredInstance),
               "mirrored instance and mirrored frame did not cancel"))
    {
        return 1;
    }

    if (!Check(!qray::ShouldReverseRasterizedWinding(
                   true, QR_RASTERIZED_GEOMETRY_STATE_FORCE_LINE_LIST),
               "line-list topology was treated as triangle winding") ||
        !Check(qray::ShouldReverseRasterizedWinding(true, 0),
               "mirrored triangle winding was not reversed") ||
        !Check(!qray::ShouldReverseRasterizedWinding(false, 0),
               "unflipped triangle winding was reversed"))
    {
        return 1;
    }

    if (!Check(!upY.ReversesWinding(rotation), "rotation reversed an authored order") ||
        !Check(!upY.ReversesWinding(nonUniformScale),
               "orientation-preserving non-uniform scale reversed an authored order") ||
        !Check(mirroredFrame.ReversesWinding(nonUniformScale),
               "mirrored frame kept the order of an orientation-preserving instance"))
    {
        return 1;
    }

    // The composed determinant is what the rule reads, so the converted matrix
    // has to keep the product's sign, and its magnitude scales with the cube of
    // the frame scale for a unit-determinant frame permutation.
    const float expectedDet = upY.Scale() * upY.Scale() * upY.Scale() * LinearDeterminant(nonUniformScale);
    if (!Check(LinearDeterminant(upY.Transform(mirroredInstance)) < 0.0f &&
               LinearDeterminant(mirroredFrame.Transform(nonUniformScale)) < 0.0f &&
               NearlyEqual(LinearDeterminant(convertedNonUniformScale) / expectedDet, 1.0f, 0.0001f),
               "composed determinant did not scale with the frame"))
    {
        return 1;
    }

    const struct
    {
        double radius;
        double expectedScore;
    } radiusCases[] = {
        { 0.0, 1600.0 },
        { 0.001, 1600.0 },
        { 0.025, 1600.0 },
        { 0.1, 100.0 },
        { 1.0, 1.0 },
    };

    for (const auto &test : radiusCases)
    {
        const double score = qray::ClusterCandidateScore(1.0, test.radius, 0.0);
        if (!Check(std::fabs(score - test.expectedScore) <= 0.0001,
                   "cluster candidate radius floor is not metric"))
        {
            return 1;
        }
    }

    if (!Check(std::fabs(qray::ClusterCandidateScore(1.0, 0.0, 0.01) - 100.0) <= 0.0001,
               "cluster candidate distance was clamped to the legacy square unit"))
    {
        return 1;
    }

    std::puts("spatial contract tests passed");
    return 0;
}
