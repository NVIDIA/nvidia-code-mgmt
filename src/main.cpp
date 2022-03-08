#include "config.h"

#include "base_controller.hpp"
#ifdef PSU_SUPPORT
#include "psu_updater.hpp"
#endif
#ifdef FPGA_SUPPORT
#include "fpga_updater.hpp"
#endif
#ifdef CPLD_SUPPORT
#include "cpld_updater.hpp"
#endif
#ifdef RT_SUPPORT
#include "retimer_updater.hpp"
#endif
#ifdef PEX_SUPPORT
#include "pex_updater.hpp"
#endif
#include "watch.hpp"

#include <getopt.h>

#include <cstdlib>
#include <exception>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

static struct option set_opts[] = {
    {"updater", required_argument, NULL, 'u'},
    {},
};
static void print_wrong_arg_exit(void)
{
    printf("invalid or unknown argument\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char** argv)
{
    std::string updater = "";
    auto ret = 0;
    while ((ret = getopt_long(argc, argv, "u:", set_opts, NULL)) != -1)
    {
        switch (ret)
        {
            case 'u':
                updater = optarg;
                break;
            default:
                print_wrong_arg_exit();
        }
    }

    using namespace nvidia::software::updater;
    auto bus = sdbusplus::bus::new_default();

    sd_event* loop = nullptr;
    sd_event_default(&loop);

    sdbusplus::server::manager::manager objManager(bus, SOFTWARE_OBJPATH);

    std::unique_ptr<BaseController> abstractController;
    std::unique_ptr<BaseItemUpdater> itemUpdater;

#if PSU_SUPPORT
    if (updater == "PSU")
    {
        itemUpdater = std::make_unique<PSUItemUpdater>(bus);
    }
#endif
#if FPGA_SUPPORT
    if (updater == "FPGA")
    {
        itemUpdater = std::make_unique<FPGAItemUpdater>(bus);
    }
#endif
#if CPLD_SUPPORT
    if (updater == "CPLD")
    {
        itemUpdater = std::make_unique<CPLDItemUpdater>(bus);
    }
#endif
#if RT_SUPPORT
    if (updater == "Retimer")
    {
        itemUpdater = std::make_unique<ReTimerItemUpdater>(bus);
    }
#endif
#if PEX_SUPPORT
    if (updater == "PEX")
    {
        itemUpdater = std::make_unique<PEXItemUpdater>(bus);
    }
#endif
    if (itemUpdater == nullptr)
    {
        printf("UnSupported Updater \"%s\"\n", updater.c_str());
        exit(EXIT_FAILURE);
    }
    bus.request_name(itemUpdater->getBusName().c_str());
    abstractController = std::make_unique<BaseController>(itemUpdater);

    if (abstractController->processExistingImages())
    {
        return -1;
    }
    if (abstractController->startWatching(bus, loop) == -1)
    {
        return -1;
    }

    sd_event_unref(loop);

    return 0;
}
