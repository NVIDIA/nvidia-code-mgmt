// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

// mst-recovery: mstflint-over-I2C firmware recovery for ConnectX, NVSwitch
// and NVLink management NIC devices. Started by connectx-recovery@,
// nvswitch-recovery@ and nvlinkmgmt-nic-recovery@ with the image path.
// The settings of each --device-type are compiled in; the platform facts
// come from the Entity Manager MSTRecoveryPolicy record of that type.

#include "mst_platform_real.hpp"
#include "mst_recovery.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::string program = argc > 0 ? argv[0] : "mst-recovery";
    if (auto pos = program.find_last_of('/'); pos != std::string::npos)
    {
        program = program.substr(pos + 1);
    }

    if (argc < 3 || std::strcmp(argv[1], "--device-type") != 0)
    {
        std::fprintf(stderr,
                     "usage: %s --device-type TYPE [-f | --force] "
                     "FW_IMAGE_PATH\n",
                     program.c_str());
        return 1;
    }
    std::string deviceType = argv[2];
    std::vector<std::string> args(argv + 3, argv + argc);
    // A bad command line does not wait for Entity Manager; with a record
    // the flow reports it in its own format
    bool argsOk =
        args.size() == 1 ||
        (args.size() == 2 && (args[0] == "-f" || args[0] == "--force"));

    auto platform = mst::makePlatform();
    auto policy = mst::findPolicy(*platform, deviceType, argsOk ? 10 : 1);
    if (!policy && !argsOk)
    {
        std::fprintf(stderr, "usage: %s [-f | --force] FW_IMAGE_PATH\n",
                     program.c_str());
        return 1;
    }
    if (!policy)
    {
        lg2::error("{TYPE} recovery: {ERROR}", "TYPE", deviceType, "ERROR",
                   policy.error());
        platform->createLog(
            "ResourceEvent.1.0.ResourceErrorsDetected",
            "xyz.openbmc_project.Logging.Entry.Level.Critical",
            {{"REDFISH_MESSAGE_ID", "ResourceEvent.1.0.ResourceErrorsDetected"},
             {"REDFISH_MESSAGE_ARGS",
              deviceType + " recovery,No valid MSTRecoveryPolicy record"},
             {"namespace", "FWUpdate"}});
        return 1;
    }
    if (policy->flow == mst::Flow::Hold)
    {
        mst::Recovery::installSignalHandlers();
    }
    mst::Recovery recovery(*platform, std::move(*policy), program);
    return recovery.run(std::move(args));
}
