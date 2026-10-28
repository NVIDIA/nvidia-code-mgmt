// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#include "mst_platform_real.hpp"

namespace mst
{

std::unique_ptr<Platform> makePlatform()
{
    return std::make_unique<RealPlatform>();
}

} // namespace mst
