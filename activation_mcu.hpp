#pragma once

#include "activation.hpp"

namespace phosphor
{
namespace software
{
namespace updater
{

class McuActivation : public Activation
{
    public:
    static constexpr auto flashMcuServiceFile =
            "mcu-update.service";

    // same constructor with Activation
    /** @brief Constructs Activation Software Manager
     *
     * @param[in] bus    - The Dbus bus object
     * @param[in] path   - The Dbus object path
     * @param[in] parent - Parent object.
     * @param[in] versionId  - The software version id
     * @param[in] activationStatus - The status of Activation
     * @param[in] assocs - Association objects
     */
    McuActivation(sdbusplus::bus::bus& bus, const std::string& path,
               ItemUpdater& parent, std::string& versionId,
               sdbusplus::xyz::openbmc_project::Software::server::Activation::
               Activations activationStatus, AssociationList& assocs) :
               Activation(bus, path, parent, versionId, activationStatus, assocs)
    {
        log<level::DEBUG>("McuActivation::constructor");
        mcuFlashed = false;
    }

    ~McuActivation()
    {
        log<level::DEBUG>("McuActivation::destructor");
    }

    Activations activation(Activations value) override;

    /** @brief Activation */
    using ActivationInherit::activation;

    /** @brief Overloaded write flash function */
    void flashWrite() override;

    /** @brief Overloaded function that acts on service file state changes */
    void onStateChanges(sdbusplus::message::message&) override;

    private:
    /** @brief Trace if the service that upgrade BIOS has done. */
    bool mcuFlashed = false;
};

} // namespace updater
} // namespace software
} // namespace phosphor
