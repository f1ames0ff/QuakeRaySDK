#include <qray/gpu_context.h>

#include <cassert>

namespace
{

qray::QrayGpuRequirements ValidRequirements()
{
    qray::QrayGpuRequirements requirements = {};
    requirements.header = {
        sizeof(qray::QrayGpuRequirements),
        qray::kGpuContextContractVersion,
    };
    return requirements;
}

} // namespace

int main()
{
    const qray::QrayGpuContractInfo *info = qray::qrayGetGpuContractInfo();
    assert(info != nullptr);
    assert(info->header.size == sizeof(qray::QrayGpuContractInfo));
    assert(info->contractVersion == qray::kGpuContextContractVersion);
    assert(info->requiredGraphicsQueues == 1);
    assert(info->maximumFramesInFlight >= 1);
    assert(info->maximumQueuesPerFamily >= 1);
    assert(qray::qrayGetGpuContextContractVersion() == qray::kGpuContextContractVersion);

    assert(qray::qrayValidateGpuRequirements(nullptr) == QR_SUCCESS);

    qray::QrayGpuRequirements requirements = ValidRequirements();
    assert(qray::qrayValidateGpuRequirements(&requirements) == QR_SUCCESS);

    qray::QrayContractDiagnostic diagnostic = {};
    requirements.header.size = 1;
    assert(qray::qrayValidateGpuRequirements(&requirements, &diagnostic) == QR_WRONG_ARGUMENT);
    assert(diagnostic.result == QR_WRONG_ARGUMENT);
    assert(diagnostic.code != 0);

    requirements = ValidRequirements();
    requirements.minimumFramesInFlight = 0;
    assert(qray::qrayValidateGpuRequirements(&requirements) == QR_WRONG_ARGUMENT);

    requirements = ValidRequirements();
    requirements.queues.graphicsQueueCount = 0;
    assert(qray::qrayValidateGpuRequirements(&requirements) == QR_WRONG_ARGUMENT);

    requirements = ValidRequirements();
    requirements.queues.computeQueueCount = 17;
    assert(qray::qrayValidateGpuRequirements(&requirements) == QR_WRONG_ARGUMENT);

    const char *validExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    requirements = ValidRequirements();
    requirements.requiredInstanceExtensions = { &validExtension, 1 };
    assert(qray::qrayValidateGpuRequirements(&requirements) == QR_SUCCESS);

    const char *emptyExtension = "";
    requirements.requiredInstanceExtensions = { &emptyExtension, 1 };
    assert(qray::qrayValidateGpuRequirements(&requirements) == QR_WRONG_ARGUMENT);

    qray::QrayGpuFormatRequirement format = {};
    format.format = VK_FORMAT_UNDEFINED;
    format.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    requirements = ValidRequirements();
    requirements.requiredFormats = { &format, 1 };
    assert(qray::qrayValidateGpuRequirements(&requirements) == QR_WRONG_ARGUMENT);

    format.format = VK_FORMAT_R8G8B8A8_UNORM;
    format.samples = VK_SAMPLE_COUNT_1_BIT;
    assert(qray::qrayValidateGpuRequirements(&requirements) == QR_SUCCESS);

    return 0;
}
