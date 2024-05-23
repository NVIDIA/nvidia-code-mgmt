/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
#ifdef ORIN_FLASH_SUPPORT
#include "orin_updater.hpp"
#endif
#ifdef SMCU_FLASH_SUPPORT
#include "smcu_updater.hpp"
#endif
#ifdef DEBUG_TOKEN_SUPPORT
#include "debug_token_install.hpp"
#include "debug_token_erase.hpp"
#endif
#if MTD_SUPPORT
#include "mtd_updater.hpp"
#endif
#if SWITCHTEC_FUSE_SUPPORT
#include "switchtec_fuse.hpp"
#endif
#if JAMPLAYER_SUPPORT
#include "jamplayer.hpp"
#endif

#include "watch.hpp"

#include <getopt.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <cstdlib>
#include <exception>

using namespace phosphor::logging;

static struct option set_opts[] = {
    {"updater", required_argument, NULL, 'u'},
    {"fallback", no_argument, NULL, 'f'},
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
    bool useFallback = false;
    (void)useFallback; // if RT_SUPPORT is disabled this isn't used currently
    std::string targetName = "";
    std::string modelName = "";
    while ((ret = getopt_long(argc, argv, "u:i:m:f", set_opts, NULL)) != -1)
    {
        switch (ret)
        {
            case 'u':
                updater = optarg;
                break;
            case 'i':
                targetName = optarg;
                break;
            case 'm':
                modelName = optarg;
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
        /* default option is to do update together, if fallback is specified
           then we use the single updater */
        itemUpdater = std::make_unique<ReTimerItemUpdater>(bus, !useFallback);
    }
#endif
#if PEX_SUPPORT
    if (updater == "PEX")
    {
        itemUpdater = std::make_unique<PEXItemUpdater>(bus);
    }
#endif
#if ORIN_FLASH_SUPPORT
    if (updater == "ORIN")
    {
        itemUpdater = std::make_unique<ORINItemUpdater>(bus);
    }
#endif
#if SMCU_FLASH_SUPPORT
    if (updater == "SMCU")
    {
        itemUpdater = std::make_unique<SMCUItemUpdater>(bus);
    }
#endif
#if DEBUG_TOKEN_SUPPORT
    try
    {
        if (updater == "DebugTokenInstall")
        {
            itemUpdater = std::make_unique<DebugTokenInstallItemUpdater>(bus);
        }
        else if (updater == "DebugTokenErase")
        {
            itemUpdater = std::make_unique<DebugTokenEraseItemUpdater>(bus);
        }
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("Debug token object exception",
                        entry("TOKEN_EXCEPTION=%s", e.what()));
        exit(EXIT_FAILURE);
    }
#endif
#if MTD_SUPPORT
    if (updater == "MTD")
    {
        itemUpdater = std::make_unique<MTDItemUpdater>(bus, targetName, modelName);
    }
#endif
#if SWITCHTEC_FUSE_SUPPORT
    if (updater == "SWITCHTEC")
    {
        itemUpdater = std::make_unique<SwitchtecFuse>(bus);
    }
#endif
#if JAMPLAYER_SUPPORT
    if (updater == "JAMPLAYER")
    {
        itemUpdater = std::make_unique<JamPlayer>(bus);
    }
#endif

    if (itemUpdater == nullptr)
    {
        printf("UnSupported Updater \"%s\"\n", updater.c_str());
        exit(EXIT_FAILURE);
    }
    try
    {
        bus.request_name(itemUpdater->getBusName().c_str());
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        log<level::ERR>("Error while getting service name",
            entry("ERROR=%s", e.what()));
        return -1;
    }
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
