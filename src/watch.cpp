#include "config.h"

#include "watch.hpp"

#include <sys/inotify.h>
#include <unistd.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <phosphor-logging/log.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
namespace nvidia
{
namespace software
{
namespace updater
{

using namespace phosphor::logging;
using namespace std::string_literals;
namespace fs = std::filesystem;

Watch::Watch(sd_event* loop, std::vector<std::filesystem::path> pathsToMonitor,
             std::function<int(std::string&)> imageCallback) :
    imageCallback(imageCallback)
{
    fd = inotify_init1(IN_NONBLOCK);
    if (-1 == fd)
    {
        // Store a copy of errno, because the string creation below will
        // invalidate errno due to one more system calls.
        auto error = errno;
        throw std::runtime_error("inotify_init1 failed, errno="s +
                                 std::strerror(error));
    }

    for (const std::filesystem::path& path : pathsToMonitor)
    {
        if (std::filesystem::exists(path))
        {
            std::filesystem::remove_all(path);
        }
        // Create Directory
        std::filesystem::create_directories(path);
        // Add to watch
        auto wd = inotify_add_watch(fd, path.c_str(), IN_CLOSE_WRITE);
        if (-1 == wd)
        {
            auto error = errno;
            close(fd);
            throw std::runtime_error("inotify_add_watch failed, errno="s +
                                     std::strerror(error));
        }
        wds[wd] = path;
    }
    auto rc = sd_event_add_io(loop, nullptr, fd, EPOLLIN, callback, this);
    if (0 > rc)
    {
        throw std::runtime_error("failed to add to event loop, rc="s +
                                 std::strerror(-rc));
    }
}

Watch::~Watch()
{
    if (-1 != fd)
    {
        for (std::map<int, std::string>::iterator itr = wds.begin();
             itr != wds.end(); ++itr)
        {
            inotify_rm_watch(fd, itr->first);
        }
        close(fd);
    }
}

int Watch::callback(sd_event_source* /* s */, int fd, uint32_t revents,
                    void* userdata)
{
    if (!(revents & EPOLLIN))
    {
        return 0;
    }

    constexpr auto maxBytes = 1024;
    uint8_t buffer[maxBytes];
    auto bytes = read(fd, buffer, maxBytes);
    if (0 > bytes)
    {
        auto error = errno;
        throw std::runtime_error("failed to read inotify event, errno="s +
                                 std::strerror(error));
    }

    auto offset = 0;
    while (offset < bytes)
    {
        auto event = reinterpret_cast<inotify_event*>(&buffer[offset]);
        if ((event->mask & IN_CLOSE_WRITE) && !(event->mask & IN_ISDIR))
        {
            auto parentPath = static_cast<Watch*>(userdata)->wds[event->wd];
            auto imagePath = parentPath + '/' + event->name;
            auto rc = static_cast<Watch*>(userdata)->imageCallback(imagePath);

            if (rc < 0)
            {
                log<level::ERR>("Error processing image",
                                entry("IMAGE=%s", imagePath.c_str()));
            }
        }
        offset += offsetof(inotify_event, name) + event->len;
        if (0 >= offset) // check to ensure tainted input from buffer
        {
            break;
        }
    }

    return 0;
}

} // namespace updater
} // namespace software
} // namespace nvidia
