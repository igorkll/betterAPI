#pragma once

#include <dsbox_net/api.hpp>
#include <dsbox_net/error.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace dsbox_net
{
    class CancellationSource;

    namespace detail
    {
        struct CancelState
        {
            std::mutex mutex;
            bool cancelled = false;
            NetErrorCode reason = NetErrorCode::Cancelled;

            // Identifies one onCancel() registration, for Subscription's
            // removal on destruction. Never reused within one State's
            // lifetime -- same rationale as CommunicationManager's
            // SubscriptionId.
            std::uint64_t nextCallbackId = 1;
            std::vector<std::pair<std::uint64_t, std::function<void()>>> callbacks;
        };
    }

    // RAII handle for one CancellationToken::onCancel() registration:
    // removes the callback on destruction if it hasn't fired yet. Move-
    // only, default-constructed as empty (destroying it is then a
    // no-op) -- mirrors dsbox::ScopedSubscription's shape deliberately,
    // since it is the same "own one registration, clean up on scope
    // exit" pattern.
    class Subscription
    {
    public:
        Subscription() noexcept = default;

        Subscription(std::shared_ptr<detail::CancelState> state, std::uint64_t id) noexcept
            : m_state(std::move(state))
            , m_id(id)
        {
        }

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Subscription(Subscription&& other) noexcept
            : m_state(std::move(other.m_state))
            , m_id(std::exchange(other.m_id, 0))
        {
        }

        Subscription& operator=(Subscription&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_state = std::move(other.m_state);
                m_id = std::exchange(other.m_id, 0);
            }

            return *this;
        }

        ~Subscription()
        {
            reset();
        }

        void reset() noexcept
        {
            auto state = std::move(m_state);
            const auto id = std::exchange(m_id, 0);

            if (!state || id == 0)
                return;

            std::lock_guard<std::mutex> lock(state->mutex);

            state->callbacks.erase(
                std::remove_if(
                    state->callbacks.begin(),
                    state->callbacks.end(),
                    [id](const auto& entry) { return entry.first == id; }),
                state->callbacks.end());
        }

    private:
        std::shared_ptr<detail::CancelState> m_state;
        std::uint64_t m_id = 0;
    };

    // A read-only view onto a cancellation signal. Cheap to copy (shares
    // the underlying state); a default-constructed token is permanently
    // "never cancelled" and safe to use anywhere a real one would be --
    // that's what lets Request::cancel default to something usable
    // without every caller needing a CancellationSource just to fill in
    // the field.
    class CancellationToken
    {
    public:
        CancellationToken() = default;

        bool isCancelled() const noexcept
        {
            if (!m_state)
                return false;

            std::lock_guard<std::mutex> lock(m_state->mutex);
            return m_state->cancelled;
        }

        // Meaningful only once isCancelled() is true; Cancelled if this
        // token can never fire (default-constructed).
        NetErrorCode reason() const noexcept
        {
            if (!m_state)
                return NetErrorCode::Cancelled;

            std::lock_guard<std::mutex> lock(m_state->mutex);
            return m_state->reason;
        }

        // Registers `callback` to run the moment this token is
        // cancelled -- immediately, inline, if it already is. Returns a
        // Subscription that removes the registration on destruction if
        // it hasn't fired yet. A default-constructed (never-cancelling)
        // token accepts this call and simply never invokes `callback`;
        // the returned Subscription is empty and harmless.
        //
        // `callback` may run on whatever thread calls
        // CancellationSource::cancel() -- keep it fast and
        // exception-free; like CommunicationManager callbacks, it is
        // free to call back into this token/source.
        Subscription onCancel(std::function<void()> callback) const
        {
            if (!m_state)
                return {};

            std::unique_lock<std::mutex> lock(m_state->mutex);

            if (m_state->cancelled)
            {
                lock.unlock();
                callback();
                return {};
            }

            const auto id = m_state->nextCallbackId++;
            m_state->callbacks.emplace_back(id, std::move(callback));

            return Subscription(m_state, id);
        }

    private:
        friend class CancellationSource;

        explicit CancellationToken(std::shared_ptr<detail::CancelState> state) noexcept
            : m_state(std::move(state))
        {
        }

        std::shared_ptr<detail::CancelState> m_state;
    };

    // Owns a cancellation signal; CancellationToken is the read-only
    // handle to it. Modeled on the same split as
    // dsbox::CommunicationManager's EventHandle/registerEvent: whoever
    // can legitimately trigger cancellation (the caller of fetch(), or
    // NetworkManager itself for a timeout) holds the Source; everything
    // downstream (interceptors, the protocol handler, a BodyPipe reader)
    // only ever sees the Token.
    class DSBOX_NET_API CancellationSource
    {
    public:
        CancellationSource()
            : m_state(std::make_shared<detail::CancelState>())
        {
        }

        CancellationToken token() const noexcept
        {
            return CancellationToken(m_state);
        }

        // First call wins: if already cancelled, does nothing (in
        // particular, does not overwrite `reason`). Safe to call
        // concurrently with itself and with onCancel(); registered
        // callbacks run synchronously, on this thread, after the state
        // is updated but before cancel() returns.
        void cancel(NetErrorCode reason = NetErrorCode::Cancelled)
        {
            std::vector<std::function<void()>> toRun;

            {
                std::lock_guard<std::mutex> lock(m_state->mutex);

                if (m_state->cancelled)
                    return;

                m_state->cancelled = true;
                m_state->reason = reason;

                toRun.reserve(m_state->callbacks.size());
                for (auto& entry : m_state->callbacks)
                    toRun.push_back(std::move(entry.second));

                m_state->callbacks.clear();
            }

            for (auto& callback : toRun)
                callback();
        }

        bool isCancelled() const noexcept
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            return m_state->cancelled;
        }

    private:
        std::shared_ptr<detail::CancelState> m_state;
    };
}
