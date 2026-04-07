#pragma once

namespace phosphor::logging
{}

namespace lg2
{

template <typename... Args>
inline void info(const char*, Args&&...)
{}

template <typename... Args>
inline void error(const char*, Args&&...)
{}

template <typename... Args>
inline void warning(const char*, Args&&...)
{}

template <typename... Args>
inline void debug(const char*, Args&&...)
{}

} // namespace lg2
