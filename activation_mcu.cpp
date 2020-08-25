#include "activation_mcu.hpp"

#include "images.hpp"
#include "item_updater.hpp"
#include "serialize.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <sdbusplus/exception.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#ifdef WANT_SIGNATURE_VERIFY
#include "image_verify.hpp"
#endif

namespace phosphor
{
namespace software
{
namespace updater
{

namespace softwareServer = sdbusplus::xyz::openbmc_project::Software::server;

using namespace phosphor::logging;
using sdbusplus::exception::SdBusError;
using InternalFailure =
    sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

#ifdef WANT_SIGNATURE_VERIFY
namespace control = sdbusplus::xyz::openbmc_project::Control::server;
#endif

auto McuActivation::activation(Activations value) -> Activations
{
    log<level::DEBUG>(("McuActivation::activation value:" + convertForMessage(value)).c_str());
    if ((value != softwareServer::Activation::Activations::Active) &&
        (value != softwareServer::Activation::Activations::Activating))
    {
        redundancyPriority.reset(nullptr);
    }

    if (value == softwareServer::Activation::Activations::Activating)
    {
        if (mcuFlashed == false)
        {
#ifdef WANT_SIGNATURE_VERIFY
            fs::path uploadDir(IMG_UPLOAD_DIR);
            if (!verifySignature(uploadDir / versionId, SIGNED_IMAGE_CONF_PATH))
            {
                onVerifyFailed();
                // Stop the activation process, if fieldMode is enabled.
                if (parent.control::FieldMode::fieldModeEnabled())
                {
                    return softwareServer::Activation::activation(
                        softwareServer::Activation::Activations::Failed);
                }
            }
#endif
            // Enable systemd signals
            Activation::subscribeToSystemdSignals();

            parent.freeSpace(*this);

            if (!activationProgress)
            {
                activationProgress =
                    std::make_unique<ActivationProgress>(bus, path);
            }

            if (!activationBlocksTransition)
            {
                activationBlocksTransition =
                    std::make_unique<ActivationBlocksTransition>(bus, path);
            }
            activationProgress->progress(10);
            flashWrite();
            activationProgress->progress(30);
        }
        else // MCU writed
        {
            // update MCU update status
            if (!redundancyPriority)
            {
                redundancyPriority =
                    std::make_unique<RedundancyPriority>(bus, path, *this, 0);
            }
            activationProgress->progress(100);

            activationBlocksTransition.reset(nullptr);
            activationProgress.reset(nullptr);

            this->mcuFlashed = false;
            Activation::unsubscribeFromSystemdSignals();
            // Remove version object from image manager
            Activation::deleteImageManagerObject();
            // Create active association
            parent.createActiveAssociation(path);
            // Only BIOS image need to create FunctionaAssociation
            // because BIOS already updated
            parent.createFunctionalAssociation(path);
            return softwareServer::Activation::activation(
                softwareServer::Activation::Activations::Active);
        }
    }
    else // activation() != Activating
    {
        activationBlocksTransition.reset(nullptr);
        activationProgress.reset(nullptr);
        this->mcuFlashed = false;
    }
    return softwareServer::Activation::activation(value);
}

} // namespace updater
} // namespace software
} // namespace phosphor
