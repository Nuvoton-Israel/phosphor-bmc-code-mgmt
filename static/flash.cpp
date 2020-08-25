#include "config.h"

#include "flash.hpp"

#include "activation.hpp"
#include "activation_mcu.hpp"

#include "images.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <filesystem>

namespace
{
constexpr auto PATH_INITRAMFS = "/run/initramfs";
constexpr auto PATH_TMP       = "/tmp";
} // namespace

namespace phosphor
{
namespace software
{
namespace updater
{

namespace softwareServer = sdbusplus::xyz::openbmc_project::Software::server;
using namespace phosphor::logging;
using InternalFailure =
    sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;
auto constexpr FULL_IMAGE = "image-bmc";
auto constexpr BIOS_IMAGE = "image-bios";
auto constexpr MCU_IMAGE  = "image-mcu";
namespace fs = std::filesystem;

void Activation::flashWrite()
{
    // For static layout code update, just put images in /run/initramfs.
    // It expects user to trigger a reboot and an updater script will program
    // the image to flash during reboot.
    fs::path uploadDir(IMG_UPLOAD_DIR);
    fs::path toPath(PATH_INITRAMFS);

    if ( fs::exists(uploadDir / versionId / FULL_IMAGE))
    {
        fs::copy_file(uploadDir / versionId / FULL_IMAGE, toPath / FULL_IMAGE,
                            fs::copy_options::overwrite_existing);
        return;
    }
    for (auto& bmcImage : phosphor::software::image::bmcImages)
    {
        if ( fs::exists(uploadDir / versionId / bmcImage))
        {
            fs::copy_file(uploadDir / versionId / bmcImage, toPath / bmcImage,
                            fs::copy_options::overwrite_existing);
        }
    }
}

void Activation::onStateChanges(sdbusplus::message::message& /*msg*/)
{
    // Empty
}

void HostActivation::flashWrite()
{
    log<level::DEBUG>("HostActivation::flashWrite");
    fs::path uploadDir(IMG_UPLOAD_DIR);
    fs::path toPath(PATH_TMP);

    if (fs::exists(uploadDir / versionId / BIOS_IMAGE))
    {
        fs::copy_file(uploadDir / versionId / BIOS_IMAGE, toPath / BIOS_IMAGE,
                          fs::copy_options::overwrite_existing);
    }
    else
    {
        log<level::ERR>("Cannot find BIOS images");
        report<InternalFailure>();
        HostActivation::activation(
            softwareServer::Activation::Activations::Failed);
        return;
    }
    auto method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                            SYSTEMD_INTERFACE, "StartUnit");
    method.append(flashBiosServiceFile, "replace");
    try
    {
        bus.call_noreply(method);
    }
    catch(const std::exception& e)
    {
        log<level::ERR>("Error in starting flash bios service",
            entry("WHAT=%s", e.what()));
        HostActivation::activation(
            softwareServer::Activation::Activations::Failed);
    }
}

void HostActivation::onStateChanges(sdbusplus::message::message& msg)
{
    uint32_t newStateID{};
    sdbusplus::message::object_path newStateObjPath;
    std::string newStateUnit{};
    std::string newStateResult{};

    // Read the msg and populate each variable
    msg.read(newStateID, newStateObjPath, newStateUnit, newStateResult);
    if (newStateUnit == flashBiosServiceFile)
    {
        log<level::DEBUG>("HostActivation::onStateChanges",
            entry("ID=%d", newStateID),
            entry("STATE=%s", newStateResult.c_str()));
        // Result string will be one of done, canceled, timeout, failed,
        // dependency, or skipped.
        if (newStateResult == "done")
        {
            biosFlashed = true;
            activationProgress->progress(activationProgress->progress() + 50);
            HostActivation::activation(
                softwareServer::Activation::Activations::Activating);
        }
        else
        {
            auto msg = "service:" + newStateUnit + " return result: " + newStateResult;
            log<level::ERR>(msg.c_str(), entry("UNIT=%s", newStateUnit.c_str()),
                    entry("STATE=%s", newStateResult.c_str()));
            HostActivation::activation(
                softwareServer::Activation::Activations::Failed);
        }
    }
}

void McuActivation::flashWrite()
{
    log<level::DEBUG>("McuActivation::flashWrite");
    fs::path uploadDir(IMG_UPLOAD_DIR);
    fs::path toPath(PATH_TMP);

    if (fs::exists(uploadDir / versionId / MCU_IMAGE))
    {
        fs::copy_file(uploadDir / versionId / MCU_IMAGE, toPath / MCU_IMAGE,
                          fs::copy_options::overwrite_existing);
    }
    else
    {
        log<level::ERR>("Cannot find MCU images");
        report<InternalFailure>();
        McuActivation::activation(
            softwareServer::Activation::Activations::Failed);
        return;
    }
    auto method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                            SYSTEMD_INTERFACE, "StartUnit");
    method.append(flashMcuServiceFile, "replace");
    try
    {
        bus.call_noreply(method);
    }
    catch(const std::exception& e)
    {
        log<level::ERR>("Error in starting flash mcu service",
            entry("WHAT=%s", e.what()));
        McuActivation::activation(
            softwareServer::Activation::Activations::Failed);
    }
}

void McuActivation::onStateChanges(sdbusplus::message::message& msg)
{
    uint32_t newStateID{};
    sdbusplus::message::object_path newStateObjPath;
    std::string newStateUnit{};
    std::string newStateResult{};

    // Read the msg and populate each variable
    msg.read(newStateID, newStateObjPath, newStateUnit, newStateResult);
    if (newStateUnit == flashMcuServiceFile)
    {
        log<level::DEBUG>("McuActivation::onStateChanges",
            entry("ID=%d", newStateID),
            entry("STATE=%s", newStateResult.c_str()));
        // Result string will be one of done, canceled, timeout, failed,
        // dependency, or skipped.
        if (newStateResult == "done")
        {
            mcuFlashed = true;
            activationProgress->progress(activationProgress->progress() + 50);
            McuActivation::activation(
                softwareServer::Activation::Activations::Activating);
        }
        else
        {
            auto msg = "service:" + newStateUnit + " return result: " + newStateResult;
            log<level::ERR>(msg.c_str(), entry("UNIT=%s", newStateUnit.c_str()),
                    entry("STATE=%s", newStateResult.c_str()));
            McuActivation::activation(
                softwareServer::Activation::Activations::Failed);
        }
    }
}

} // namespace updater
} // namespace software
} // namespace phosphor
