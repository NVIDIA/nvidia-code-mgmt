#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test::fw_status_fake_gpio
{

struct LineState
{
    bool present = true;
    bool throwOnRequest = false;
    bool throwOnRelease = false;
    bool throwOnSet = false;
    bool requested = false;
    bool released = false;
    int requestCount = 0;
    int releaseCount = 0;
    int requestDefault = -1;
    int getValue = 0;
    int eventFd = 10;
    int eventType = 1;
    std::vector<int> setValues;
};

inline std::map<std::string, LineState> lines{};

inline void reset()
{
    lines.clear();
}

} // namespace test::fw_status_fake_gpio

namespace gpiod
{

struct line_request
{
    enum direction
    {
        DIRECTION_INPUT = 0,
        DIRECTION_OUTPUT = 1,
        EVENT_BOTH_EDGES = 2,
    };

    struct config
    {
        std::string consumer;
        direction request_type;
        int flags;
    };
};

struct line_event
{
    enum Type
    {
        RISING_EDGE = 1,
        FALLING_EDGE = 2,
    };

    Type event_type = RISING_EDGE;
};

class line
{
  public:
    enum
    {
        ACTIVE_HIGH = 0,
        ACTIVE_LOW = 1,
    };

    line() = default;

    line(std::string lineName, bool validLine) :
        name(std::move(lineName)), valid(validLine)
    {}

    explicit operator bool() const noexcept
    {
        return valid;
    }

    void request(const line_request::config&, int defaultValue = 0) const
    {
        auto& state = stateFor();
        if (state.throwOnRequest)
        {
            throw std::runtime_error("fake gpio request failure");
        }
        state.requested = true;
        state.requestDefault = defaultValue;
        ++state.requestCount;
    }

    void release() const
    {
        auto& state = stateFor();
        if (state.throwOnRelease)
        {
            throw std::runtime_error("fake gpio release failure");
        }
        state.released = true;
        ++state.releaseCount;
    }

    void set_value(int value) const
    {
        auto& state = stateFor();
        if (state.throwOnSet)
        {
            throw std::runtime_error("fake gpio set failure");
        }
        state.setValues.push_back(value);
    }

    int get_value() const
    {
        const auto& state = stateFor();
        if (!state.setValues.empty())
        {
            return state.setValues.back();
        }
        return state.getValue;
    }

    int event_get_fd() const
    {
        return stateFor().eventFd;
    }

    line_event event_read() const
    {
        line_event event;
        event.event_type = static_cast<line_event::Type>(stateFor().eventType);
        return event;
    }

  private:
    test::fw_status_fake_gpio::LineState& stateFor() const
    {
        if (!valid)
        {
            throw std::runtime_error("invalid fake gpio line");
        }
        return test::fw_status_fake_gpio::lines[name];
    }

    std::string name{};
    bool valid = false;
};

inline line find_line(const std::string& name)
{
    auto it = test::fw_status_fake_gpio::lines.find(name);
    if (it == test::fw_status_fake_gpio::lines.end() || !it->second.present)
    {
        return line(name, false);
    }
    return line(name, true);
}

} // namespace gpiod
