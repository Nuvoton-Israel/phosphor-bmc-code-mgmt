#include "config.h"

#include "item_updater.hpp"

#include "images.hpp"
#include "serialize.hpp"
#include "version.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/log.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Software/Image/error.hpp>

#include <filesystem>
#include <fstream>
#include <queue>
#include <set>
#include <list>
#include <string>
#include <thread>

namespace phosphor
{
namespace software
{
namespace updater
{

// When you see server:: you know we're referencing our base class
namespace server = sdbusplus::xyz::openbmc_project::Software::server;
namespace control = sdbusplus::xyz::openbmc_project::Control::server;

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Software::Image::Error;
using namespace phosphor::software::image;
namespace fs = std::filesystem;
using NotAllowed = sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed;
using VersionPurpose = server::Version::VersionPurpose;

void ItemUpdater::createActivation(sdbusplus::message::message& msg)
{

    using SVersion = server::Version;
    using VersionClass = phosphor::software::manager::Version;

    sdbusplus::message::object_path objPath;
    auto purpose = VersionPurpose::Unknown;
    std::string version;
    std::map<std::string, std::map<std::string, std::variant<std::string>>>
        interfaces;
    msg.read(objPath, interfaces);
    std::string path(std::move(objPath));
    std::string filePath;

    for (const auto& intf : interfaces)
    {
        if (intf.first == VERSION_IFACE)
        {
            for (const auto& property : intf.second)
            {
                if (property.first == "Purpose")
                {
                    auto value = SVersion::convertVersionPurposeFromString(
                        std::get<std::string>(property.second));
                    if (value == VersionPurpose::BMC ||
                        value == VersionPurpose::Host ||
                        value == VersionPurpose::System ||
                        value == VersionPurpose::MCU)
                    {
                        purpose = value;
                    }
                }
                else if (property.first == "Version")
                {
                    version = std::get<std::string>(property.second);
                }
            }
        }
        else if (intf.first == FILEPATH_IFACE)
        {
            for (const auto& property : intf.second)
            {
                if (property.first == "Path")
                {
                    filePath = std::get<std::string>(property.second);
                }
            }
        }
    }
    if (version.empty() || filePath.empty() ||
        purpose == VersionPurpose::Unknown)
    {
        return;
    }

    // Version id is the last item in the path
    auto pos = path.rfind("/");
    if (pos == std::string::npos)
    {
        log<level::ERR>("No version id found in object path",
                        entry("OBJPATH=%s", path.c_str()));
        return;
    }

    auto versionId = path.substr(pos + 1);

    if (activations.find(versionId) == activations.end())
    {
        // Determine the Activation state by processing the given image dir.
        auto activationState = server::Activation::Activations::Invalid;
        ItemUpdater::ActivationStatus result;
#if 1 // Nuvoton BIOS image should be valid
        if (purpose == VersionPurpose::BMC || purpose == VersionPurpose::System)
            result = ItemUpdater::validateSquashFSImage(filePath);
        else if (purpose == VersionPurpose::Host)
            result = ItemUpdater::validateBIOSImage(filePath);
        else
            result = ItemUpdater::validateMCUImage(filePath);
#else // Facebook host update
        if (purpose == VersionPurpose::BMC || purpose == VersionPurpose::System)
            result = ItemUpdater::validateSquashFSImage(filePath);
        else
            result = ItemUpdater::ActivationStatus::ready;
#endif

        AssociationList associations = {};

        if (result == ItemUpdater::ActivationStatus::ready)
        {
            activationState = server::Activation::Activations::Ready;
            // Create an association to the BMC inventory item
            std::string _inventoryPath;
            if (purpose == VersionPurpose::Host)
            {
                _inventoryPath = hostInventoryPath;
            }
            else if (purpose == VersionPurpose::MCU)
            {
                _inventoryPath = mcuInventoryPath;
            }
            else
            {
                _inventoryPath = bmcInventoryPath;
            }
            associations.emplace_back(
                std::make_tuple(ACTIVATION_FWD_ASSOCIATION,
                                ACTIVATION_REV_ASSOCIATION, _inventoryPath));
        }

        std::unique_ptr<Activation> activationPtr;
        if (purpose == VersionPurpose::Host)
        {
            activationPtr = std::make_unique<HostActivation>(bus, path,
                        *this, versionId, activationState, associations);
        }
        else if (purpose == VersionPurpose::MCU)
        {
            activationPtr = std::make_unique<McuActivation>(bus, path,
                        *this, versionId, activationState, associations);
        }
        else
        {
            activationPtr = std::make_unique<Activation>(bus, path,
                        *this, versionId, activationState, associations);
        }

        activations.insert(std::make_pair(versionId, std::move(activationPtr)));

        auto versionPtr = std::make_unique<VersionClass>(
            bus, path, version, purpose, filePath,
            std::bind(&ItemUpdater::erase, this, std::placeholders::_1));
        versionPtr->deleteObject =
            std::make_unique<phosphor::software::manager::Delete>(bus, path,
                                                                  *versionPtr);
        versions.insert(std::make_pair(versionId, std::move(versionPtr)));
    }
    return;
}

void ItemUpdater::processHostImage()
{
    auto biosRelease = fs::path(BIOS_FW_FILE);

    if (!fs::is_regular_file(biosRelease))
    {
        log<level::ERR>("Failed to read biosRelease",
                        entry("FILENAME=%s", biosRelease.string().c_str()));
        return;
    }

    // Read bios-release from /usr/share/phosphor-bmc-code-mgmt to get the initial BIOS version
    // The version may be chenaged by ipmi command, set system info.
    auto initialVersion = VersionClass::getBMCVersion(biosRelease);
    if (initialVersion != INVALID_VERSION)
    {
        createHostVersion(initialVersion);
    }
    else
    {
        log<level::INFO>("Invalid version, skip create host version!");
    }
}

void ItemUpdater::createHostVersion(const std::string& version)
{
    using VersionClass = phosphor::software::manager::Version;
    log<level::INFO>(("created host version: " + version).c_str());

    auto activationState = server::Activation::Activations::Active;
    auto purpose = server::Version::VersionPurpose::Host;
    auto id = VersionClass::getId(version);
    auto path = fs::path(SOFTWARE_OBJPATH) / id;
    createFunctionalAssociation(path);

    AssociationList associations = {};

    // Create an association to the system inventory item
    associations.emplace_back(std::make_tuple(ACTIVATION_FWD_ASSOCIATION,
                                              ACTIVATION_REV_ASSOCIATION,
                                              hostInventoryPath));

    // Create an active association since this image is active
    createActiveAssociation(path);

    // Create Version instance for this version.
    auto versionPtr = std::make_unique<VersionClass>(
        bus, path, version, purpose, "",
        std::bind(&ItemUpdater::erase, this, std::placeholders::_1));

    versions.insert(std::make_pair(id, std::move(versionPtr)));

    // Create Activation instance for this version.
    activations.insert(std::make_pair(
        id, std::make_unique<HostActivation>(bus, path, *this, id,
                                    activationState, associations)));

    uint8_t priority = std::numeric_limits<uint8_t>::max();
    if (!restorePriority(id, priority))
        priority = 0;

    activations.find(id)->second->redundancyPriority =
        std::make_unique<RedundancyPriority>(
            bus, path, *(activations.find(id)->second), priority, false);

    return;
}

void ItemUpdater::processMcuImage()
{
    auto mcuRelease = fs::path(MCU_FW_FILE);

    if (!fs::is_regular_file(mcuRelease))
    {
        log<level::INFO>("Failed to read mcuRelease",
                        entry("FILENAME=%s", mcuRelease.string().c_str()));
        return;
    }

    // Read mcu-release from /usr/share/phosphor-bmc-code-mgmt to get the initial MCU version
    auto initialVersion = VersionClass::getBMCVersion(mcuRelease);
    if (initialVersion != INVALID_VERSION)
    {
        createMcuVersion(initialVersion);
    }
    else
    {
        log<level::INFO>("Invalid version, skip create mcu version!");
    }
}

void ItemUpdater::createMcuVersion(const std::string& version)
{
    using VersionClass = phosphor::software::manager::Version;
    log<level::INFO>(("created mcu version: " + version).c_str());

    auto activationState = server::Activation::Activations::Active;
    auto purpose = server::Version::VersionPurpose::MCU;
    auto id = VersionClass::getId(version);
    auto path = fs::path(SOFTWARE_OBJPATH) / id;
    createFunctionalAssociation(path);

    AssociationList associations = {};

    // Create an association to the system inventory item
    associations.emplace_back(std::make_tuple(ACTIVATION_FWD_ASSOCIATION,
                                              ACTIVATION_REV_ASSOCIATION,
                                              mcuInventoryPath));

    // Create an active association since this image is active
    createActiveAssociation(path);

    // Create Version instance for this version.
    auto versionPtr = std::make_unique<VersionClass>(
        bus, path, version, purpose, "",
        std::bind(&ItemUpdater::erase, this, std::placeholders::_1));

    versions.insert(std::make_pair(id, std::move(versionPtr)));

    // Create Activation instance for this version.
    activations.insert(std::make_pair(
        id, std::make_unique<McuActivation>(bus, path, *this, id,
                                    activationState, associations)));

    uint8_t priority = std::numeric_limits<uint8_t>::max();
    if (!restorePriority(id, priority))
        priority = 0;

    activations.find(id)->second->redundancyPriority =
        std::make_unique<RedundancyPriority>(
            bus, path, *(activations.find(id)->second), priority, false);

    return;
}

void ItemUpdater::processBMCImage()
{
    using VersionClass = phosphor::software::manager::Version;

    // Check MEDIA_DIR and create if it does not exist
    try
    {
        if (!fs::is_directory(MEDIA_DIR))
        {
            fs::create_directory(MEDIA_DIR);
        }
    }
    catch (const fs::filesystem_error& e)
    {
        log<level::ERR>("Failed to prepare dir", entry("ERR=%s", e.what()));
        return;
    }

    // Read os-release from /etc/ to get the functional BMC version
    auto functionalVersion = VersionClass::getBMCVersion(OS_RELEASE_FILE);

    // Read os-release from folders under /media/ to get
    // BMC Software Versions.
    for (const auto& iter : fs::directory_iterator(MEDIA_DIR))
    {
        auto activationState = server::Activation::Activations::Active;
        static const auto BMC_RO_PREFIX_LEN = strlen(BMC_ROFS_PREFIX);

        // Check if the BMC_RO_PREFIXis the prefix of the iter.path
        if (0 ==
            iter.path().native().compare(0, BMC_RO_PREFIX_LEN, BMC_ROFS_PREFIX))
        {
            // Get the version to calculate the id
            fs::path releaseFile(OS_RELEASE_FILE);
            auto osRelease = iter.path() / releaseFile.relative_path();
            if (!fs::is_regular_file(osRelease))
            {
                log<level::ERR>(
                    "Failed to read osRelease",
                    entry("FILENAME=%s", osRelease.string().c_str()));

                // Try to get the version id from the mount directory name and
                // call to delete it as this version may be corrupted. Dynamic
                // volumes created by the UBI layout for example have the id in
                // the mount directory name. The worst that can happen is that
                // erase() is called with an non-existent id and returns.
                auto id = iter.path().native().substr(BMC_RO_PREFIX_LEN);
                ItemUpdater::erase(id);

                continue;
            }
            auto version = VersionClass::getBMCVersion(osRelease);
            if (version.empty())
            {
                log<level::ERR>(
                    "Failed to read version from osRelease",
                    entry("FILENAME=%s", osRelease.string().c_str()));

                // Try to delete the version, same as above if the
                // OS_RELEASE_FILE does not exist.
                auto id = iter.path().native().substr(BMC_RO_PREFIX_LEN);
                ItemUpdater::erase(id);

                continue;
            }

            auto id = VersionClass::getId(version);

            // Check if the id has already been added. This can happen if the
            // BMC partitions / devices were manually flashed with the same
            // image.
            if (versions.find(id) != versions.end())
            {
                continue;
            }

            auto purpose = server::Version::VersionPurpose::BMC;
            restorePurpose(id, purpose);

            auto path = fs::path(SOFTWARE_OBJPATH) / id;

            // Create functional association if this is the functional
            // version
            if (version.compare(functionalVersion) == 0)
            {
                createFunctionalAssociation(path);
            }

            AssociationList associations = {};

            if (activationState == server::Activation::Activations::Active)
            {
                // Create an association to the BMC inventory item
                associations.emplace_back(std::make_tuple(
                    ACTIVATION_FWD_ASSOCIATION, ACTIVATION_REV_ASSOCIATION,
                    bmcInventoryPath));
                // Create an active association since this image is active
                createActiveAssociation(path);
            }

            // All updateable firmware components must expose the updateable
            // association.
            createUpdateableAssociation(path);

            // Create Version instance for this version.
            auto versionPtr = std::make_unique<VersionClass>(
                bus, path, version, purpose, "",
                std::bind(&ItemUpdater::erase, this, std::placeholders::_1));
            auto isVersionFunctional = versionPtr->isFunctional();
            if (!isVersionFunctional)
            {
                versionPtr->deleteObject =
                    std::make_unique<phosphor::software::manager::Delete>(
                        bus, path, *versionPtr);
            }
            versions.insert(std::make_pair(id, std::move(versionPtr)));

            // Create Activation instance for this version.
            activations.insert(std::make_pair(
                id, std::make_unique<Activation>(
                        bus, path, *this, id, activationState, associations)));

            // If Active, create RedundancyPriority instance for this
            // version.
            if (activationState == server::Activation::Activations::Active)
            {
                uint8_t priority = std::numeric_limits<uint8_t>::max();
                if (!restorePriority(id, priority))
                {
                    if (isVersionFunctional)
                    {
                        priority = 0;
                    }
                    else
                    {
                        log<level::ERR>("Unable to restore priority from file.",
                                        entry("VERSIONID=%s", id.c_str()));
                    }
                }
                activations.find(id)->second->redundancyPriority =
                    std::make_unique<RedundancyPriority>(
                        bus, path, *(activations.find(id)->second), priority,
                        false);
            }
        }
    }

    // If there are no bmc versions mounted under MEDIA_DIR, then read the
    // /etc/os-release and create rofs-<versionId> under MEDIA_DIR, then call
    // again processBMCImage() to create the D-Bus interface for it.
    if (activations.size() < 2)
    {
        auto version = VersionClass::getBMCVersion(OS_RELEASE_FILE);
        auto id = phosphor::software::manager::Version::getId(version);
        auto versionFileDir = BMC_ROFS_PREFIX + id + "/etc/";
        try
        {
            if (!fs::is_directory(versionFileDir))
            {
                fs::create_directories(versionFileDir);
            }
            auto versionFilePath = BMC_ROFS_PREFIX + id + OS_RELEASE_FILE;
            fs::create_directory_symlink(OS_RELEASE_FILE, versionFilePath);
            ItemUpdater::processBMCImage();
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(e.what());
        }
    }

    mirrorUbootToAlt();
    return;
}

void ItemUpdater::erase(std::string entryId)
{
    // Find entry in versions map
    auto it = versions.find(entryId);
    if (it != versions.end())
    {
        if (it->second->isFunctional() && ACTIVE_BMC_MAX_ALLOWED > 1)
        {
            log<level::ERR>("Error: Version is currently running on the BMC. "
                            "Unable to remove.",
                            entry("VERSIONID=%s", entryId.c_str()));
            return;
        }
    }

    // First call resetUbootEnvVars() so that the BMC points to a valid image to
    // boot from. If resetUbootEnvVars() is called after the image is actually
    // deleted from the BMC flash, there'd be a time window where the BMC would
    // be pointing to a non-existent image to boot from.
    // Need to remove the entries from the activations map before that call so
    // that resetUbootEnvVars() doesn't use the version to be deleted.
    auto iteratorActivations = activations.find(entryId);
    if (iteratorActivations == activations.end())
    {
        log<level::ERR>("Error: Failed to find version in item updater "
                        "activations map. Unable to remove.",
                        entry("VERSIONID=%s", entryId.c_str()));
    }
    else
    {
        removeAssociations(iteratorActivations->second->path);
        this->activations.erase(entryId);
    }
    ItemUpdater::resetUbootEnvVars();

    if (it != versions.end())
    {
        // Delete ReadOnly partitions if it's not active
        removeReadOnlyPartition(entryId);
        removePersistDataDirectory(entryId);

        // Removing entry in versions map
        this->versions.erase(entryId);
    }
    else
    {
        // Delete ReadOnly partitions even if we can't find the version
        removeReadOnlyPartition(entryId);
        removePersistDataDirectory(entryId);

        log<level::ERR>("Error: Failed to find version in item updater "
                        "versions map. Unable to remove.",
                        entry("VERSIONID=%s", entryId.c_str()));
    }

    helper.clearEntry(entryId);

    return;
}

void ItemUpdater::deleteAll()
{
    std::vector<std::string> deletableVersions;

    for (const auto& versionIt : versions)
    {
        if (!versionIt.second->isFunctional())
        {
            deletableVersions.push_back(versionIt.first);
        }
    }

    for (const auto& deletableIt : deletableVersions)
    {
        ItemUpdater::erase(deletableIt);
    }

    helper.cleanup();
}

ItemUpdater::ActivationStatus
    ItemUpdater::validateBIOSImage(const std::string& filePath)
{
    bool valid = true;


    imageUpdateList.clear();
    imageUpdateList.push_back(biosFullImages);
    valid = checkImage(filePath, imageUpdateList);
    if (!valid)
    {
        log<level::ERR>("Failed to find the needed BIOS images.");
        return ItemUpdater::ActivationStatus::invalid;
    }


    return ItemUpdater::ActivationStatus::ready;
}

ItemUpdater::ActivationStatus
    ItemUpdater::validateMCUImage(const std::string& filePath)
{
    bool valid = true;

    imageUpdateList.clear();
    imageUpdateList.push_back(mcuFullImages);
    valid = checkImage(filePath, imageUpdateList);
    if (!valid)
    {
        log<level::ERR>("Failed to find the needed MCU images.");
        return ItemUpdater::ActivationStatus::invalid;
    }


    return ItemUpdater::ActivationStatus::ready;
}

ItemUpdater::ActivationStatus
    ItemUpdater::validateSquashFSImage(const std::string& filePath)
{
    bool valid = true;

    // Record the images which are being updated
    // First check for the fullimage, then check for images with partitions
    imageUpdateList.clear();
    imageUpdateList.push_back(bmcFullImages);
    valid = checkImage(filePath, imageUpdateList);
    if (!valid)
    {
        imageUpdateList.clear();
        imageUpdateList.assign(bmcImages.begin(), bmcImages.end());
        valid = checkImage(filePath, imageUpdateList);
        if (!valid)
        {
            log<level::ERR>("Failed to find the needed BMC images.");
            return ItemUpdater::ActivationStatus::invalid;
        }
    }

    return ItemUpdater::ActivationStatus::ready;
}

void ItemUpdater::savePriority(const std::string& versionId, uint8_t value)
{
    storePriority(versionId, value);
    helper.setEntry(versionId, value);
}

void ItemUpdater::freePriority(uint8_t value, const std::string& versionId)
{
    std::map<std::string, uint8_t> priorityMap;

    // Insert the requested version and priority, it may not exist yet.
    priorityMap.insert(std::make_pair(versionId, value));

    for (const auto& intf : activations)
    {
        if (intf.second->redundancyPriority)
        {
            priorityMap.insert(std::make_pair(
                intf.first, intf.second->redundancyPriority.get()->priority()));
        }
    }

    // Lambda function to compare 2 priority values, use <= to allow duplicates
    typedef std::function<bool(std::pair<std::string, uint8_t>,
                               std::pair<std::string, uint8_t>)>
        cmpPriority;
    cmpPriority cmpPriorityFunc =
        [](std::pair<std::string, uint8_t> priority1,
           std::pair<std::string, uint8_t> priority2) {
            return priority1.second <= priority2.second;
        };

    // Sort versions by ascending priority
    std::set<std::pair<std::string, uint8_t>, cmpPriority> prioritySet(
        priorityMap.begin(), priorityMap.end(), cmpPriorityFunc);

    auto freePriorityValue = value;
    for (auto& element : prioritySet)
    {
        if (element.first == versionId)
        {
            continue;
        }
        if (element.second == freePriorityValue)
        {
            ++freePriorityValue;
            auto it = activations.find(element.first);
            it->second->redundancyPriority.get()->sdbusPriority(
                freePriorityValue);
        }
    }

    auto lowestVersion = prioritySet.begin()->first;
    if (value == prioritySet.begin()->second)
    {
        lowestVersion = versionId;
    }
    updateUbootEnvVars(lowestVersion);
}

void ItemUpdater::reset()
{
    constexpr auto setFactoryResetWait = std::chrono::seconds(10);
    helper.factoryReset();

    // Need to wait for env variables to complete, otherwise an immediate reboot
    // will not factory reset.
    std::this_thread::sleep_for(setFactoryResetWait);

    log<level::INFO>("BMC factory reset will take effect upon reboot.");
}

void ItemUpdater::removeReadOnlyPartition(std::string versionId)
{
    helper.removeVersion(versionId);
}

bool ItemUpdater::fieldModeEnabled(bool value)
{
    // enabling field mode is intended to be one way: false -> true
    if (value && !control::FieldMode::fieldModeEnabled())
    {
        control::FieldMode::fieldModeEnabled(value);

        auto method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                                          SYSTEMD_INTERFACE, "StartUnit");
        method.append("obmc-flash-bmc-setenv@fieldmode\\x3dtrue.service",
                      "replace");
        bus.call_noreply(method);

        method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                                     SYSTEMD_INTERFACE, "StopUnit");
        method.append("usr-local.mount", "replace");
        bus.call_noreply(method);

