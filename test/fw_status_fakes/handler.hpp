#pragma once

#include <coroutine>

namespace mctp_vdm::requester
{

struct Coroutine
{
    struct promise_type
    {
        std::coroutine_handle<> parent_handle;
        uint8_t data = 0;
        bool detached = false;

        Coroutine get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_never initial_suspend()
        {
            return {};
        }

        auto final_suspend() noexcept
        {
            struct awaiter
            {
                bool await_ready() const noexcept
                {
                    return false;
                }

                void await_resume() const noexcept
                {}

                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept
                {
                    auto parent = h.promise().parent_handle;
                    if (h.promise().detached)
                    {
                        h.destroy();
                    }
                    return parent ? parent : std::noop_coroutine();
                }
            };
            return awaiter{};
        }

        void unhandled_exception()
        {}

        void return_value(uint8_t value) noexcept
        {
            data = value;
        }
    };

    bool await_ready() const noexcept
    {
        return handle.done();
    }

    uint8_t await_resume() const noexcept
    {
        return handle.promise().data;
    }

    bool await_suspend(std::coroutine_handle<> coroutine)
    {
        handle.promise().parent_handle = coroutine;
        return true;
    }

    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;

    Coroutine(std::coroutine_handle<promise_type> coroutine) noexcept :
        handle(coroutine)
    {}

    Coroutine(Coroutine&& other) noexcept : handle(other.handle)
    {
        other.handle = nullptr;
    }

    Coroutine& operator=(Coroutine&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        if (handle)
        {
            handle.destroy();
        }
        handle = other.handle;
        other.handle = nullptr;
        return *this;
    }

    ~Coroutine()
    {
        if (handle)
        {
            handle.destroy();
        }
    }

    void detach()
    {
        if (!handle)
        {
            return;
        }

        if (handle.done())
        {
            handle.destroy();
        }
        else
        {
            handle.promise().detached = true;
        }
        handle = nullptr;
    }

    mutable std::coroutine_handle<promise_type> handle;
};

} // namespace mctp_vdm::requester
