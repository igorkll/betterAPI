#pragma once

#include <dsbox/module.hpp>
#include <dsbox/owner.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace dsbox
{
    // Identifies one registerEvent() subscription. Returned by
    // registerEvent() and accepted by unregisterEvent(). Unique within
    // one CommunicationManager for that manager's whole lifetime and
    // never reused, so a stale id can never remove somebody else's
    // newer subscription. kInvalidSubscription (0) is never a real id.
    //
    // Deliberately a plain value, not an RAII object: every existing
    // call site that ignores registerEvent()'s result must keep meaning
    // "subscribe for the manager's lifetime". Use ScopedSubscription (or
    // registerEventScoped()) when you want automatic unsubscription.
    using SubscriptionId = std::uint64_t;
    constexpr SubscriptionId kInvalidSubscription = 0;

    // One row of CommunicationManager::subscribers()/listEvents().
    struct SubscriberInfo
    {
        SubscriptionId id = kInvalidSubscription;

        // Who registered it; Anonymous (null) if nobody. Identity only:
        // the module may be gone by now, do not dereference.
        const IModule* owner = Anonymous;

        // The owner's name, copied at registration time. Safe to use even
        // after the module was destroyed.
        std::string ownerName{ kAnonymousOwnerName };
    };

    // One row of CommunicationManager::listEvents().
    struct EventInfo
    {
        std::string name;
        std::type_index payloadType;
        std::vector<SubscriberInfo> subscribers;
    };

    // One row of CommunicationManager::listHandledEvents().
    struct HandledEventInfo
    {
        std::string name;
        std::type_index payloadType;
        std::type_index returnType;
        std::vector<SubscriberInfo> subscribers;
    };

    // One row of CommunicationManager::listHandlers(); also the payload of
    // the comm.debug.handler_* events.
    struct HandlerInfo
    {
        std::string name;
        std::type_index payloadType;
        std::type_index returnType;
        const IModule* owner = Anonymous;
        std::string ownerName{ kAnonymousOwnerName };
    };

    // Payload of the comm.debug.event_* events: one subscription of one
    // event was added or removed.
    struct SubscriptionInfo
    {
        std::string name;
        std::type_index payloadType;
        SubscriberInfo subscriber;
    };

    // Payload of the comm.debug.handled_event_* events: one subscription
    // of one handled event was added or removed.
    struct HandledSubscriptionInfo
    {
        std::string name;
        std::type_index payloadType;
        std::type_index returnType;
        SubscriberInfo subscriber;
    };

    // Payload of comm.debug.owner_unregistered.
    struct OwnerUnregisteredInfo
    {
        const IModule* owner = Anonymous;
        std::string ownerName{ kAnonymousOwnerName };
        std::size_t handlers = 0;
        std::size_t subscriptions = 0;

        // Handled-event subscriptions removed; counted separately from
        // plain-event `subscriptions` above since they live in their own
        // namespace of ids and entries. Defaulted so existing
        // (positional) construction of this struct keeps compiling.
        std::size_t handledEventSubscriptions = 0;
    };

    // Debug events the CommunicationManager emits about itself. They are
    // ordinary events: subscribe with registerEvent<Payload>(topic, ...).
    //
    //   topic                             payload
    //   comm.debug.handler_registered     HandlerInfo
    //   comm.debug.handler_unregistered   HandlerInfo
    //   comm.debug.event_subscribed       SubscriptionInfo
    //   comm.debug.event_unsubscribed     SubscriptionInfo
    //   comm.debug.handled_event_subscribed    HandledSubscriptionInfo
    //   comm.debug.handled_event_unsubscribed  HandledSubscriptionInfo
    //   comm.debug.owner_unregistered     OwnerUnregisteredInfo
    //
    // Guarantees:
    //   - Sent after the change has taken effect and after the manager's
    //     lock was released, so a listener may call back into the manager.
    //     Concurrent changes from several threads may be reported in a
    //     different order than they happened.
    //   - unregisterOwner() reports each removed handler and subscription
    //     individually (plain-event and handled-event alike), then one
    //     comm.debug.owner_unregistered summary (only if it removed
    //     anything).
    //   - Nothing is reported about topics that start with "comm.debug."
    //     themselves, so listening to these events never produces more of
    //     them.
    //   - An exception thrown by a debug listener is swallowed; it can
    //     never make a registration or unregistration fail.
    //   - A listener only hears about changes made after it subscribed;
    //     to get the current state, call listEvents()/listHandlers()/
    //     listHandledEvents() right after subscribing.
    //   - Free when nobody listens: no payload is even built.
    inline constexpr std::string_view kDebugTopicPrefix = "comm.debug.";

    inline constexpr std::string_view kDebugHandlerRegistered = "comm.debug.handler_registered";
    inline constexpr std::string_view kDebugHandlerUnregistered = "comm.debug.handler_unregistered";
    inline constexpr std::string_view kDebugEventSubscribed = "comm.debug.event_subscribed";
    inline constexpr std::string_view kDebugEventUnsubscribed = "comm.debug.event_unsubscribed";
    inline constexpr std::string_view kDebugHandledEventSubscribed = "comm.debug.handled_event_subscribed";
    inline constexpr std::string_view kDebugHandledEventUnsubscribed = "comm.debug.handled_event_unsubscribed";
    inline constexpr std::string_view kDebugOwnerUnregistered = "comm.debug.owner_unregistered";

    // One subscriber's answer to a publishHandledEvent<Signature>() call:
    // either the value it returned, or (if it threw) the exception it
    // threw, captured so the caller can rethrow/inspect it on its own
    // terms instead of the whole publish() call failing because one of
    // several subscribers misbehaved.
    //
    // RT must not be void -- a handled event exists to collect answers
    // from its subscribers; a plain registerEvent()/emit() already covers
    // the "no answer expected" case.
    template<typename RT>
    using HandledEventResult = std::variant<RT, std::exception_ptr>;

    namespace detail
    {
        // Reads the module's name. Called once per registration and only
        // with a live module (see owner.hpp).
        inline std::string ownerNameOf(const IModule* owner)
        {
            return owner ? std::string(owner->name()) : std::string(kAnonymousOwnerName);
        }

        struct EndpointKey
        {
            std::string name;
            std::type_index payload_type;
            std::type_index return_type;

            bool operator==(const EndpointKey& other) const noexcept
            {
                return name == other.name
                    && payload_type == other.payload_type
                    && return_type == other.return_type;
            }
        };

        struct EndpointKeyHash
        {
            std::size_t operator()(const EndpointKey& key) const noexcept
            {
                std::size_t hash = std::hash<std::string>{}(key.name);

                hash ^= key.payload_type.hash_code()
                    + 0x9e3779b9
                    + (hash << 6)
                    + (hash >> 2);

                hash ^= key.return_type.hash_code()
                    + 0x9e3779b9
                    + (hash << 6)
                    + (hash >> 2);

                return hash;
            }
        };

        template<typename Signature>
        struct SignatureTraits;

        template<typename RT, typename PT>
        struct SignatureTraits<RT(PT)>
        {
            using return_type = RT;
            using payload_type = PT;
        };

        struct HandlerEntry
        {
            virtual ~HandlerEntry() = default;

            // Set once at registration, never modified afterwards.
            const IModule* owner = Anonymous;
            std::string ownerName;
        };

        template<typename RT, typename PT>
        struct TypedHandlerEntry final : HandlerEntry
        {
            using Function = std::function<RT(PT)>;

            TypedHandlerEntry(Function fn, const IModule* ownerModule, std::string ownerModuleName)
                : function(std::move(fn))
            {
                owner = ownerModule;
                ownerName = std::move(ownerModuleName);
            }

            Function function;
        };

        struct EventEntry
        {
            virtual ~EventEntry() = default;

            virtual std::size_t subscriberCount() const = 0;
            virtual std::vector<SubscriberInfo> subscribers() const = 0;

            // Removes subscriber `id` and describes it in `removed`.
            // Returns the subscriber list as it was BEFORE the removal
            // (type-erased) if `id` was found, or null if it wasn't. The
            // caller keeps the returned pointer alive until it has
            // released every lock it holds: the old list owns the removed
            // callback, and destroying a callback runs arbitrary user
            // code that may re-enter the manager.
            virtual std::shared_ptr<const void> remove(SubscriptionId id, SubscriberInfo& removed) = 0;

            // Same, for every subscriber belonging to `owner`; appends
            // their descriptions to `removed`. Returns null if there was
            // none.
            virtual std::shared_ptr<const void> removeOwner(const IModule* owner, std::vector<SubscriberInfo>& removed) = 0;
        };

        // The subscriber list is immutable once published: add()/remove()
        // build a NEW list and swap it in (copy-on-write), while readers
        // (emit(), EventHandle) just copy a shared_ptr to whatever list
        // is current and iterate it without holding any lock. That is
        // what makes concurrent emit() vs. register/unregister safe, and
        // why a callback may freely (un)subscribe from inside itself.
        template<typename PT>
        class TypedEventEntry final : public EventEntry
        {
        public:
            using Function = std::function<void(PT)>;

            struct Subscriber
            {
                SubscriptionId id = kInvalidSubscription;
                const IModule* owner = Anonymous;
                std::string ownerName;
                Function function;
            };

            using SubscriberList = std::vector<Subscriber>;

            std::shared_ptr<const SubscriberList> snapshot() const
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                return m_subscribers;
            }

            // add()/remove()/removeOwner() are only ever called by
            // CommunicationManager while it holds its own exclusive lock,
            // so mutators never race each other; m_mutex only has to
            // order them against concurrent readers.
            //
            // add() returns the list as it was BEFORE the addition
            // (type-erased). The caller must keep it alive until it has
            // released every lock it holds, for the same reason as in
            // EventEntry::remove().
            std::shared_ptr<const void> add(
                SubscriptionId id,
                const IModule* owner,
                std::string ownerName,
                Function function)
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                std::shared_ptr<const SubscriberList> previous = m_subscribers;

                auto next = std::make_shared<SubscriberList>(*previous);
                next->push_back(Subscriber{ id, owner, std::move(ownerName), std::move(function) });

                m_subscribers = std::move(next);

                return previous;
            }

            std::shared_ptr<const void> remove(SubscriptionId id, SubscriberInfo& removed) override
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                const auto found = std::find_if(
                    m_subscribers->begin(),
                    m_subscribers->end(),
                    [id](const Subscriber& subscriber) { return subscriber.id == id; });

                if (found == m_subscribers->end())
                    return nullptr;

                removed = SubscriberInfo{ found->id, found->owner, found->ownerName };

                auto next = std::make_shared<SubscriberList>();
                next->reserve(m_subscribers->size() - 1);

                for (const auto& subscriber : *m_subscribers)
                {
                    if (subscriber.id != id)
                        next->push_back(subscriber);
                }

                std::shared_ptr<const void> retired = m_subscribers;
                m_subscribers = std::move(next);
                return retired;
            }

            std::shared_ptr<const void> removeOwner(const IModule* owner, std::vector<SubscriberInfo>& removed) override
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                const bool any = std::any_of(
                    m_subscribers->begin(),
                    m_subscribers->end(),
                    [owner](const Subscriber& subscriber) { return subscriber.owner == owner; });

                if (!any)
                    return nullptr;

                auto next = std::make_shared<SubscriberList>();
                next->reserve(m_subscribers->size());

                for (const auto& subscriber : *m_subscribers)
                {
                    if (subscriber.owner == owner)
                        removed.push_back(SubscriberInfo{ subscriber.id, subscriber.owner, subscriber.ownerName });
                    else
                        next->push_back(subscriber);
                }

                std::shared_ptr<const void> retired = m_subscribers;
                m_subscribers = std::move(next);
                return retired;
            }

            std::size_t subscriberCount() const override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                return m_subscribers->size();
            }

            std::vector<SubscriberInfo> subscribers() const override
            {
                const auto list = snapshot();

                std::vector<SubscriberInfo> result;
                result.reserve(list->size());

                for (const auto& subscriber : *list)
                    result.push_back(SubscriberInfo{ subscriber.id, subscriber.owner, subscriber.ownerName });

                return result;
            }

            void invoke(const PT& payload) const
            {
                const auto list = snapshot();

                for (const auto& subscriber : *list)
                    subscriber.function(payload);
            }

        private:
            mutable std::mutex m_mutex;
            std::shared_ptr<const SubscriberList> m_subscribers = std::make_shared<SubscriberList>();
        };

        // A "handled event": structurally the same copy-on-write
        // subscriber list as TypedEventEntry<PT> above (same locking,
        // same snapshot semantics, same reasoning for why mutators never
        // race readers), except each subscriber returns an RT instead of
        // returning nothing, and invoke() collects every subscriber's
        // answer instead of discarding it.
        //
        // Kept as its own type (rather than factored into a shared base
        // with TypedEventEntry) so that a plain event's invoke() -- the
        // hot, simple path -- is untouched by this addition, matching
        // this file's existing style of TypedHandlerEntry/TypedEventEntry
        // as separate, independently-reasoned-about types rather than one
        // generalized over whether a payload produces a reply.
        template<typename RT, typename PT>
        class TypedHandledEventEntry final : public EventEntry
        {
        public:
            using Function = std::function<RT(PT)>;

            struct Subscriber
            {
                SubscriptionId id = kInvalidSubscription;
                const IModule* owner = Anonymous;
                std::string ownerName;
                Function function;
            };

            using SubscriberList = std::vector<Subscriber>;

            std::shared_ptr<const SubscriberList> snapshot() const
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                return m_subscribers;
            }

            std::shared_ptr<const void> add(
                SubscriptionId id,
                const IModule* owner,
                std::string ownerName,
                Function function)
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                std::shared_ptr<const SubscriberList> previous = m_subscribers;

                auto next = std::make_shared<SubscriberList>(*previous);
                next->push_back(Subscriber{ id, owner, std::move(ownerName), std::move(function) });

                m_subscribers = std::move(next);

                return previous;
            }

            std::shared_ptr<const void> remove(SubscriptionId id, SubscriberInfo& removed) override
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                const auto found = std::find_if(
                    m_subscribers->begin(),
                    m_subscribers->end(),
                    [id](const Subscriber& subscriber) { return subscriber.id == id; });

                if (found == m_subscribers->end())
                    return nullptr;

                removed = SubscriberInfo{ found->id, found->owner, found->ownerName };

                auto next = std::make_shared<SubscriberList>();
                next->reserve(m_subscribers->size() - 1);

                for (const auto& subscriber : *m_subscribers)
                {
                    if (subscriber.id != id)
                        next->push_back(subscriber);
                }

                std::shared_ptr<const void> retired = m_subscribers;
                m_subscribers = std::move(next);
                return retired;
            }

            std::shared_ptr<const void> removeOwner(const IModule* owner, std::vector<SubscriberInfo>& removed) override
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                const bool any = std::any_of(
                    m_subscribers->begin(),
                    m_subscribers->end(),
                    [owner](const Subscriber& subscriber) { return subscriber.owner == owner; });

                if (!any)
                    return nullptr;

                auto next = std::make_shared<SubscriberList>();
                next->reserve(m_subscribers->size());

                for (const auto& subscriber : *m_subscribers)
                {
                    if (subscriber.owner == owner)
                        removed.push_back(SubscriberInfo{ subscriber.id, subscriber.owner, subscriber.ownerName });
                    else
                        next->push_back(subscriber);
                }

                std::shared_ptr<const void> retired = m_subscribers;
                m_subscribers = std::move(next);
                return retired;
            }

            std::size_t subscriberCount() const override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                return m_subscribers->size();
            }

            std::vector<SubscriberInfo> subscribers() const override
            {
                const auto list = snapshot();

                std::vector<SubscriberInfo> result;
                result.reserve(list->size());

                for (const auto& subscriber : *list)
                    result.push_back(SubscriberInfo{ subscriber.id, subscriber.owner, subscriber.ownerName });

                return result;
            }

            // Calls every current subscriber (subscription order, same
            // snapshot-at-the-start semantics as TypedEventEntry::invoke)
            // and collects one HandledEventResult<RT> per subscriber. A
            // subscriber throwing is caught here and turned into the
            // exception_ptr alternative instead of unwinding out of
            // invoke() -- one misbehaving subscriber must not stop the
            // rest from being called or lose their answers.
            std::vector<HandledEventResult<RT>> invoke(const PT& payload) const
            {
                const auto list = snapshot();

                std::vector<HandledEventResult<RT>> results;
                results.reserve(list->size());

                for (const auto& subscriber : *list)
                {
                    try
                    {
                        results.emplace_back(std::in_place_index<0>, subscriber.function(payload));
                    }
                    catch (...)
                    {
                        results.emplace_back(std::in_place_index<1>, std::current_exception());
                    }
                }

                return results;
            }

        private:
            mutable std::mutex m_mutex;
            std::shared_ptr<const SubscriberList> m_subscribers = std::make_shared<SubscriberList>();
        };
    }

    // Handle returned by CommunicationManager::getHandler(). Does not
    // keep the handler alive: it refers to the registered handler
    // weakly, so once unregisterHandler()/unregisterOwner() removes it
    // (or the CommunicationManager itself is destroyed) the handle simply
    // becomes invalid -- operator bool() turns false and calling it
    // throws -- instead of dangling. That is deliberate: a module that
    // unregisters its handlers on unload can rely on nobody still being
    // able to call into its (possibly already unmapped) code through a
    // cached handle.
    //
    // A call that is already running when the handler is unregistered
    // is not interrupted; it holds the handler alive until it returns.
    template<typename Signature>
    class HandlerHandle;

    template<typename RT, typename PT>
    class HandlerHandle<RT(PT)>
    {
    public:
        using Entry = detail::TypedHandlerEntry<RT, PT>;

        HandlerHandle() = default;

        explicit HandlerHandle(const std::shared_ptr<Entry>& entry)
            : m_entry(entry)
        {
        }

        // True while the handler is still registered.
        explicit operator bool() const noexcept
        {
            return !m_entry.expired();
        }

        RT operator()(PT payload) const
        {
            auto entry = m_entry.lock();

            if (!entry)
                throw std::runtime_error("Attempt to invoke an invalid handler");

            return entry->function(std::move(payload));
        }

        // Strong reference to the underlying entry (null if the handler
        // is gone). Lets a caller do "if it still exists, call it,
        // otherwise do something else" without a window between the
        // check and the call in which another thread could unregister
        // it -- see Overridable.
        std::shared_ptr<Entry> pin() const noexcept
        {
            return m_entry.lock();
        }

    private:
        std::weak_ptr<Entry> m_entry;
    };


    // Handle returned by CommunicationManager::getEvent(). Like
    // HandlerHandle it refers to the event weakly, so it becomes
    // invalid (never dangles) if the event goes away -- which happens
    // when its LAST subscriber is unregistered, or when the manager is
    // destroyed. While valid it always delivers to the subscribers that
    // exist at the moment of the call, including ones registered after
    // the handle was obtained.
    //
    // Invoking the handle is safe against concurrent registerEvent()/
    // unregisterEvent() from other threads (it iterates a snapshot,
    // exactly like CommunicationManager::emit()).
    template<typename PT>
    class EventHandle
    {
    public:
        using Entry = detail::TypedEventEntry<PT>;

        EventHandle() = default;

        explicit EventHandle(const std::shared_ptr<Entry>& entry)
            : m_entry(entry)
        {
        }

        explicit operator bool() const noexcept
        {
            return !m_entry.expired();
        }

        void operator()(PT payload) const
        {
            auto entry = m_entry.lock();

            if (!entry)
                throw std::runtime_error("Attempt to invoke an invalid event");

            entry->invoke(payload);
        }

    private:
        std::weak_ptr<Entry> m_entry;
    };


    // Handle returned by CommunicationManager::getHandledEvent(). Same
    // weak-reference behavior as EventHandle: it goes invalid (never
    // dangles) once the last subscriber is unregistered or the manager
    // is destroyed, and while valid it always delivers to whoever is
    // currently subscribed. operator() returns one HandledEventResult<RT>
    // per subscriber, in subscription order, exactly like
    // CommunicationManager::publishHandledEvent().
    template<typename Signature>
    class HandledEventHandle;

    template<typename RT, typename PT>
    class HandledEventHandle<RT(PT)>
    {
    public:
        using Entry = detail::TypedHandledEventEntry<RT, PT>;

        HandledEventHandle() = default;

        explicit HandledEventHandle(const std::shared_ptr<Entry>& entry)
            : m_entry(entry)
        {
        }

        explicit operator bool() const noexcept
        {
            return !m_entry.expired();
        }

        std::vector<HandledEventResult<RT>> operator()(PT payload) const
        {
            auto entry = m_entry.lock();

            if (!entry)
                throw std::runtime_error("Attempt to invoke an invalid handled event");

            return entry->invoke(payload);
        }

    private:
        std::weak_ptr<Entry> m_entry;
    };


    class CommunicationManager;

    // RAII owner of one event or handled-event subscription: unsubscribes
    // on destruction. Move-only. Typical use is a module keeping these as
    // members (or in a std::vector) so onShutdown()/destruction removes
    // every callback that captured `this`.
    //
    // The CommunicationManager it was created for must outlive it.
    class ScopedSubscription
    {
    public:
        // Which registration table `reset()` must unsubscribe from.
        // Plain events and handled events keep separate id -> entry maps
        // (see CommunicationManager), so the id alone isn't enough to
        // know which one to call.
        enum class Kind
        {
            Event,
            HandledEvent,
        };

        ScopedSubscription() noexcept = default;

        ScopedSubscription(CommunicationManager& comm, SubscriptionId id, Kind kind = Kind::Event) noexcept
            : m_comm(&comm)
            , m_id(id)
            , m_kind(kind)
        {
        }

        ScopedSubscription(const ScopedSubscription&) = delete;
        ScopedSubscription& operator=(const ScopedSubscription&) = delete;

        ScopedSubscription(ScopedSubscription&& other) noexcept
            : m_comm(std::exchange(other.m_comm, nullptr))
            , m_id(std::exchange(other.m_id, kInvalidSubscription))
            , m_kind(other.m_kind)
        {
        }

        ScopedSubscription& operator=(ScopedSubscription&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_comm = std::exchange(other.m_comm, nullptr);
                m_id = std::exchange(other.m_id, kInvalidSubscription);
                m_kind = other.m_kind;
            }

            return *this;
        }

        ~ScopedSubscription()
        {
            reset();
        }

        SubscriptionId id() const noexcept
        {
            return m_id;
        }

        // True while this object still owns a subscription.
        explicit operator bool() const noexcept
        {
            return m_id != kInvalidSubscription;
        }

        // Unsubscribes now (no-op if empty). Defined below, once
        // CommunicationManager is complete.
        void reset() noexcept;

        // Gives up ownership WITHOUT unsubscribing; the subscription
        // then lives until unregisterEvent()/unregisterHandledEvent() or
        // the manager's end.
        SubscriptionId release() noexcept
        {
            m_comm = nullptr;
            return std::exchange(m_id, kInvalidSubscription);
        }

    private:
        CommunicationManager* m_comm = nullptr;
        SubscriptionId m_id = kInvalidSubscription;
        Kind m_kind = Kind::Event;
    };


    // Typed, name-keyed message bus with three kinds of endpoint:
    //   - "handlers"      request/response, at most one per name+signature.
    //   - "events"        pub/sub, any number of subscribers per
    //                      name+payload type, nothing comes back to the
    //                      publisher.
    //   - "handled events" pub/sub like events (any number of subscribers
    //                      per name+payload+return type), except every
    //                      subscriber returns an RT and publishHandledEvent()
    //                      hands the publisher back one HandledEventResult<RT>
    //                      per subscriber -- the value it returned, or the
    //                      exception it threw. The missing middle ground
    //                      between a handler (exactly one answer, a second
    //                      registration is a hard error) and a plain event
    //                      (any number of listeners, no answer at all):
    //                      several independent subscribers may all want to
    //                      react to the same call and hand something back,
    //                      without the publisher being brought down by
    //                      (or even order-dependent on) any one of them
    //                      throwing.
    // All three can be registered and unregistered at any time, and all
    // three can be inspected.
    //
    // Registration
    //   registerHandler       - one handler per (name, signature); a second
    //                           registration throws. unregisterHandler()
    //                           frees the slot again. Any code may
    //                           unregister any handler; there is
    //                           deliberately no ownership check.
    //   registerEvent         - returns a SubscriptionId; unregisterEvent(id)
    //                           removes exactly that subscription.
    //   registerHandledEvent  - same shape as registerEvent, but keyed like
    //                           a handler (name+payload+return type) and
    //                           removed with unregisterHandledEvent(id).
    //   All three take an optional trailing `owner` (a module: pass `this`
    //   from inside it; omitted = Anonymous). It is recorded for
    //   diagnostics and is what unregisterOwner() removes by.
    //
    // Cleanup
    //   unregisterOwner(module) removes every handler, event subscription
    //   and handled-event subscription that module registered -- the
    //   one-liner for a module's shutdown/unload.
    //
    // Introspection (all thread-safe, all return snapshots)
    //   hasHandler / subscriberCount / hasSubscribers / subscribers,
    //   the type-erased listEvents() / listHandlers() / listHandledEvents(),
    //   and the comm.debug.* events (see kDebugTopicPrefix) that report
    //   changes as they happen.
    //
    // Thread safety: every public method is safe to call concurrently
    // from multiple threads. The internal maps are protected by a
    // shared_mutex (lookups run concurrently; register/unregister take
    // an exclusive lock). The lock is never held while invoking a
    // handler or event callback, nor while destroying a removed one, so
    // a callback is free to call back into this same CommunicationManager
    // (register, unregister -- including itself -- emit, ...) without
    // deadlocking.
    //
    // Unregistering is not a barrier for callbacks already running:
    // after unregisterEvent()/unregisterHandler()/unregisterOwner()
    // returns, no NEW emit()/call will pick the removed callback up, but
    // a call that had already started on another thread may still be
    // executing it. Whoever is about to destroy state the callback uses
    // must synchronize that themselves (e.g. capture a shared_ptr, or
    // guard with a weak_ptr).
    //
    // Snapshot semantics of emit(): the set of subscribers is fixed at
    // the moment emit() begins. A subscriber removed by an earlier
    // callback of the SAME emit() still receives that one event; a
    // subscriber added during an emit() first sees the next one.
    class CommunicationManager
    {
    public:
        CommunicationManager() = default;

        CommunicationManager(const CommunicationManager&) = delete;
        CommunicationManager& operator=(const CommunicationManager&) = delete;

        CommunicationManager(CommunicationManager&&) = delete;
        CommunicationManager& operator=(CommunicationManager&&) = delete;

        /*
         * Handler
         */

        template<typename Signature, typename Callable>
        void registerHandler(std::string_view name, Callable&& callable, const IModule* owner = Anonymous)
        {
            using Traits = detail::SignatureTraits<Signature>;

            using RT = typename Traits::return_type;
            using PT = typename Traits::payload_type;

            using Entry = detail::TypedHandlerEntry<RT, PT>;

            auto key = makeKey<PT, RT>(name);

            auto entry = std::make_shared<Entry>(
                typename Entry::Function(std::forward<Callable>(callable)),
                owner,
                detail::ownerNameOf(owner)
            );

            {
                std::unique_lock<std::shared_mutex> lock(m_mutex);

                if (m_handlers.contains(key))
                    throw std::runtime_error(
                        "Handler with the same name and signature is already registered"
                    );

                m_handlers.emplace(
                    std::move(key),
                    entry
                );
            }

            notifyDebug<HandlerInfo>(kDebugHandlerRegistered, name, [&]
            {
                return HandlerInfo{
                    std::string(name),
                    std::type_index(typeid(PT)),
                    std::type_index(typeid(RT)),
                    entry->owner,
                    entry->ownerName
                };
            });
        }

        // Removes the handler registered for `name` with exactly this
        // signature, whoever registered it. Returns true if there was
        // one. Handles obtained earlier via getHandler() become invalid
        // (see HandlerHandle); the same name+signature can be registered
        // again afterwards.
        template<typename Signature>
        bool unregisterHandler(std::string_view name)
        {
            using Traits = detail::SignatureTraits<Signature>;

            using RT = typename Traits::return_type;
            using PT = typename Traits::payload_type;

            const auto key = makeKey<PT, RT>(name);

            // Declared before the lock: the removed handler (and
            // whatever its callable captured) is destroyed after the
            // lock is released.
            std::shared_ptr<detail::HandlerEntry> retired;

            {
                std::unique_lock<std::shared_mutex> lock(m_mutex);

                const auto it = m_handlers.find(key);

                if (it == m_handlers.end())
                    return false;

                retired = std::move(it->second);
                m_handlers.erase(it);
            }

            notifyDebug<HandlerInfo>(kDebugHandlerUnregistered, name, [&]
            {
                return HandlerInfo{
                    std::string(name),
                    std::type_index(typeid(PT)),
                    std::type_index(typeid(RT)),
                    retired->owner,
                    retired->ownerName
                };
            });

            return true;
        }

        template<typename Signature>
        HandlerHandle<Signature> getHandler(std::string_view name)
        {
            using Traits = detail::SignatureTraits<Signature>;

            using RT = typename Traits::return_type;
            using PT = typename Traits::payload_type;

            const auto key = makeKey<PT, RT>(name);

            std::shared_lock<std::shared_mutex> lock(m_mutex);

            const auto it = m_handlers.find(key);

            if (it == m_handlers.end())
                return {};

            return HandlerHandle<Signature>(
                std::static_pointer_cast<detail::TypedHandlerEntry<RT, PT>>(it->second)
            );
        }

        template<typename Signature>
        bool hasHandler(std::string_view name) const
        {
            using Traits = detail::SignatureTraits<Signature>;

            using RT = typename Traits::return_type;
            using PT = typename Traits::payload_type;

            const auto key = makeKey<PT, RT>(name);

            std::shared_lock<std::shared_mutex> lock(m_mutex);
            return m_handlers.contains(key);
        }

        // Every registered handler, regardless of signature, sorted by
        // name (then by payload type name) so the output is stable.
        std::vector<HandlerInfo> listHandlers() const
        {
            std::vector<HandlerInfo> result;

            {
                std::shared_lock<std::shared_mutex> lock(m_mutex);

                result.reserve(m_handlers.size());

                for (const auto& [key, entry] : m_handlers)
                {
                    result.push_back(HandlerInfo{
                        key.name,
                        key.payload_type,
                        key.return_type,
                        entry->owner,
                        entry->ownerName
                    });
                }
            }

            std::sort(
                result.begin(),
                result.end(),
                [](const HandlerInfo& a, const HandlerInfo& b)
                {
                    if (a.name != b.name)
                        return a.name < b.name;

                    return std::string_view(a.payloadType.name()) < std::string_view(b.payloadType.name());
                });

            return result;
        }


        /*
         * Event
         */

        // Subscribes `callable` to events named `name` carrying payload
        // type PT and returns the id of the new subscription. `owner` is
        // the module the subscription belongs to (pass `this` from inside
        // a module); omitted means Anonymous. Ignoring the result is fine
        // and means "subscribe for the manager's lifetime", exactly as
        // before.
        template<typename PT, typename Callable>
        SubscriptionId registerEvent(std::string_view name, Callable&& callable, const IModule* owner = Anonymous)
        {
            using Entry = detail::TypedEventEntry<PT>;

            auto key = makeKey<PT, void>(name);

            typename Entry::Function function(std::forward<Callable>(callable));
            std::string ownerName = detail::ownerNameOf(owner);

            // Declared before the lock so it is released after it: it owns
            // the previous subscriber list, and destroying that runs user
            // code (captured objects' destructors).
            std::shared_ptr<const void> retired;

            SubscriptionId id = kInvalidSubscription;

            {
                std::unique_lock<std::shared_mutex> lock(m_mutex);

                id = m_nextSubscriptionId++;

                const auto it = m_events.find(key);

                if (it == m_events.end())
                {
                    auto entry = std::make_shared<Entry>();
                    retired = entry->add(id, owner, ownerName, std::move(function));

                    m_events.emplace(key, std::move(entry));
                }
                else
                {
                    retired = static_cast<Entry*>(it->second.get())->add(id, owner, ownerName, std::move(function));
                }

                m_subscriptionKeys.emplace(id, std::move(key));
            }

            notifyDebug<SubscriptionInfo>(kDebugEventSubscribed, name, [&]
            {
                return SubscriptionInfo{
                    std::string(name),
                    std::type_index(typeid(PT)),
                    SubscriberInfo{ id, owner, ownerName }
                };
            });

            return id;
        }

        // Same as registerEvent(), but the returned object unsubscribes
        // automatically when it is destroyed or reset().
        template<typename PT, typename Callable>
        ScopedSubscription registerEventScoped(std::string_view name, Callable&& callable, const IModule* owner = Anonymous)
        {
            return ScopedSubscription(
                *this,
                registerEvent<PT>(name, std::forward<Callable>(callable), owner)
            );
        }

        // Removes exactly the subscription `id` came from. Returns true
        // if it was still subscribed, false for an unknown/already
        // removed/invalid id (so calling it twice is harmless). When the
        // last subscriber of an event is removed the event itself is
        // dropped: getEvent() for it returns an invalid handle and
        // listEvents() no longer mentions it.
        bool unregisterEvent(SubscriptionId id)
        {
            if (id == kInvalidSubscription)
                return false;

            // Declared before the lock so they are released after it:
            // the removed callback's destructor is user code and may
            // call back into this manager.
            std::shared_ptr<const void> retiredSubscribers;
            std::shared_ptr<detail::EventEntry> retiredEntry;

            std::optional<detail::EndpointKey> key;
            SubscriberInfo removed;

            {
                std::unique_lock<std::shared_mutex> lock(m_mutex);

                const auto keyIt = m_subscriptionKeys.find(id);

                if (keyIt == m_subscriptionKeys.end())
                    return false;

                key = keyIt->second;

                const auto eventIt = m_events.find(keyIt->second);

                m_subscriptionKeys.erase(keyIt);

                if (eventIt == m_events.end())
                    return false;

                retiredSubscribers = eventIt->second->remove(id, removed);

                if (eventIt->second->subscriberCount() == 0)
                {
                    retiredEntry = std::move(eventIt->second);
                    m_events.erase(eventIt);
                }
            }

            if (!retiredSubscribers)
                return false;

            notifyDebug<SubscriptionInfo>(kDebugEventUnsubscribed, key->name, [&]
            {
                return SubscriptionInfo{ key->name, key->payload_type, removed };
            });

            return true;
        }

        // Removes every handler, every event subscription and every
        // handled-event subscription that was registered with this
        // `owner` (a module, or Anonymous for the ones registered without
        // one), and returns how many it removed in total. Events and
        // handled events left without subscribers are dropped, exactly
        // as with unregisterEvent()/unregisterHandledEvent(). Idempotent:
        // a second call returns 0.
        //
        // This is the intended one-liner for a module's shutdown/unload.
        // The pointer is only compared, never dereferenced, so it is fine
        // to call this with a module that is already being destroyed.
        std::size_t unregisterOwner(const IModule* owner)
        {
            // Declared before the lock so everything removed is destroyed
            // after it (see unregisterEvent()).
            std::vector<std::shared_ptr<detail::HandlerEntry>> retiredHandlers;
            std::vector<std::shared_ptr<const void>> retiredLists;
            std::vector<std::shared_ptr<detail::EventEntry>> retiredEntries;

            std::vector<HandlerInfo> removedHandlers;
            std::vector<SubscriptionInfo> removedSubscriptions;
            std::vector<HandledSubscriptionInfo> removedHandledSubscriptions;

            {
                std::unique_lock<std::shared_mutex> lock(m_mutex);

                for (auto it = m_handlers.begin(); it != m_handlers.end();)
                {
                    if (it->second->owner != owner)
                    {
                        ++it;
                        continue;
                    }

                    removedHandlers.push_back(HandlerInfo{
                        it->first.name,
                        it->first.payload_type,
                        it->first.return_type,
                        it->second->owner,
                        it->second->ownerName
                    });

                    retiredHandlers.push_back(std::move(it->second));
                    it = m_handlers.erase(it);
                }

                for (auto it = m_events.begin(); it != m_events.end();)
                {
                    std::vector<SubscriberInfo> removed;
                    auto retiredList = it->second->removeOwner(owner, removed);

                    if (!retiredList)
                    {
                        ++it;
                        continue;
                    }

                    retiredLists.push_back(std::move(retiredList));

                    for (const auto& info : removed)
                    {
                        m_subscriptionKeys.erase(info.id);

                        removedSubscriptions.push_back(SubscriptionInfo{
                            it->first.name,
                            it->first.payload_type,
                            info
                        });
                    }

                    if (it->second->subscriberCount() == 0)
                    {
                        retiredEntries.push_back(std::move(it->second));
                        it = m_events.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }

                for (auto it = m_handledEvents.begin(); it != m_handledEvents.end();)
                {
                    std::vector<SubscriberInfo> removed;
                    auto retiredList = it->second->removeOwner(owner, removed);

                    if (!retiredList)
                    {
                        ++it;
                        continue;
                    }

                    retiredLists.push_back(std::move(retiredList));

                    for (const auto& info : removed)
                    {
                        m_handledEventSubscriptionKeys.erase(info.id);

                        removedHandledSubscriptions.push_back(HandledSubscriptionInfo{
                            it->first.name,
                            it->first.payload_type,
                            it->first.return_type,
                            info
                        });
                    }

                    if (it->second->subscriberCount() == 0)
                    {
                        retiredEntries.push_back(std::move(it->second));
                        it = m_handledEvents.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            for (const auto& info : removedHandlers)
                notifyDebug<HandlerInfo>(kDebugHandlerUnregistered, info.name, [&] { return info; });

            for (const auto& info : removedSubscriptions)
                notifyDebug<SubscriptionInfo>(kDebugEventUnsubscribed, info.name, [&] { return info; });

            for (const auto& info : removedHandledSubscriptions)
                notifyDebug<HandledSubscriptionInfo>(kDebugHandledEventUnsubscribed, info.name, [&] { return info; });

            const std::size_t total = removedHandlers.size() + removedSubscriptions.size() + removedHandledSubscriptions.size();

            if (total != 0)
            {
                // Take the name from what was removed rather than from
                // `owner`: that module may already be gone.
                const std::string& ownerName = !removedHandlers.empty()
                    ? removedHandlers.front().ownerName
                    : !removedSubscriptions.empty()
                        ? removedSubscriptions.front().subscriber.ownerName
                        : removedHandledSubscriptions.front().subscriber.ownerName;

                notifyDebug<OwnerUnregisteredInfo>(kDebugOwnerUnregistered, {}, [&]
                {
                    return OwnerUnregisteredInfo{
                        owner,
                        ownerName,
                        removedHandlers.size(),
                        removedSubscriptions.size(),
                        removedHandledSubscriptions.size()
                    };
                });
            }

            return total;
        }

        // Invalid handle if nothing is currently subscribed to this
        // name+payload type.
        template<typename PT>
        EventHandle<PT> getEvent(std::string_view name)
        {
            auto entry = findEvent<PT>(name);

            if (!entry)
                return {};

            return EventHandle<PT>(entry);
        }

        // Delivers `payload` to every subscriber registered for `name`
        // at the moment of the call (see "Snapshot semantics" above).
        // Safe to call concurrently with registerEvent()/
        // unregisterEvent(); a subscriber may call back into this
        // CommunicationManager from within itself.
        template<typename PT>
        void emit(std::string_view name, PT payload)
        {
            auto entry = findEvent<PT>(name);

            if (!entry)
                return;

            entry->invoke(payload);
        }

        // Number of live subscribers for this name+payload type (0 if
        // there are none).
        template<typename PT>
        std::size_t subscriberCount(std::string_view name) const
        {
            auto entry = findEvent<PT>(name);
            return entry ? entry->subscriberCount() : 0;
        }

        template<typename PT>
        bool hasSubscribers(std::string_view name) const
        {
            return subscriberCount<PT>(name) != 0;
        }

        // The live subscribers for this name+payload type, in
        // subscription order (oldest first). Empty if there are none.
        template<typename PT>
        std::vector<SubscriberInfo> subscribers(std::string_view name) const
        {
            auto entry = findEvent<PT>(name);

            if (!entry)
                return {};

            return entry->subscribers();
        }

        // Every event that currently has at least one subscriber,
        // regardless of payload type, with its subscribers. Sorted by
        // name (then by payload type name) so the output is stable.
        std::vector<EventInfo> listEvents() const
        {
            std::vector<std::pair<detail::EndpointKey, std::shared_ptr<detail::EventEntry>>> entries;

            {
                std::shared_lock<std::shared_mutex> lock(m_mutex);

                entries.reserve(m_events.size());

                for (const auto& [key, entry] : m_events)
                    entries.emplace_back(key, entry);
            }

            std::vector<EventInfo> result;
            result.reserve(entries.size());

            for (const auto& [key, entry] : entries)
                result.push_back(EventInfo{ key.name, key.payload_type, entry->subscribers() });

            std::sort(
                result.begin(),
                result.end(),
                [](const EventInfo& a, const EventInfo& b)
                {
                    if (a.name != b.name)
                        return a.name < b.name;

                    return std::string_view(a.payloadType.name()) < std::string_view(b.payloadType.name());
                });

            return result;
        }


        /*
         * HandledEvent
         */

        // Subscribes `callable` (an RT(PT)) to the handled event `name`
        // and returns the id of the new subscription; several subscribers
        // may share the same (name, PT, RT). `owner` is recorded exactly
        // as for registerEvent() / unregisterOwner() removes by it.
        //
        // RT must not be void -- see HandledEventResult.
        template<typename Signature, typename Callable>
        SubscriptionId registerHandledEvent(std::string_view name, Callable&& callable, const IModule* owner = Anonymous)
        {
            using Traits = detail::SignatureTraits<Signature>;

            using RT = typename Traits::return_type;
            using PT = typename Traits::payload_type;

            static_assert(!std::is_void_v<RT>,
                "registerHandledEvent's RT must not be void -- a plain registerEvent() already covers that");

            using Entry = detail::TypedHandledEventEntry<RT, PT>;

            auto key = makeKey<PT, RT>(name);

            typename Entry::Function function(std::forward<Callable>(callable));
            std::string ownerName = detail::ownerNameOf(owner);

            // Declared before the lock so it is released after it, same
            // reasoning as registerEvent().
            std::shared_ptr<const void> retired;

            SubscriptionId id = kInvalidSubscription;

            {
                std::unique_lock<std::shared_mutex> lock(m_mutex);

                id = m_nextSubscriptionId++;

                const auto it = m_handledEvents.find(key);

                if (it == m_handledEvents.end())
                {
                    auto entry = std::make_shared<Entry>();
                    retired = entry->add(id, owner, ownerName, std::move(function));

                    m_handledEvents.emplace(key, std::move(entry));
                }
                else
                {
                    retired = static_cast<Entry*>(it->second.get())->add(id, owner, ownerName, std::move(function));
                }

                m_handledEventSubscriptionKeys.emplace(id, std::move(key));
            }

            notifyDebug<HandledSubscriptionInfo>(kDebugHandledEventSubscribed, name, [&]
            {
                return HandledSubscriptionInfo{
                    std::string(name),
                    std::type_index(typeid(PT)),
                    std::type_index(typeid(RT)),
                    SubscriberInfo{ id, owner, ownerName }
                };
            });

            return id;
        }

        // Same as registerHandledEvent(), but the returned object
        // unsubscribes automatically when it is destroyed or reset().
        template<typename Signature, typename Callable>
        ScopedSubscription registerHandledEventScoped(std::string_view name, Callable&& callable, const IModule* owner = Anonymous)
        {
            return ScopedSubscription(
                *this,
                registerHandledEvent<Signature>(name, std::forward<Callable>(callable), owner),
                ScopedSubscription::Kind::HandledEvent
            );
        }

        // Removes exactly the subscription `id` came from (an id returned
        // by registerHandledEvent()). Returns true if it was still
        // subscribed. When the last subscriber of a handled event is
        // removed, the handled event itself is dropped, exactly as
        // unregisterEvent() drops an emptied-out event.
        bool unregisterHandledEvent(SubscriptionId id)
        {
            if (id == kInvalidSubscription)
                return false;

            // Declared before the lock, same reasoning as unregisterEvent().
            std::shared_ptr<const void> retiredSubscribers;
            std::shared_ptr<detail::EventEntry> retiredEntry;

            std::optional<detail::EndpointKey> key;
            SubscriberInfo removed;

            {
                std::unique_lock<std::shared_mutex> lock(m_mutex);

                const auto keyIt = m_handledEventSubscriptionKeys.find(id);

                if (keyIt == m_handledEventSubscriptionKeys.end())
                    return false;

                key = keyIt->second;

                const auto entryIt = m_handledEvents.find(keyIt->second);

                m_handledEventSubscriptionKeys.erase(keyIt);

                if (entryIt == m_handledEvents.end())
                    return false;

                retiredSubscribers = entryIt->second->remove(id, removed);

                if (entryIt->second->subscriberCount() == 0)
                {
                    retiredEntry = std::move(entryIt->second);
                    m_handledEvents.erase(entryIt);
                }
            }

            if (!retiredSubscribers)
                return false;

            notifyDebug<HandledSubscriptionInfo>(kDebugHandledEventUnsubscribed, key->name, [&]
            {
                return HandledSubscriptionInfo{ key->name, key->payload_type, key->return_type, removed };
            });

            return true;
        }

        // Invalid handle if nothing is currently subscribed to this
        // name+signature.
        template<typename Signature>
        HandledEventHandle<Signature> getHandledEvent(std::string_view name)
        {
            auto entry = findHandledEvent<Signature>(name);

            if (!entry)
                return {};

            return HandledEventHandle<Signature>(entry);
        }

        // Calls every subscriber registered for `name`+Signature at the
        // moment of the call (snapshot semantics identical to emit()) and
        // collects their answers. Returns one HandledEventResult<RT> per
        // subscriber, in subscription order -- the value it returned, or
        // the exception it threw, caught here so a throwing subscriber
        // can never stop the rest from running or being heard from. An
        // empty vector means nobody is subscribed; that is not an error.
        template<typename Signature>
        std::vector<HandledEventResult<typename detail::SignatureTraits<Signature>::return_type>>
        publishHandledEvent(std::string_view name, typename detail::SignatureTraits<Signature>::payload_type payload)
        {
            auto entry = findHandledEvent<Signature>(name);

            if (!entry)
                return {};

            return entry->invoke(payload);
        }

        // Number of live subscribers for this name+signature (0 if there
        // are none).
        template<typename Signature>
        std::size_t handledEventSubscriberCount(std::string_view name) const
        {
            auto entry = findHandledEvent<Signature>(name);
            return entry ? entry->subscriberCount() : 0;
        }

        template<typename Signature>
        bool hasHandledEventSubscribers(std::string_view name) const
        {
            return handledEventSubscriberCount<Signature>(name) != 0;
        }

        // The live subscribers for this name+signature, in subscription
        // order (oldest first). Empty if there are none.
        template<typename Signature>
        std::vector<SubscriberInfo> handledEventSubscribers(std::string_view name) const
        {
            auto entry = findHandledEvent<Signature>(name);

            if (!entry)
                return {};

            return entry->subscribers();
        }

        // Every handled event that currently has at least one subscriber,
        // with its subscribers. Sorted by name (then by payload type name,
        // then by return type name) so the output is stable.
        std::vector<HandledEventInfo> listHandledEvents() const
        {
            std::vector<std::pair<detail::EndpointKey, std::shared_ptr<detail::EventEntry>>> entries;

            {
                std::shared_lock<std::shared_mutex> lock(m_mutex);

                entries.reserve(m_handledEvents.size());

                for (const auto& [key, entry] : m_handledEvents)
                    entries.emplace_back(key, entry);
            }

            std::vector<HandledEventInfo> result;
            result.reserve(entries.size());

            for (const auto& [key, entry] : entries)
                result.push_back(HandledEventInfo{ key.name, key.payload_type, key.return_type, entry->subscribers() });

            std::sort(
                result.begin(),
                result.end(),
                [](const HandledEventInfo& a, const HandledEventInfo& b)
                {
                    if (a.name != b.name)
                        return a.name < b.name;

                    if (std::string_view(a.payloadType.name()) != std::string_view(b.payloadType.name()))
                        return std::string_view(a.payloadType.name()) < std::string_view(b.payloadType.name());

                    return std::string_view(a.returnType.name()) < std::string_view(b.returnType.name());
                });

            return result;
        }

    private:
        template<typename PT, typename RT>
        static detail::EndpointKey makeKey(std::string_view name)
        {
            return detail::EndpointKey{
                std::string(name),
                std::type_index(typeid(PT)),
                std::type_index(typeid(RT))
            };
        }

        template<typename PT>
        std::shared_ptr<detail::TypedEventEntry<PT>> findEvent(std::string_view name) const
        {
            const auto key = makeKey<PT, void>(name);

            std::shared_lock<std::shared_mutex> lock(m_mutex);

            const auto it = m_events.find(key);

            if (it == m_events.end())
                return nullptr;

            return std::static_pointer_cast<detail::TypedEventEntry<PT>>(it->second);
        }

        template<typename Signature>
        std::shared_ptr<detail::TypedHandledEventEntry<
            typename detail::SignatureTraits<Signature>::return_type,
            typename detail::SignatureTraits<Signature>::payload_type>>
        findHandledEvent(std::string_view name) const
        {
            using Traits = detail::SignatureTraits<Signature>;

            using RT = typename Traits::return_type;
            using PT = typename Traits::payload_type;

            const auto key = makeKey<PT, RT>(name);

            std::shared_lock<std::shared_mutex> lock(m_mutex);

            const auto it = m_handledEvents.find(key);

            if (it == m_handledEvents.end())
                return nullptr;

            return std::static_pointer_cast<detail::TypedHandledEventEntry<RT, PT>>(it->second);
        }

        // Emits one comm.debug.* event. `subject` is the name of the
        // handler/event the change was about; changes to comm.debug.*
        // topics themselves are never reported. `make` builds the payload
        // and only runs if somebody is listening. Must be called with no
        // lock held; swallows anything a listener throws.
        template<typename PT, typename Make>
        void notifyDebug(std::string_view topic, std::string_view subject, Make&& make) noexcept
        {
            try
            {
                if (subject.starts_with(kDebugTopicPrefix))
                    return;

                auto entry = findEvent<PT>(topic);

                if (!entry)
                    return;

                entry->invoke(make());
            }
            catch (...)
            {
            }
        }

        mutable std::shared_mutex m_mutex;

        // Next id handed out by registerEvent(); guarded by m_mutex.
        // Starts at 1 so kInvalidSubscription (0) is never issued.
        SubscriptionId m_nextSubscriptionId = 1;

        std::unordered_map<
            detail::EndpointKey,
            std::shared_ptr<detail::HandlerEntry>,
            detail::EndpointKeyHash
        > m_handlers;

        std::unordered_map<
            detail::EndpointKey,
            std::shared_ptr<detail::EventEntry>,
            detail::EndpointKeyHash
        > m_events;

        // Which event each live subscription belongs to, so
        // unregisterEvent() needs nothing but the id.
        std::unordered_map<SubscriptionId, detail::EndpointKey> m_subscriptionKeys;

        std::unordered_map<
            detail::EndpointKey,
            std::shared_ptr<detail::EventEntry>,
            detail::EndpointKeyHash
        > m_handledEvents;

        // Which handled event each live subscription belongs to, so
        // unregisterHandledEvent() needs nothing but the id. Kept separate
        // from m_subscriptionKeys (plain events) even though both are
        // SubscriptionId -> EndpointKey, because the id alone doesn't say
        // which of m_events / m_handledEvents to look in -- that is also
        // why ScopedSubscription carries a Kind.
        std::unordered_map<SubscriptionId, detail::EndpointKey> m_handledEventSubscriptionKeys;
    };

    inline void ScopedSubscription::reset() noexcept
    {
        // Cleared first, so re-entrancy (a callback destructor calling
        // reset() again) can never unsubscribe twice.
        CommunicationManager* comm = std::exchange(m_comm, nullptr);
        const SubscriptionId id = std::exchange(m_id, kInvalidSubscription);
        const Kind kind = m_kind;

        if (!comm || id == kInvalidSubscription)
            return;

        if (kind == Kind::HandledEvent)
            comm->unregisterHandledEvent(id);
        else
            comm->unregisterEvent(id);
    }
}
