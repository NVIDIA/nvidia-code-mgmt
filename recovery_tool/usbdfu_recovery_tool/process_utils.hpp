// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#pragma once

#include <optional>
#include <string>
#include <vector>

/**
 * Child-process helpers shared by the USB DFU recovery tool.  Every command is
 * wrapped in timeout(1) so a hung dfu-util cannot stall the recovery task.
 */
namespace usbdfu::process
{

/**
 * Execute argv (argv[0] = program path) and capture its stdout and stderr
 * (merged).  The pipe is drained to EOF before the child is reaped, so a
 * chatty command cannot block on a full pipe.  Returns the
 * output when the process ran to completion (any exit code), nullopt when it
 * could not be started or was killed by the timeout.  The exit status is
 * reported through `exitCode` when non-null.
 */
std::optional<std::string> capture(const std::vector<std::string>& argv,
                                   int timeoutSecs,
                                   int* exitCode = nullptr) noexcept;

} // namespace usbdfu::process