        std::vector<std::string> usrLocal = {"usr-local.mount"};

        method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                                     SYSTEMD_INTERFACE, "MaskUnitFiles");
        method.append(usrLocal, false, true);
        bus.call_noreply(method);
    }
    else if (!value && control::FieldMode::fieldModeEnabled())
    {
        elog<NotAllowed>(xyz::openbmc_project::Common::NotAllowed::REASON(
            "FieldMode is not allowed to be cleared"));
    }

    return control::FieldMode::fieldModeEnabled();
}

void ItemUpdater::restoreFieldModeStatus()
{
    std::system("fw_printenv > /tmp/env");
    std::ifstream input("/tmp/env");

    for (std::string envVar; getline(input, envVar); )
    {
        if (envVar.find("fieldmode=true") != std::string::npos)
        {
            ItemUpdater::fieldModeEnabled(true);
        }
    }
    std::system("rm /tmp/env");
}

void ItemUpdater::setHostInventoryPath()
{
    auto depth = 0;
    auto mapperCall = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                          MAPPER_INTERFACE, "GetSubTreePaths");

    mapperCall.append(INVENTORY_PATH);
    mapperCall.append(depth);
    std::vector<std::string> filter = {"xyz.openbmc_project.Inventory.Item.System"};
    mapperCall.append(filter);

    try
    {
        auto response = bus.call(mapperCall);

        using ObjectPaths = std::vector<std::string>;
        ObjectPaths result;
        response.read(result);

        if (!result.empty())
        {
            hostInventoryPath = result.front();
        }
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        log<level::ERR>("Error in mapper host GetSubTreePath");
        return;
    }

    return;
}

