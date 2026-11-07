#include "qray/host_contract.h"

#include <vector>

namespace test_host
{

enum class Call
{
    CreateMaterial,
    CreateAnimatedMaterial,
    DestroyMaterial,
    BeginStatic,
    UploadGeometry,
    SubmitStatic,
    UploadWorldLights,
    SetFogVolumes,
    StartFrame,
    UpdateTransform,
    UpdateTexCoords,
    UpdateMaterial,
    UploadRasterized,
    UploadParticles,
    UploadDecal,
    UploadPortal,
    UploadDirectionalLight,
    UploadSphericalLight,
    UploadSpotLight,
    UploadPolygonalLight,
    UploadTexturedAreaLight,
    UploadDtal,
    UploadClusterSources,
    DrawFrame,
};

struct State
{
    std::vector<Call> calls;
    std::vector<QrGeometryUploadInfo> geometries;
    std::vector<QrWorldLightsUploadInfo> worldLights;
    std::vector<QrWorldLightFace> worldLightFaces;
    std::vector<QrDtalGroupUploadBatch> dtals;
    QrMaterial nextMaterial = 100;
    bool failUploadGeometry = false;
};

State &GetState()
{
    static State state;
    return state;
}

void Reset()
{
    GetState() = State{};
}

} // namespace test_host

extern "C"
{

QrResult qrCreateMaterial(QrInstance, const QrMaterialCreateInfo *, QrMaterial *pResult)
{
    test_host::GetState().calls.push_back(test_host::Call::CreateMaterial);
    *pResult = test_host::GetState().nextMaterial++;
    return QR_SUCCESS;
}

QrResult qrCreateAnimatedMaterial(QrInstance, const QrAnimatedMaterialCreateInfo *, QrMaterial *pResult)
{
    test_host::GetState().calls.push_back(test_host::Call::CreateAnimatedMaterial);
    *pResult = test_host::GetState().nextMaterial++;
    return QR_SUCCESS;
}

QrResult qrDestroyMaterial(QrInstance, QrMaterial)
{
    test_host::GetState().calls.push_back(test_host::Call::DestroyMaterial);
    return QR_SUCCESS;
}

QrResult qrBeginStaticGeometries(QrInstance)
{
    test_host::GetState().calls.push_back(test_host::Call::BeginStatic);
    return QR_SUCCESS;
}

QrResult qrUploadGeometry(QrInstance, const QrGeometryUploadInfo *pInfo)
{
    auto &state = test_host::GetState();
    state.calls.push_back(test_host::Call::UploadGeometry);
    state.geometries.push_back(*pInfo);
    return state.failUploadGeometry ? QR_WRONG_ARGUMENT : QR_SUCCESS;
}

QrResult qrSubmitStaticGeometries(QrInstance)
{
    test_host::GetState().calls.push_back(test_host::Call::SubmitStatic);
    return QR_SUCCESS;
}

QrResult qrUploadWorldLights(QrInstance, const QrWorldLightsUploadInfo *pInfo)
{
    auto &state = test_host::GetState();
    state.calls.push_back(test_host::Call::UploadWorldLights);
    state.worldLights.push_back(*pInfo);
    if (pInfo->numFaces != 0)
        state.worldLightFaces.assign(pInfo->pFaces, pInfo->pFaces + pInfo->numFaces);
    return QR_SUCCESS;
}

QrResult qrSetFogVolumes(QrInstance, uint32_t, const QrFogVolume *)
{
    test_host::GetState().calls.push_back(test_host::Call::SetFogVolumes);
    return QR_SUCCESS;
}

QrResult qrStartFrame(QrInstance, const QrStartFrameInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::StartFrame);
    return QR_SUCCESS;
}

QrResult qrUpdateGeometryTransform(QrInstance, const QrUpdateTransformInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UpdateTransform);
    return QR_SUCCESS;
}

QrResult qrUpdateGeometryTexCoords(QrInstance, const QrUpdateTexCoordsInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UpdateTexCoords);
    return QR_SUCCESS;
}

QrResult qrUpdateMaterialContents(QrInstance, const QrMaterialUpdateInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UpdateMaterial);
    return QR_SUCCESS;
}

QrResult qrUploadRasterizedGeometry(QrInstance, const QrRasterizedGeometryUploadInfo *,
                                    const float *, const QrViewport *)
{
    test_host::GetState().calls.push_back(test_host::Call::UploadRasterized);
    return QR_SUCCESS;
}

QrResult qrUploadParticles(QrInstance, const QrParticleUploadInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UploadParticles);
    return QR_SUCCESS;
}

QrResult qrUploadDecal(QrInstance, const QrDecalUploadInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UploadDecal);
    return QR_SUCCESS;
}

QrResult qrUploadPortal(QrInstance, const QrPortalUploadInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UploadPortal);
    return QR_SUCCESS;
}

QrResult qrUploadDirectionalLight(QrInstance, const QrDirectionalLightUploadInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UploadDirectionalLight);
    return QR_SUCCESS;
}

QrResult qrUploadSphericalLight(QrInstance, const QrSphericalLightUploadInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UploadSphericalLight);
    return QR_SUCCESS;
}

QrResult qrUploadSpotLight(QrInstance, const QrSpotLightUploadInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UploadSpotLight);
    return QR_SUCCESS;
}

QrResult qrUploadPolygonalLight(QrInstance, const QrPolygonalLightUploadInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UploadPolygonalLight);
    return QR_SUCCESS;
}

QrResult qrUploadTexturedAreaLight(QrInstance, const QrTexturedAreaLightUploadInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UploadTexturedAreaLight);
    return QR_SUCCESS;
}

QrResult qrUploadDtalGroups(QrInstance, const QrDtalGroupUploadBatch *pInfo)
{
    auto &state = test_host::GetState();
    state.calls.push_back(test_host::Call::UploadDtal);
    state.dtals.push_back(*pInfo);
    return QR_SUCCESS;
}

QrResult qrUploadClusterLightSources(QrInstance, const QrClusterLightSourcesUploadInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::UploadClusterSources);
    return QR_SUCCESS;
}

QrResult qrDrawFrame(QrInstance, const QrDrawFrameInfo *)
{
    test_host::GetState().calls.push_back(test_host::Call::DrawFrame);
    return QR_SUCCESS;
}

} // extern "C"
