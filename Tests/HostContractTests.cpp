#include "qray/host_contract.h"

#include "HostContractStubs.cpp"

#include <cassert>
#include <cstdint>
#include <utility>

namespace
{

using qray::QrayArrayView;
using qray::QrayContractHeader;
using qray::QrayFrameUpdates;
using qray::QrayFrameView;
using qray::QraySceneAsset;
using qray::QraySceneGeometry;
using qray::QraySceneMaterial;
using qray::QrayStableKey;

template<typename T>
QrayContractHeader Header()
{
    return { sizeof(T), qray::kHostContractVersion };
}

QrayStableKey Key(uint64_t value, qray::QrayStableKeyDomain domain)
{
    return { value, uint32_t(domain), 0 };
}

struct Fixture
{
    QrVertex vertex = {};
    QrMaterialCreateInfo materialInfo = {};
    QraySceneMaterial material = {};
    QraySceneGeometry geometry = {};
    QraySceneAsset asset = {};

    Fixture()
    {
        materialInfo.size = { 1, 1 };
        materialInfo.pRelativePath = "test_material";
        material.key = Key(1, qray::QRAY_KEY_MATERIAL);
        material.pCreateInfo = &materialInfo;

        vertex.position[0] = 0.0f;
        vertex.position[1] = 0.0f;
        vertex.position[2] = 0.0f;
        vertex.normal[2] = 1.0f;
        geometry.key = Key(1, qray::QRAY_KEY_SURFACE);
        geometry.materialKeys[0] = material.key;
        geometry.geometry.uniqueID = 10;
        geometry.geometry.geomType = QR_GEOMETRY_TYPE_STATIC;
        geometry.geometry.vertexCount = 1;
        geometry.geometry.pVertices = &vertex;

        asset.header = Header<QraySceneAsset>();
        asset.sceneKey = Key(1, qray::QRAY_KEY_SCENE);
        asset.fingerprint = { 11, 22, 1, 0 };
        asset.materials = { &material, 1 };
        asset.geometry = { &geometry, 1 };
    }
};

class MockHost final : public qray::IQrayHost
{
public:
    qray::QrayHostCapabilities Capabilities() const noexcept override
    {
        return { Header<qray::QrayHostCapabilities>(),
                 qray::QRAY_HOST_CAP_SURFACE_KEYS | qray::QRAY_HOST_CAP_SIDECAR };
    }