void ItemUpdater::setBMCInventoryPath()
{
    auto depth = 0;
    auto mapperCall = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                          MAPPER_INTERFACE, "GetSubTreePaths");

    mapperCall.append(INVENTORY_PATH);
    mapperCall.append(depth);
    std::vector<std::string> filter = {BMC_INVENTORY_INTERFACE};
    mapperCall.append(filter);

    try
    {
        auto response = bus.call(mapperCall);

        using ObjectPaths = std::vector<std::string>;
        ObjectPaths result;
        response.read(result);

        if (!result.empty())
        {
            bmcInventoryPath = result.front();
        }
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        log<level::ERR>("Error in mapper GetSubTreePath");
        return;
    }

    return;
}

void ItemUpdater::setMcuInventoryPath()
{
    auto depth = 0;
    auto mapperCall = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                          MAPPER_INTERFACE, "GetSubTreePaths");

    mapperCall.append(INVENTORY_PATH);
    mapperCall.append(depth);
    std::vector<std::string> filter = {"xyz.openbmc_project.Inventory.Item.Mcu"};
    mapperCall.append(filter);

    try
    {
        auto response = bus.call(mapperCall);

        using ObjectPaths = std::vector<std::string>;
        ObjectPaths result;
        response.read(result);

        if (!result.empty())
        {
            mcuInventoryPath = result.front();
        }
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        log<level::INFO>("Error in mapper Mcu GetSubTreePath");
        return;
    }

    return;
}

