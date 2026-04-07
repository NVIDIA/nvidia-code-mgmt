#pragma once

#include <optional>
#include <stdexcept>
#include <string>

namespace test::mcu_fake_bus
{

template <typename T>
struct ReplySlot
{
    static inline std::optional<T> value = std::nullopt;
};

inline std::string errorMessage{};

template <typename T>
inline void setReply(const T& value)
{
    ReplySlot<T>::value = value;
    errorMessage.clear();
}

inline void setError(std::string message)
{
    errorMessage = std::move(message);
}

template <typename T>
inline void resetReply()
{
    ReplySlot<T>::value = std::nullopt;
}

inline void reset()
{
    errorMessage.clear();
}

} // namespace test::mcu_fake_bus

namespace sdbusplus::exception
{

class SdBusError : public std::runtime_error
{
  public:
    explicit SdBusError(const std::string& what) : std::runtime_error(what)
    {}
};

} // namespace sdbusplus::exception

namespace sdbusplus::bus
{

class message
{
  public:
    template <typename... Args>
    void append(Args&&...)
    {}
};

class reply
{
  public:
    template <typename T>
    void read(T& out) const
    {
        if (!test::mcu_fake_bus::errorMessage.empty())
        {
            throw sdbusplus::exception::SdBusError(
                test::mcu_fake_bus::errorMessage);
        }

        const auto& value = test::mcu_fake_bus::ReplySlot<T>::value;
        out = value ? *value : T{};
    }
};

class bus
{
  public:
    template <typename... Args>
    message new_method_call(Args&&...)
    {
        return {};
    }

    reply call(const message&)
    {
        return {};
    }

    void call_noreply(const message&)
    {}
};

inline bus new_default()
{
    return {};
}

} // namespace sdbusplus::bus
