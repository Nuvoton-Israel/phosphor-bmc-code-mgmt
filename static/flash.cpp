#include "config.h"

#include "flash.hpp"

#include "activation.hpp"
#include "images.hpp"

#include <experimental/filesystem>
#include <phosphor-logging/log.hpp>

namespace
{
constexpr auto PATH_INITRAMFS = "/run/initramfs";
} // namespace

namespace phosphor
{
namespace software
{
namespace updater
{
using namespace phosphor::logging;
namespace fs = std::experimental::filesystem;

void Activation::flashWrite()
{
    // For static layout code update, just put images in /run/initramfs.
    // It expects user to trigger a reboot and an updater script will program
    // the image to flash during reboot.
    fs::path uploadDir(IMG_UPLOAD_DIR);
    fs::path toPath(PATH_INITRAMFS);

    for (auto& bmcImage : phosphor::software::image::bmcImages)
    {
        if ( fs::exists(uploadDir / versionId / bmcImage))
        {
            if(bmcImage.compare("image-bios")==0)
            {
                fs::copy_file(uploadDir / versionId / bmcImage, "/tmp/bios-image",
                              fs::copy_options::overwrite_existing);

                std::string serviceFile = "phosphor-ipmi-flash-bios-update.service";
                auto method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                                      SYSTEMD_INTERFACE, "StartUnit");
                method.append(serviceFile, "replace");
                try
                {
                    auto reply = bus.call(method);
                }
                catch(const std::exception& e)
                {
                    log<level::ERR>("Error in starting flash bios service",
                        entry("WHAT=%s", e.what()));
                }
            }
            else
                fs::copy_file(uploadDir / versionId / bmcImage, toPath / bmcImage,
                              fs::copy_options::overwrite_existing);
        }
    }
}

void Activation::onStateChanges(sdbusplus::message::message& /*msg*/)
{
    // Empty
}

} // namespace updater
} // namespace software
} // namespace phosphor
