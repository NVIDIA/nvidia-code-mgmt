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



#pragma once
#include "config.h"

#include "base_item_updater.hpp"

namespace nvidia
{
namespace software
{
namespace updater
{

/*
 * BaseController
 */
class BaseController
{
  protected:
    /**
     * @var BaseItemUpdater
     */
    std::unique_ptr<BaseItemUpdater> abstractItemUpdater_;

  public:
    /**
     * @brief constructor
     * @param abstractItemUpdater
     */
    BaseController(std::unique_ptr<BaseItemUpdater>& abstractItemUpdater) :
        abstractItemUpdater_(std::move(abstractItemUpdater))
    {}

    /**
     * @brief destructor
     * @return
     */
    virtual ~BaseController()
    {}

    /**
     * @brief Get the Name item updater
     *
     * @return std::string
     */
    virtual std::string getName() const
    {
        return abstractItemUpdater_->getName();
    }

    /**
     * @brief Process the updated image
     *
     * @param filePath
     * @return int
     */
    int processImage(const std::string& filePath);

    /**
     * @brief Starts watching the set dir for new image
     *
     * @param bus
     * @param loop
     * @return int
     */
    int startWatching(sdbusplus::bus::bus& bus, sd_event* loop);

    /**
     * @brief Updates devices using existing images
     *
     * @return int
     */
    int processExistingImages();
};
} // namespace updater
} // namespace software
} // namespace nvidia