void ItemUpdater::createActiveAssociation(const std::string& path)
{
    assocs.emplace_back(
        std::make_tuple(ACTIVE_FWD_ASSOCIATION, ACTIVE_REV_ASSOCIATION, path));
    associations(assocs);
}

void ItemUpdater::createFunctionalAssociation(const std::string& path)
{
    assocs.emplace_back(std::make_tuple(FUNCTIONAL_FWD_ASSOCIATION,
                                        FUNCTIONAL_REV_ASSOCIATION, path));
    associations(assocs);
}

void ItemUpdater::createUpdateableAssociation(const std::string& path)
{
    assocs.emplace_back(std::make_tuple(UPDATEABLE_FWD_ASSOCIATION,
                                        UPDATEABLE_REV_ASSOCIATION, path));
    associations(assocs);
}

void ItemUpdater::removeAssociations(const std::string& path)
{
    for (auto iter = assocs.begin(); iter != assocs.end();)
    {
        if ((std::get<2>(*iter)).compare(path) == 0)
        {
            iter = assocs.erase(iter);
            associations(assocs);
        }
        else
        {
            ++iter;
        }
    }
}

bool ItemUpdater::isLowestPriority(uint8_t value)
{
    for (const auto& intf : activations)
    {
        if (intf.second->redundancyPriority)
        {
            if (intf.second->redundancyPriority.get()->priority() < value)
            {
                return false;
            }
        }
    }
    return true;
}

