/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "reporting/resource_interfaces.hpp"

namespace pwrseq::reporting
{

void Reporter::applyStatus(ElementState s, bool present)
{
    writeTuple_(toStatus(s, present));
}

void Reporter::applyStatus(ModuleState s)
{
    writeTuple_(toStatus(s));
}

void Reporter::applyStatus(TrayState s)
{
    writeTuple_(toStatus(s));
}

} // namespace pwrseq::reporting
