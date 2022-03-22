

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