void ItemUpdater::updateUbootEnvVars(const std::string& versionId)
{
    helper.updateUbootVersionId(versionId);
}

void ItemUpdater::resetUbootEnvVars()
{
    decltype(activations.begin()->second->redundancyPriority.get()->priority())
        lowestPriority = std::numeric_limits<uint8_t>::max();
    decltype(activations.begin()->second->versionId) lowestPriorityVersion;
    for (const auto& intf : activations)
    {
        if (!intf.second->redundancyPriority.get())
        {
            // Skip this version if the redundancyPriority is not initialized.
            continue;
        }

        if (intf.second->redundancyPriority.get()->priority() <= lowestPriority)
        {
            lowestPriority = intf.second->redundancyPriority.get()->priority();
            lowestPriorityVersion = intf.second->versionId;
        }
    }

    // Update the U-boot environment variable to point to the lowest priority
    updateUbootEnvVars(lowestPriorityVersion);
}

void ItemUpdater::freeSpace(Activation& caller)
{
    //  Versions with the highest priority in front
    std::priority_queue<std::pair<int, std::string>,
                        std::vector<std::pair<int, std::string>>,
                        std::less<std::pair<int, std::string>>>
        versionsPQ;

    std::size_t count = 0;
    auto caller_purpose = versions.find(caller.versionId)->second.get()->purpose();
    for (const auto& iter : activations)
    {
        if ((iter.second.get()->activation() ==
             server::Activation::Activations::Active) ||
            (iter.second.get()->activation() ==
             server::Activation::Activations::Failed))
        {
            count++;
            // Don't put the functional version on the queue since we can't
            // remove the "running" BMC version.
            // If ACTIVE_BMC_MAX_ALLOWED <= 1, there is only one active BMC,
            // so remove functional version as well.
            // Don't delete the the Activation object that called this function.
            if ((versions.find(iter.second->versionId)
                     ->second->isFunctional() &&
                 ACTIVE_BMC_MAX_ALLOWED > 1) ||
                (iter.second->versionId == caller.versionId))
            {
                continue;
            }
            // Do not free different purpose image
            if (caller_purpose != versions.find(iter.second->versionId)
                    ->second.get()->purpose())
            {
                continue;
            }
            // keep BMC functional image after active new image for automatic test
            if (caller_purpose == VersionPurpose::BMC &&
                    versions.find(iter.second->versionId)->second->isFunctional())
            {
                continue;
            }

            // Failed activations don't have priority, assign them a large value
            // for sorting purposes.
            auto priority = 999;
            if (iter.second.get()->activation() ==
                    server::Activation::Activations::Active &&
                iter.second->redundancyPriority)
            {
                priority = iter.second->redundancyPriority.get()->priority();
            }

            versionsPQ.push(std::make_pair(priority, iter.second->versionId));
        }
    }

    // If the number of BMC versions is over ACTIVE_BMC_MAX_ALLOWED -1,
    // remove the highest priority one(s).
    while ((count >= ACTIVE_BMC_MAX_ALLOWED) && (!versionsPQ.empty()))
    {
        erase(versionsPQ.top().second);
        versionsPQ.pop();
        count--;
    }
}

