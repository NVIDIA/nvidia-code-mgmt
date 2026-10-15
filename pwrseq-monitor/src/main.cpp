/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * lpu-pwrseq-monitord entry point.
 *
 * Single-threaded sd-event loop (design §7.1). Loads the platform descriptor,
 * constructs the orchestrator, which instantiates the FSM hierarchy + HAL
 * drivers and registers their event fds with the loop. Runs until
 * SIGTERM / SIGINT.
 *
 * CLI:
 *   --config <path>   Path to the platform descriptor JSON.
 *                     Default: /usr/share/lpu-pwrseq-monitor/platform.json
 */

#include "config/descriptor.hpp"
#include "dbus_manager.hpp"
#include "fsm/state_dump.hpp"
#include "gpio_monitor_client.hpp"
#include "orchestrator.hpp"

#include <systemd/sd-event.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace
{

constexpr const char* kDefaultDescriptorPath =
    "/usr/share/lpu-pwrseq-monitor/platform.json";

int onSigInt(sd_event_source* /*src*/, const struct signalfd_siginfo*,
             void* userdata)
{
    auto* loop = static_cast<sd_event*>(userdata);
    lg2::info("pwrseq-monitor: signal received, exiting");
    sd_event_exit(loop, 0);
    return 0;
}

int onSigUsr1(sd_event_source* /*src*/, const struct signalfd_siginfo*,
              void* userdata)
{
    auto* orch = static_cast<pwrseq::Orchestrator*>(userdata);
    orch->dumpState(pwrseq::kFsmDumpPath);
    return 0;
}

struct CliOptions
{
    std::string configPath = kDefaultDescriptorPath;
};

void printUsage(const char* prog)
{
    lg2::info("Usage: {PROG} [--config <path>]\n"
              "  --config <path>   Platform descriptor JSON "
              "(default: /usr/share/lpu-pwrseq-monitor/platform.json)",
              "PROG", prog);
}

bool parseArgs(int argc, char** argv, CliOptions& opts)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--config") == 0)
        {
            if (i + 1 >= argc)
            {
                lg2::error("--config requires a path argument");
                return false;
            }
            opts.configPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "-h") == 0 ||
                 std::strcmp(argv[i], "--help") == 0)
        {
            printUsage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        else
        {
            lg2::error("Unknown argument: {ARG}", "ARG", argv[i]);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    CliOptions opts;
    if (!parseArgs(argc, argv, opts))
    {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    lg2::info("pwrseq-monitor: starting up (PID={PID}, config={CFG})", "PID",
              static_cast<int>(getpid()), "CFG", opts.configPath);

    // 1. Load the platform descriptor. Bring the service down hard if the
    //    descriptor is malformed — silently mis-configuring a tray is worse
    //    than failing to start.
    pwrseq::config::PlatformDescriptor desc;
    try
    {
        desc = pwrseq::config::loadFromFile(opts.configPath);
    }
    catch (const pwrseq::config::DescriptorError& e)
    {
        lg2::error("Failed to load platform descriptor from {CFG}: {ERR}",
                   "CFG", opts.configPath, "ERR", e.what());
        return EXIT_FAILURE;
    }

    // 2. sd-event loop
    sd_event* loop = nullptr;
    if (int rc = sd_event_default(&loop); rc < 0)
    {
        lg2::error("sd_event_default failed: {RC}", "RC", rc);
        return EXIT_FAILURE;
    }
    std::unique_ptr<sd_event, decltype(&sd_event_unref)> loopGuard(
        loop, sd_event_unref);

    // 3. Signal handlers. If any of these steps fails the daemon ends up
    // with SIGINT/SIGTERM blocked and no graceful-shutdown path -- a
    // wedged daemon that systemd can only SIGKILL on stop, with no
    // chance to release GPIO holds. Fail fast instead.
    sigset_t ss;
    if (sigemptyset(&ss) != 0)
    {
        lg2::error("sigemptyset failed: {ERR}", "ERR", std::strerror(errno));
        return EXIT_FAILURE;
    }
    if (sigaddset(&ss, SIGINT) != 0 || sigaddset(&ss, SIGTERM) != 0 ||
        sigaddset(&ss, SIGUSR1) != 0)
    {
        lg2::error("sigaddset failed: {ERR}", "ERR", std::strerror(errno));
        return EXIT_FAILURE;
    }
    if (sigprocmask(SIG_BLOCK, &ss, nullptr) != 0)
    {
        lg2::error("sigprocmask(SIG_BLOCK) failed: {ERR}", "ERR",
                   std::strerror(errno));
        return EXIT_FAILURE;
    }
    if (int rc = sd_event_add_signal(loop, nullptr, SIGINT, onSigInt, loop);
        rc < 0)
    {
        lg2::error("sd_event_add_signal(SIGINT) failed: {ERR}", "ERR",
                   std::strerror(-rc));
        return EXIT_FAILURE;
    }
    if (int rc = sd_event_add_signal(loop, nullptr, SIGTERM, onSigInt, loop);
        rc < 0)
    {
        lg2::error("sd_event_add_signal(SIGTERM) failed: {ERR}", "ERR",
                   std::strerror(-rc));
        return EXIT_FAILURE;
    }

    // 4. sdbusplus bus — claim the well-known name only after the
    // orchestrator and manager methods are registered. Type=dbus marks
    // the unit active at request_name(); gpio-mon property seeding must
    // not start until that point or initial Value updates are lost.
    auto bus = sdbusplus::bus::new_default();
    bus.attach_event(loop, SD_EVENT_PRIORITY_NORMAL);

    // 5. Orchestrator (descriptor-driven)
    pwrseq::Orchestrator orch(bus, loop);
    if (orch.initFromDescriptor(desc) != pwrseq::hal::Result::Ok)
    {
        lg2::error("Orchestrator initFromDescriptor() failed");
        // Continue anyway — drivers that did initialise will still report.
    }

    // 5b. D-Bus method interface (com.Nvidia.PwrSeqMonitor.Manager).
    pwrseq::DBusManager dbus_mgr(bus, orch, orch.lines());

    // SIGUSR1: dump-manager / dreport asks for a live FSM snapshot.
    if (int rc = sd_event_add_signal(loop, nullptr, SIGUSR1, onSigUsr1, &orch);
        rc < 0)
    {
        lg2::error("sd_event_add_signal(SIGUSR1) failed: {ERR}", "ERR",
                   std::strerror(-rc));
        return EXIT_FAILURE;
    }

    bus.request_name("com.Nvidia.PwrSeqMonitor");
    // Match PropertiesChanged before GetManagedObjects snapshot (inside).
    // Kept alive for the daemon lifetime (owns sd-bus match slots).
    auto gpioSub = pwrseq::startGpioPropertySubscriber(bus, desc, dbus_mgr);

    lg2::info("pwrseq-monitor: ready; entering event loop "
              "(platform={PLAT})",
              "PLAT", desc.platform);

    // 6. Run
    if (int rc = sd_event_loop(loop); rc < 0)
    {
        lg2::error("sd_event_loop returned {RC}", "RC", rc);
        return EXIT_FAILURE;
    }

    lg2::info("pwrseq-monitor: shutdown clean");
    return EXIT_SUCCESS;
}
