#pragma once

#include <sys/inotify.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <phosphor-logging/log.hpp>
#include <stdexcept>
#include <string>

namespace nvidia
{
namespace software
{
namespace updater
{

/** @class Watch
 *
 *  @brief Adds inotify watch on software image upload directory
 *
 *  The inotify watch is hooked up with sd-event, so that on call back,
 *  appropriate actions related to a software image upload can be taken.
 */
class Watch
{
  public:
    /** @brief ctor - hook inotify watch with sd-event
     *
     *  @param[in] loop - sd-event object
     *  @param[in] imageCallback - The callback function for processing
     *                             the image
     */
    Watch(sd_event* loop, std::vector<std::filesystem::path> pathsToMonitor,
          std::function<int(std::string&)> imageCallback);

    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
    Watch(Watch&&) = delete;
    Watch& operator=(Watch&&) = delete;

    /** @brief dtor - remove inotify watch and close fd's
     */
    ~Watch();

    /** @brief image upload directory watch descriptor */
    // int wd = -1;
    std::map<int, std::string> wds;

  private:
    /** @brief sd-event callback
     *
     *  @param[in] s - event source, floating (unused) in our case
     *  @param[in] fd - inotify fd
     *  @param[in] revents - events that matched for fd
     *  @param[in] userdata - pointer to Watch object
     *  @returns 0 on success, -1 on fail
     */
    static int callback(sd_event_source* s, int fd, uint32_t revents,
                        void* userdata);

    /** @brief inotify file descriptor */
    int fd = -1;

    /** @brief The callback function for processing the image. */
    std::function<int(std::string&)> imageCallback;
};

} // namespace updater
} // namespace software
} // namespace nvidia