void ItemUpdater::mirrorUbootToAlt()
{
    helper.mirrorAlt();
}

void ItemUpdater::updateHostVer(std::string version)
{
    if (version.empty())
    {
        log<level::ERR>("Host version must contains data");
        return;
    }
    log<level::INFO>(("Try to update host version: " + version).c_str());
    auto _verId = VersionClass::getId(version);
    std::list<std::string> nonActiveHostVids = {};
    std::string activeHostVid;
    // we must find from activations, only activation contains versionId data
    for (const auto& iter : activations)
    {
        auto _activation = iter.second->activation();
        auto _version = versions.find(iter.second->versionId);
        // cannot find mapping version
        if (_version == versions.end())
        {
            log<level::ERR>("Cannot find mapping version data",
                entry("VERSIONID=%s", iter.second->versionId.c_str()));
            continue;
        }
        // we only take care host version
        if (_version->second->purpose() == VersionPurpose::Host)
        {
            // collect host versions in active and non-active
            if (_activation == server::Activation::Activations::Active)
            {
                // there must only one host is active
                activeHostVid = iter.second->versionId;
            }
            else
            {
                nonActiveHostVids.emplace_back(iter.second->versionId);
            }
        }
    }

    char msgBuff[128];
    snprintf(msgBuff, sizeof(msgBuff),
        "Active host: %s, non-active host length: %d\n",
        activeHostVid.empty()?"":activeHostVid.c_str(),
        nonActiveHostVids.size());
    log<level::DEBUG>(msgBuff);
    // there already exist active host
    if (!activeHostVid.empty())
    {
        // remove old acitve version and create new one
        if (_verId != activeHostVid)
        {
            this->activations.erase(activeHostVid);
            this->versions.erase(activeHostVid);
            createHostVersion(version);
        }
        // same version id just ignore
    }
    else
    {
        // handle first time get host version
        if (nonActiveHostVids.size() == 0)
        {
            createHostVersion(version);
        }
        else
        {
            // here is error handling
            log<level::ERR>("there must exist one active host!!");
            auto host_it = std::find(
                nonActiveHostVids.begin(),nonActiveHostVids.end(), _verId);
            if (host_it == nonActiveHostVids.end())
            {
                // still try to create host version if version is different
                createHostVersion(version);
            }
            else
            {
                log<level::INFO >("ignore to craete same version in error activate");
            }
        }
    }
}