    QrResult ApplySidecar(const qray::QraySidecar &sidecar,
                          qray::QrayMutableArrayView<qray::QraySidecarEntryResult> results,
                          uint32_t *pCount) noexcept override
    {
        for (uint32_t i = 0; i < sidecar.entries.count; ++i)
        {
            results.pData[i].authoringId = sidecar.entries.pData[i].authoringId;
            results.pData[i].status = qray::QraySidecarEntryStatus::Applied;
            results.pData[i].result = QR_SUCCESS;
        }
        *pCount = sidecar.entries.count;
        return QR_SUCCESS;
    }
};

void TestValidation()
{
    assert(qray::qrayValidateSceneAsset(nullptr) == QR_WRONG_ARGUMENT);
    Fixture fixture;
    assert(qray::qrayValidateSceneAsset(&fixture.asset) == QR_SUCCESS);

    fixture.asset.header.size = 1;
    assert(qray::qrayValidateSceneAsset(&fixture.asset) == QR_WRONG_ARGUMENT);

    fixture.asset.header = Header<QraySceneAsset>();
    fixture.geometry.geometry.pVertices = nullptr;
    assert(qray::qrayValidateSceneAsset(&fixture.asset) == QR_WRONG_ARGUMENT);
}

void TestBindAndResolve()
{
    test_host::Reset();
    Fixture fixture;
    qray::QraySceneBinding binding;
    assert(binding.Bind(nullptr, fixture.asset) == QR_SUCCESS);

    const auto &calls = test_host::GetState().calls;
    assert(calls.size() == 4);
    assert(calls[0] == test_host::Call::CreateMaterial);
    assert(calls[1] == test_host::Call::BeginStatic);
    assert(calls[2] == test_host::Call::UploadGeometry);
    assert(calls[3] == test_host::Call::SubmitStatic);

    const auto &upload = test_host::GetState().geometries.front();
    assert(upload.geomMaterial.layerMaterials[0] == 100);
    assert(upload.geomMaterial.layerMaterials[1] == QR_NO_MATERIAL);

    uint64_t runtimeId = 0;
    assert(binding.ResolveSurface(fixture.geometry.key, &runtimeId) == QR_SUCCESS);
    assert(runtimeId == fixture.geometry.geometry.uniqueID);

    QrMaterial material = QR_NO_MATERIAL;
    assert(binding.ResolveMaterial(fixture.material.key, &material) == QR_SUCCESS);
    assert(material == 100);

    binding.Reset();
}

void TestFrameDispatch()
{
    test_host::Reset();
    Fixture fixture;
    qray::QraySceneBinding binding;
    assert(binding.Bind(nullptr, fixture.asset) == QR_SUCCESS);
    test_host::GetState().calls.clear();
    test_host::GetState().geometries.clear();

    QrStartFrameInfo start = {};
    QrDrawFrameInfo draw = {};
    QrayFrameView view = {};
    view.header = Header<QrayFrameView>();
    view.viewId = 1;
    view.pStartInfo = &start;
    view.pDrawInfo = &draw;

    qray::QrayTransformUpdate transform = {};
    transform.geometry = fixture.geometry.key;
    uint8_t materialBytes[4] = {};
    qray::QrayMaterialContentUpdate materialUpdate = {};
    materialUpdate.material = fixture.material.key;
    materialUpdate.textures.pDataNormal = materialBytes;

    QrayFrameUpdates updates = {};
    updates.header = Header<QrayFrameUpdates>();
    updates.transforms = { &transform, 1 };
    updates.materialContents = { &materialUpdate, 1 };

    assert(binding.RenderFrame(view, updates) == QR_SUCCESS);
    const auto &calls = test_host::GetState().calls;
    assert(calls.size() == 4);
    assert(calls[0] == test_host::Call::StartFrame);
    assert(calls[1] == test_host::Call::UpdateTransform);
    assert(calls[2] == test_host::Call::UpdateMaterial);
    assert(calls[3] == test_host::Call::DrawFrame);

    binding.Reset();
}

void TestOwnedSceneFrameData()
{
    test_host::Reset();
    Fixture fixture;
    QrSphericalLightUploadInfo lightInfo = {};
    lightInfo.uniqueID = 77;
    lightInfo.color = { { 1.0f, 0.5f, 0.25f } };
    lightInfo.position = { { 0.0f, 1.0f, 2.0f } };
    lightInfo.radius = 0.1f;

    qray::QraySceneLight light = {};
    light.key = Key(3, qray::QRAY_KEY_LIGHT);
    light.kind = qray::QraySceneLightKind::Spherical;
    light.pSpherical = &lightInfo;
    fixture.asset.lights = { &light, 1 };

    qray::QraySceneBinding binding;
    assert(binding.Bind(nullptr, fixture.asset) == QR_SUCCESS);

    light.pSpherical = nullptr;
    lightInfo.uniqueID = 0;
    test_host::GetState().calls.clear();

    QrStartFrameInfo start = {};
    QrDrawFrameInfo draw = {};
    QrayFrameView view = {};
    view.header = Header<QrayFrameView>();
    view.viewId = 2;
    view.pStartInfo = &start;
    view.pDrawInfo = &draw;
    QrayFrameUpdates updates = {};
    updates.header = Header<QrayFrameUpdates>();

    assert(binding.RenderFrame(view, updates) == QR_SUCCESS);
    const auto &calls = test_host::GetState().calls;
    assert(calls.size() == 3);
    assert(calls[0] == test_host::Call::StartFrame);
    assert(calls[1] == test_host::Call::UploadSphericalLight);
    assert(calls[2] == test_host::Call::DrawFrame);

    qray::QraySceneBinding binding2;
    fixture.asset.lights.pData = nullptr;
    assert(binding2.Bind(nullptr, fixture.asset) == QR_WRONG_ARGUMENT);

    binding.Reset();
}

void TestStaticFailure()
{
    test_host::Reset();
    Fixture fixture;
    test_host::GetState().failUploadGeometry = true;
    qray::QraySceneBinding binding;
    assert(binding.Bind(nullptr, fixture.asset) == QR_WRONG_ARGUMENT);
    assert(binding.IsPoisoned());

    const auto &calls = test_host::GetState().calls;
    assert(calls.size() == 5);
    assert(calls[0] == test_host::Call::CreateMaterial);
    assert(calls[1] == test_host::Call::BeginStatic);
    assert(calls[2] == test_host::Call::UploadGeometry);
    assert(calls[3] == test_host::Call::SubmitStatic);
    assert(calls[4] == test_host::Call::DestroyMaterial);
}

void TestBindingOwnership()
{
    test_host::Reset();
    Fixture fixture;
    {
        qray::QraySceneBinding first;
        qray::QraySceneBinding second;
        assert(first.Bind(nullptr, fixture.asset) == QR_SUCCESS);
        assert(second.Bind(nullptr, fixture.asset) == QR_SUCCESS);
        test_host::GetState().calls.clear();

        first = std::move(second);
        assert(test_host::GetState().calls.size() == 1);
        assert(test_host::GetState().calls[0] == test_host::Call::DestroyMaterial);
    }
    assert(test_host::GetState().calls.size() == 2);
    assert(test_host::GetState().calls[1] == test_host::Call::DestroyMaterial);
}

void TestWorldLightMaterialLowering()
{
    test_host::Reset();
    Fixture fixture;
    QrWorldLightFace face = {};
    QrWorldLightsUploadInfo upload = {};
    upload.numFaces = 1;
    upload.pFaces = &face;
    const QrayStableKey faceMaterial = fixture.material.key;
    fixture.asset.worldLights.pUpload = &upload;
    fixture.asset.worldLights.faceMaterialKeys = { &faceMaterial, 1 };

    {
        qray::QraySceneBinding binding;
        assert(binding.Bind(nullptr, fixture.asset) == QR_SUCCESS);
        const auto &state = test_host::GetState();
        assert(state.worldLightFaces.size() == 1);
        assert(state.worldLightFaces[0].material == 100);
    }
}

void TestFrameReferenceValidation()
{
    test_host::Reset();
    Fixture fixture;
    qray::QraySceneBinding binding;
    assert(binding.Bind(nullptr, fixture.asset) == QR_SUCCESS);

    QraySceneGeometry dynamic = fixture.geometry;
    dynamic.key = Key(2, qray::QRAY_KEY_SURFACE);
    dynamic.geometry.uniqueID = 20;
    dynamic.geometry.geomType = QR_GEOMETRY_TYPE_DYNAMIC;
    dynamic.materialKeys[0] = Key(999, qray::QRAY_KEY_MATERIAL);

    QrStartFrameInfo start = {};
    QrDrawFrameInfo draw = {};
    QrayFrameView view = {};
    view.header = Header<QrayFrameView>();
    view.viewId = 1;
    view.pStartInfo = &start;
    view.pDrawInfo = &draw;
    QrayFrameUpdates updates = {};
    updates.header = Header<QrayFrameUpdates>();
    updates.dynamicGeometry = { &dynamic, 1 };

    test_host::GetState().calls.clear();
    assert(binding.RenderFrame(view, updates) == QR_WRONG_ARGUMENT);
    assert(test_host::GetState().calls.empty());
}

void TestUnsupportedAndSidecar()
{
    test_host::Reset();
    Fixture fixture;
    qray::QraySceneBinding binding;
    assert(binding.Bind(nullptr, fixture.asset) == QR_SUCCESS);

    QrStartFrameInfo start = {};
    QrDrawFrameInfo draw = {};
    QrayFrameView view = {};
    view.header = Header<QrayFrameView>();
    view.viewId = 1;
    view.flags = qray::QRAY_FRAME_VIEW_RESET_HISTORY;
    view.pStartInfo = &start;
    view.pDrawInfo = &draw;
    QrayFrameUpdates updates = {};
    updates.header = Header<QrayFrameUpdates>();

    test_host::GetState().calls.clear();
    assert(binding.RenderFrame(view, updates) == QR_FEATURE_UNAVAILABLE);
    assert(test_host::GetState().calls.empty());

    qray::QraySidecarEntry entries[2] = {};
    entries[0].authoringId = { 1, 2 };
    entries[0].kind = qray::QraySidecarEntryKind::Material;
    entries[0].material.surface = fixture.geometry.key;
    entries[0].material.flags = qray::QRAY_MATERIAL_INTENT_ROUGHNESS;
    entries[1] = entries[0];

    qray::QraySidecar sidecar = {};
    sidecar.header = Header<qray::QraySidecar>();
    sidecar.sceneKey = fixture.asset.sceneKey;
    sidecar.fingerprint = fixture.asset.fingerprint;
    sidecar.entries = { entries, 2 };
    assert(qray::qrayValidateSidecar(&sidecar) == QR_WRONG_ARGUMENT);

    entries[1].authoringId = { 3, 4 };
    assert(qray::qrayValidateSidecar(&sidecar) == QR_SUCCESS);
    entries[1].material.flags |= 1u << 31;
    assert(qray::qrayValidateSidecar(&sidecar) == QR_WRONG_ARGUMENT);
    entries[1].material.flags &= ~(1u << 31);

    MockHost host;
    qray::QraySidecarEntryResult results[2] = {};
    uint32_t resultCount = 0;
    assert(binding.ApplySidecar(host, sidecar, { results, 2 }, &resultCount) == QR_SUCCESS);
    assert(resultCount == 2);
    assert(results[0].status == qray::QraySidecarEntryStatus::Applied);

    QrayStableKey key = {};
    uint64_t id = 0;
    assert(host.SurfaceKey(1, &key) == QR_FEATURE_UNAVAILABLE);
    assert(host.ResolveSurface(fixture.geometry.key, &id) == QR_FEATURE_UNAVAILABLE);
    assert(qray::qrayGetHostContractVersion() == qray::kHostContractVersion);

    binding.Reset();
}

} // namespace

int main()
{
    TestValidation();
    TestBindAndResolve();
    TestFrameDispatch();
    TestOwnedSceneFrameData();
    TestStaticFailure();
    TestBindingOwnership();
    TestWorldLightMaterialLowering();
    TestFrameReferenceValidation();
    TestUnsupportedAndSidecar();
    return 0;
}