bool ItemUpdater::checkImage(const std::string& filePath,
                             const std::vector<std::string>& imageList)
{
    bool valid = true;

    for (auto& bmcImage : imageList)
    {
        fs::path file(filePath);
        file /= bmcImage;
        std::ifstream efile(file.c_str());
        if (efile.good() != 1)
        {
            valid = false;
            break;
        }
    }

    return valid;
}

#ifdef HOST_BIOS_UPGRADE
void ItemUpdater::createBIOSObject()
{
    std::string path = BIOS_OBJPATH;
    // Get version id from last item in the path
    auto pos = path.rfind("/");
    if (pos == std::string::npos)
    {
        log<level::ERR>("No version id found in object path",
                        entry("BIOS_OBJPATH=%s", path.c_str()));
        return;
    }

    createActiveAssociation(path);
    createFunctionalAssociation(path);

    auto versionId = path.substr(pos + 1);
    auto version = "null";
    AssociationList assocs = {};
    biosActivation = std::make_unique<Activation>(
        bus, path, *this, versionId, server::Activation::Activations::Active,
        assocs);
    auto dummyErase = [](std::string /*entryId*/) {
        // Do nothing;
    };
    biosVersion = std::make_unique<VersionClass>(
        bus, path, version, VersionPurpose::Host, "", "",
        std::bind(dummyErase, std::placeholders::_1));
    biosVersion->deleteObject =
        std::make_unique<phosphor::software::manager::Delete>(bus, path,
                                                              *biosVersion);
}
#endif

} // namespace updater
} // namespace software
} // namespace phosphor
