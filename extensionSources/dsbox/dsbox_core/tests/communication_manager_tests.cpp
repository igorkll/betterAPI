#include "test_framework.hpp"

#include <dsbox/communication_manager.hpp>
#include <dsbox/module.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    struct Payload
    {
        int value;
    };

    struct TypeA
    {
        int value;
    };

    struct TypeB
    {
        int value;
    };

    // A module that does nothing: only its identity and name matter, as
    // the `owner` of registrations.
    class TestModule final : public dsbox::IModule
    {
    public:
        explicit TestModule(std::string name)
            : m_name(std::move(name))
        {
        }

        std::string_view name() const override
        {
            return m_name;
        }

    private:
        std::string m_name;
    };

    void testHandlerCanBeRegisteredAndCalled()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<int(Payload)>(
            "add",
            [](Payload payload) { return payload.value + 10; });

        auto handler = comm.getHandler<int(Payload)>("add");

        DSBOX_CHECK(handler);
        DSBOX_CHECK_EQ(handler(Payload{ 5 }), 15);
    }

    void testCommunicationManagersAreIndependent()
    {
        dsbox::CommunicationManager commA;
        dsbox::CommunicationManager commB;

        commA.registerHandler<int(Payload)>(
            "foo",
            [](Payload payload) { return payload.value + 1; });

        auto handlerA = commA.getHandler<int(Payload)>("foo");
        auto handlerB = commB.getHandler<int(Payload)>("foo");

        DSBOX_CHECK(handlerA);
        DSBOX_CHECK(!handlerB);

        DSBOX_CHECK_EQ(handlerA(Payload{ 10 }), 11);
    }

    void testCachedHandler()
    {
        dsbox::CommunicationManager comm;

        int calls = 0;

        comm.registerHandler<int(Payload)>(
            "foo",
            [&](Payload payload)
            {
                ++calls;
                return payload.value * 2;
            });

        auto handler = comm.getHandler<int(Payload)>("foo");

        DSBOX_CHECK(handler);

        DSBOX_CHECK_EQ(handler(Payload{ 1 }), 2);
        DSBOX_CHECK_EQ(handler(Payload{ 2 }), 4);
        DSBOX_CHECK_EQ(handler(Payload{ 3 }), 6);

        DSBOX_CHECK_EQ(calls, 3);
    }

    void testSameNameDifferentReturnTypes()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<TypeA(Payload)>(
            "someFoo",
            [](Payload payload) { return TypeA{ payload.value + 1 }; });

        comm.registerHandler<TypeB(Payload)>(
            "someFoo",
            [](Payload payload) { return TypeB{ payload.value + 2 }; });

        auto handlerA = comm.getHandler<TypeA(Payload)>("someFoo");
        auto handlerB = comm.getHandler<TypeB(Payload)>("someFoo");

        DSBOX_CHECK(handlerA);
        DSBOX_CHECK(handlerB);

        DSBOX_CHECK_EQ(handlerA(Payload{ 10 }).value, 11);
        DSBOX_CHECK_EQ(handlerB(Payload{ 10 }).value, 12);
    }

    void testMissingHandler()
    {
        dsbox::CommunicationManager comm;

        auto handler = comm.getHandler<int(Payload)>("doesNotExist");

        DSBOX_CHECK(!handler);
    }

    void testDuplicateHandler()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<int(Payload)>("foo", [](Payload) { return 1; });

        bool thrown = false;

        try
        {
            comm.registerHandler<int(Payload)>("foo", [](Payload) { return 2; });
        }
        catch (const std::runtime_error&)
        {
            thrown = true;
        }

        DSBOX_CHECK(thrown);
    }

    void testEventMultipleHandlers()
    {
        dsbox::CommunicationManager comm;

        int first = 0;
        int second = 0;

        comm.registerEvent<Payload>(
            "testEvent",
            [&](Payload payload) { first = payload.value; });

        comm.registerEvent<Payload>(
            "testEvent",
            [&](Payload payload) { second = payload.value * 2; });

        auto event = comm.getEvent<Payload>("testEvent");

        DSBOX_CHECK(event);

        event(Payload{ 21 });

        DSBOX_CHECK_EQ(first, 21);
        DSBOX_CHECK_EQ(second, 42);
    }

    void testEmit()
    {
        dsbox::CommunicationManager comm;

        int count = 0;

        comm.registerEvent<Payload>("testEvent", [&](Payload) { ++count; });
        comm.registerEvent<Payload>("testEvent", [&](Payload) { ++count; });

        comm.emit("testEvent", Payload{});

        DSBOX_CHECK_EQ(count, 2);
    }

    void testEventsDifferentPayloadTypes()
    {
        dsbox::CommunicationManager comm;

        int payloadA = 0;
        int payloadB = 0;

        comm.registerEvent<TypeA>(
            "someEvent",
            [&](TypeA payload) { payloadA = payload.value; });

        comm.registerEvent<TypeB>(
            "someEvent",
            [&](TypeB payload) { payloadB = payload.value; });

        comm.emit("someEvent", TypeA{ 10 });
        comm.emit("someEvent", TypeB{ 20 });

        DSBOX_CHECK_EQ(payloadA, 10);
        DSBOX_CHECK_EQ(payloadB, 20);
    }

    /*
     * unregisterHandler
     */

    void testUnregisterHandler()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<int(Payload)>("foo", [](Payload) { return 1; });

        DSBOX_CHECK(comm.hasHandler<int(Payload)>("foo"));
        DSBOX_CHECK(comm.unregisterHandler<int(Payload)>("foo"));
        DSBOX_CHECK(!comm.hasHandler<int(Payload)>("foo"));
        DSBOX_CHECK(!comm.getHandler<int(Payload)>("foo"));

        // Nothing left to remove the second time.
        DSBOX_CHECK(!comm.unregisterHandler<int(Payload)>("foo"));

        // The slot is free again.
        comm.registerHandler<int(Payload)>("foo", [](Payload) { return 2; });
        DSBOX_CHECK_EQ(comm.getHandler<int(Payload)>("foo")(Payload{}), 2);
    }

    void testUnregisterHandlerMatchesExactSignature()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<int(Payload)>("foo", [](Payload) { return 1; });

        DSBOX_CHECK(!comm.unregisterHandler<TypeA(Payload)>("foo"));
        DSBOX_CHECK(!comm.unregisterHandler<int(TypeA)>("foo"));
        DSBOX_CHECK(!comm.unregisterHandler<int(Payload)>("bar"));

        DSBOX_CHECK(comm.hasHandler<int(Payload)>("foo"));
    }

    void testHandlerHandleInvalidAfterUnregister()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<int(Payload)>("foo", [](Payload) { return 1; });

        auto handler = comm.getHandler<int(Payload)>("foo");
        DSBOX_CHECK(handler);

        comm.unregisterHandler<int(Payload)>("foo");

        DSBOX_CHECK(!handler);

        bool thrown = false;

        try
        {
            handler(Payload{});
        }
        catch (const std::runtime_error&)
        {
            thrown = true;
        }

        DSBOX_CHECK(thrown);
    }

    void testHandlerCanUnregisterItselfWhileRunning()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<int(Payload)>(
            "self",
            [&comm](Payload payload)
            {
                comm.unregisterHandler<int(Payload)>("self");
                return payload.value + 1;
            });

        auto handler = comm.getHandler<int(Payload)>("self");

        DSBOX_CHECK_EQ(handler(Payload{ 1 }), 2);
        DSBOX_CHECK(!comm.hasHandler<int(Payload)>("self"));
    }

    void testHandlesBecomeInvalidWhenManagerIsDestroyed()
    {
        dsbox::HandlerHandle<int(Payload)> handler;
        dsbox::EventHandle<Payload> event;

        {
            dsbox::CommunicationManager comm;

            comm.registerHandler<int(Payload)>("h", [](Payload) { return 1; });
            comm.registerEvent<Payload>("e", [](Payload) {});

            handler = comm.getHandler<int(Payload)>("h");
            event = comm.getEvent<Payload>("e");

            DSBOX_CHECK(handler);
            DSBOX_CHECK(event);
        }

        // Used to be dangling pointers; must now simply report invalid.
        DSBOX_CHECK(!handler);
        DSBOX_CHECK(!event);
    }

    /*
     * unregisterEvent
     */

    void testRegisterEventReturnsDistinctIds()
    {
        dsbox::CommunicationManager comm;

        const auto a = comm.registerEvent<Payload>("e", [](Payload) {});
        const auto b = comm.registerEvent<Payload>("e", [](Payload) {});
        const auto c = comm.registerEvent<TypeA>("e", [](TypeA) {});

        DSBOX_CHECK(a != dsbox::kInvalidSubscription);
        DSBOX_CHECK(b != dsbox::kInvalidSubscription);
        DSBOX_CHECK(c != dsbox::kInvalidSubscription);
        DSBOX_CHECK(a != b);
        DSBOX_CHECK(b != c);
        DSBOX_CHECK(a != c);
    }

    void testUnregisterEvent()
    {
        dsbox::CommunicationManager comm;

        int first = 0;
        int second = 0;

        const auto firstId = comm.registerEvent<Payload>("e", [&](Payload p) { first += p.value; });
        comm.registerEvent<Payload>("e", [&](Payload p) { second += p.value; });

        comm.emit("e", Payload{ 1 });

        DSBOX_CHECK(comm.unregisterEvent(firstId));

        comm.emit("e", Payload{ 10 });

        DSBOX_CHECK_EQ(first, 1);
        DSBOX_CHECK_EQ(second, 11);

        // Harmless to repeat, or to pass junk.
        DSBOX_CHECK(!comm.unregisterEvent(firstId));
        DSBOX_CHECK(!comm.unregisterEvent(dsbox::kInvalidSubscription));
        DSBOX_CHECK(!comm.unregisterEvent(999999));
    }

    void testUnregisterEventOnlyAffectsItsOwnPayloadType()
    {
        dsbox::CommunicationManager comm;

        int a = 0;
        int b = 0;

        const auto idA = comm.registerEvent<TypeA>("e", [&](TypeA) { ++a; });
        comm.registerEvent<TypeB>("e", [&](TypeB) { ++b; });

        DSBOX_CHECK(comm.unregisterEvent(idA));

        comm.emit("e", TypeA{});
        comm.emit("e", TypeB{});

        DSBOX_CHECK_EQ(a, 0);
        DSBOX_CHECK_EQ(b, 1);
        DSBOX_CHECK(!comm.hasSubscribers<TypeA>("e"));
        DSBOX_CHECK(comm.hasSubscribers<TypeB>("e"));
    }

    void testUnregisteringLastSubscriberDropsTheEvent()
    {
        dsbox::CommunicationManager comm;

        const auto id = comm.registerEvent<Payload>("e", [](Payload) {});

        auto handle = comm.getEvent<Payload>("e");
        DSBOX_CHECK(handle);

        DSBOX_CHECK(comm.unregisterEvent(id));

        DSBOX_CHECK(!comm.getEvent<Payload>("e"));
        DSBOX_CHECK(!handle);
        DSBOX_CHECK(comm.listEvents().empty());

        // emit() with nobody listening is a no-op, not an error.
        comm.emit("e", Payload{});

        // And the name can be used again.
        int count = 0;
        comm.registerEvent<Payload>("e", [&](Payload) { ++count; });
        comm.emit("e", Payload{});

        DSBOX_CHECK_EQ(count, 1);
    }

    void testEventHandleSeesLaterSubscribers()
    {
        dsbox::CommunicationManager comm;

        int count = 0;

        comm.registerEvent<Payload>("e", [&](Payload) { ++count; });

        auto event = comm.getEvent<Payload>("e");

        comm.registerEvent<Payload>("e", [&](Payload) { count += 10; });

        event(Payload{});

        DSBOX_CHECK_EQ(count, 11);
    }

    void testUnsubscribeFromInsideCallback()
    {
        dsbox::CommunicationManager comm;

        int selfCalls = 0;
        int otherCalls = 0;

        dsbox::SubscriptionId selfId = dsbox::kInvalidSubscription;
        dsbox::SubscriptionId otherId = dsbox::kInvalidSubscription;

        selfId = comm.registerEvent<Payload>("e", [&](Payload)
        {
            ++selfCalls;
            comm.unregisterEvent(selfId);
            comm.unregisterEvent(otherId);
        });

        otherId = comm.registerEvent<Payload>("e", [&](Payload) { ++otherCalls; });

        // Snapshot semantics: `other` was removed mid-emit but this one
        // event was already going to it.
        comm.emit("e", Payload{});
        DSBOX_CHECK_EQ(selfCalls, 1);
        DSBOX_CHECK_EQ(otherCalls, 1);

        comm.emit("e", Payload{});
        DSBOX_CHECK_EQ(selfCalls, 1);
        DSBOX_CHECK_EQ(otherCalls, 1);

        DSBOX_CHECK_EQ(comm.subscriberCount<Payload>("e"), 0u);
    }

    void testSubscribeFromInsideCallbackSeesNextEmitOnly()
    {
        dsbox::CommunicationManager comm;

        int added = 0;
        bool once = false;

        comm.registerEvent<Payload>("e", [&](Payload)
        {
            if (once)
                return;

            once = true;
            comm.registerEvent<Payload>("e", [&](Payload) { ++added; });
        });

        comm.emit("e", Payload{});
        DSBOX_CHECK_EQ(added, 0);

        comm.emit("e", Payload{});
        DSBOX_CHECK_EQ(added, 1);
    }

    // A callable whose destructor calls back into the manager -- the
    // "captured object owns another subscription" case. Every place that
    // destroys a removed/replaced callback must do so outside the
    // manager's lock, or this deadlocks (the test hangs or aborts with a
    // "resource deadlock" error, depending on the platform's shared_mutex).
    //
    // The destructor only acts while `armed`, so the temporaries and
    // internal copies destroyed during setup don't fire it early.
    struct ReentrantState
    {
        bool armed = false;
        dsbox::SubscriptionId victim = dsbox::kInvalidSubscription;
    };

    struct UnsubscribeWhenDestroyed
    {
        dsbox::CommunicationManager* comm;
        std::shared_ptr<ReentrantState> state;

        UnsubscribeWhenDestroyed(dsbox::CommunicationManager* c, std::shared_ptr<ReentrantState> s)
            : comm(c), state(std::move(s)) {}

        UnsubscribeWhenDestroyed(const UnsubscribeWhenDestroyed&) = default;
        UnsubscribeWhenDestroyed(UnsubscribeWhenDestroyed&&) = default;

        ~UnsubscribeWhenDestroyed()
        {
            if (comm && state && state->armed)
                comm->unregisterEvent(state->victim);
        }

        void operator()(Payload) const {}
    };

    void testRemovedCallbackMayReenterManagerFromItsDestructor()
    {
        dsbox::CommunicationManager comm;

        auto state = std::make_shared<ReentrantState>();

        const auto trigger = comm.registerEvent<Payload>("trigger", UnsubscribeWhenDestroyed{ &comm, state });

        // Phase 1 -- registerEvent(): adding a second subscriber replaces
        // the list that owns a copy of the callback above; destroying
        // that copy re-enters the manager.
        const auto victim1 = comm.registerEvent<Payload>("victim1", [](Payload) {});
        state->armed = true;
        state->victim = victim1;

        comm.registerEvent<Payload>("trigger", [](Payload) {});

        state->armed = false;
        DSBOX_CHECK(!comm.hasSubscribers<Payload>("victim1"));

        // Phase 2 -- unregisterEvent(): the removed callback's
        // destructor re-enters the manager.
        const auto victim2 = comm.registerEvent<Payload>("victim2", [](Payload) {});
        state->armed = true;
        state->victim = victim2;

        DSBOX_CHECK(comm.unregisterEvent(trigger));

        state->armed = false;
        DSBOX_CHECK(!comm.hasSubscribers<Payload>("victim2"));
    }

    /*
     * HandledEvent
     */

    void testHandledEventMultipleSubscribersInOrder()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandledEvent<int(Payload)>("double", [](Payload p) { return p.value * 2; });
        comm.registerHandledEvent<int(Payload)>("double", [](Payload p) { return p.value * 3; });

        const auto results = comm.publishHandledEvent<int(Payload)>("double", Payload{ 5 });

        DSBOX_CHECK_EQ(results.size(), 2u);

        if (results.size() != 2)
            return;

        DSBOX_CHECK(std::holds_alternative<int>(results[0]));
        DSBOX_CHECK(std::holds_alternative<int>(results[1]));
        DSBOX_CHECK_EQ(std::get<int>(results[0]), 10);
        DSBOX_CHECK_EQ(std::get<int>(results[1]), 15);
    }

    void testPublishHandledEventCapturesExceptionsPerSubscriber()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandledEvent<int(Payload)>("risky", [](Payload) -> int { throw std::runtime_error("boom"); });
        comm.registerHandledEvent<int(Payload)>("risky", [](Payload p) { return p.value; });

        const auto results = comm.publishHandledEvent<int(Payload)>("risky", Payload{ 7 });

        DSBOX_CHECK_EQ(results.size(), 2u);

        if (results.size() != 2)
            return;

        // A throwing subscriber doesn't stop the rest from running or
        // losing its own answer -- it turns into the exception_ptr
        // alternative instead of unwinding out of publishHandledEvent().
        DSBOX_CHECK(std::holds_alternative<std::exception_ptr>(results[0]));
        DSBOX_CHECK(std::holds_alternative<int>(results[1]));
        DSBOX_CHECK_EQ(std::get<int>(results[1]), 7);

        bool rethrown = false;

        try
        {
            std::rethrow_exception(std::get<std::exception_ptr>(results[0]));
        }
        catch (const std::runtime_error& error)
        {
            rethrown = true;
            DSBOX_CHECK_EQ(std::string(error.what()), "boom");
        }

        DSBOX_CHECK(rethrown);
    }

    void testPublishHandledEventWithNoSubscribersReturnsEmpty()
    {
        dsbox::CommunicationManager comm;

        const auto results = comm.publishHandledEvent<int(Payload)>("doesNotExist", Payload{});

        DSBOX_CHECK(results.empty());
    }

    void testHandledEventDifferentReturnTypesAreDifferentEndpoints()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandledEvent<int(Payload)>("calc", [](Payload p) { return p.value + 1; });
        comm.registerHandledEvent<TypeA(Payload)>("calc", [](Payload p) { return TypeA{ p.value + 2 }; });

        const auto intResults = comm.publishHandledEvent<int(Payload)>("calc", Payload{ 10 });
        const auto typeAResults = comm.publishHandledEvent<TypeA(Payload)>("calc", Payload{ 10 });

        DSBOX_CHECK_EQ(intResults.size(), 1u);
        DSBOX_CHECK_EQ(typeAResults.size(), 1u);

        if (intResults.size() == 1)
            DSBOX_CHECK_EQ(std::get<int>(intResults[0]), 11);

        if (typeAResults.size() == 1)
            DSBOX_CHECK_EQ(std::get<TypeA>(typeAResults[0]).value, 12);
    }

    void testUnregisterHandledEvent()
    {
        dsbox::CommunicationManager comm;

        const auto firstId = comm.registerHandledEvent<int(Payload)>("e", [](Payload p) { return p.value; });
        comm.registerHandledEvent<int(Payload)>("e", [](Payload p) { return p.value * 10; });

        DSBOX_CHECK(comm.unregisterHandledEvent(firstId));

        const auto results = comm.publishHandledEvent<int(Payload)>("e", Payload{ 3 });

        DSBOX_CHECK_EQ(results.size(), 1u);

        if (results.size() == 1)
            DSBOX_CHECK_EQ(std::get<int>(results[0]), 30);

        // Harmless to repeat, or to pass junk.
        DSBOX_CHECK(!comm.unregisterHandledEvent(firstId));
        DSBOX_CHECK(!comm.unregisterHandledEvent(dsbox::kInvalidSubscription));
    }

    void testUnregisteringLastHandledEventSubscriberDropsIt()
    {
        dsbox::CommunicationManager comm;

        const auto id = comm.registerHandledEvent<int(Payload)>("e", [](Payload p) { return p.value; });

        auto handle = comm.getHandledEvent<int(Payload)>("e");
        DSBOX_CHECK(handle);

        DSBOX_CHECK(comm.unregisterHandledEvent(id));

        DSBOX_CHECK(!comm.getHandledEvent<int(Payload)>("e"));
        DSBOX_CHECK(!handle);
        DSBOX_CHECK(comm.listHandledEvents().empty());

        // publishHandledEvent() with nobody listening is a no-op, not an
        // error.
        DSBOX_CHECK(comm.publishHandledEvent<int(Payload)>("e", Payload{}).empty());

        // And the name can be used again.
        comm.registerHandledEvent<int(Payload)>("e", [](Payload p) { return p.value + 1; });
        const auto results = comm.publishHandledEvent<int(Payload)>("e", Payload{ 1 });

        DSBOX_CHECK_EQ(results.size(), 1u);

        if (results.size() == 1)
            DSBOX_CHECK_EQ(std::get<int>(results[0]), 2);
    }

    void testGetHandledEventHandleSeesLaterSubscribersAndBecomesInvalid()
    {
        dsbox::CommunicationManager comm;

        const auto first = comm.registerHandledEvent<int(Payload)>("e", [](Payload p) { return p.value; });

        auto handle = comm.getHandledEvent<int(Payload)>("e");
        DSBOX_CHECK(handle);

        const auto second = comm.registerHandledEvent<int(Payload)>("e", [](Payload p) { return p.value * 100; });

        const auto results = handle(Payload{ 2 });

        DSBOX_CHECK_EQ(results.size(), 2u);

        if (results.size() == 2)
        {
            DSBOX_CHECK_EQ(std::get<int>(results[0]), 2);
            DSBOX_CHECK_EQ(std::get<int>(results[1]), 200);
        }

        comm.unregisterHandledEvent(first);
        comm.unregisterHandledEvent(second);

        DSBOX_CHECK(!handle);

        bool thrown = false;

        try
        {
            handle(Payload{});
        }
        catch (const std::runtime_error&)
        {
            thrown = true;
        }

        DSBOX_CHECK(thrown);
    }

    /*
     * Introspection
     */

    void testSubscribersListing()
    {
        dsbox::CommunicationManager comm;

        DSBOX_CHECK(comm.subscribers<Payload>("e").empty());
        DSBOX_CHECK_EQ(comm.subscriberCount<Payload>("e"), 0u);
        DSBOX_CHECK(!comm.hasSubscribers<Payload>("e"));

        TestModule modA("module.a");
        TestModule modC("module.c");

        const auto a = comm.registerEvent<Payload>("e", [](Payload) {}, &modA);
        const auto b = comm.registerEvent<Payload>("e", [](Payload) {});
        const auto c = comm.registerEvent<Payload>("e", [](Payload) {}, &modC);

        DSBOX_CHECK(comm.hasSubscribers<Payload>("e"));
        DSBOX_CHECK_EQ(comm.subscriberCount<Payload>("e"), 3u);

        auto list = comm.subscribers<Payload>("e");
        DSBOX_CHECK_EQ(list.size(), 3u);

        if (list.size() != 3)
            return;

        // Subscription order, oldest first.
        DSBOX_CHECK_EQ(list[0].id, a);
        DSBOX_CHECK(list[0].owner == &modA);
        DSBOX_CHECK_EQ(list[0].ownerName, "module.a");
        DSBOX_CHECK_EQ(list[1].id, b);
        DSBOX_CHECK(list[1].owner == dsbox::Anonymous);
        DSBOX_CHECK_EQ(list[1].ownerName, "Anonymous");
        DSBOX_CHECK_EQ(list[2].id, c);
        DSBOX_CHECK(list[2].owner == &modC);
        DSBOX_CHECK_EQ(list[2].ownerName, "module.c");

        comm.unregisterEvent(b);

        list = comm.subscribers<Payload>("e");
        DSBOX_CHECK_EQ(list.size(), 2u);

        if (list.size() != 2)
            return;

        DSBOX_CHECK_EQ(list[0].id, a);
        DSBOX_CHECK_EQ(list[1].id, c);

        // A different payload type under the same name is a different event.
        DSBOX_CHECK(comm.subscribers<TypeA>("e").empty());
    }

    void testListEventsAndHandlers()
    {
        dsbox::CommunicationManager comm;

        TestModule m1("m1");
        TestModule m2("m2");
        TestModule m3("m3");

        comm.registerEvent<Payload>("zeta", [](Payload) {}, &m1);
        comm.registerEvent<Payload>("alpha", [](Payload) {}, &m1);
        comm.registerEvent<Payload>("alpha", [](Payload) {}, &m2);
        comm.registerEvent<TypeA>("alpha", [](TypeA) {});

        comm.registerHandler<int(Payload)>("beta", [](Payload) { return 1; }, &m3);
        comm.registerHandler<TypeA(Payload)>("beta", [](Payload) { return TypeA{ 1 }; });

        const auto events = comm.listEvents();

        // alpha/Payload, alpha/TypeA, zeta/Payload -- sorted by name.
        DSBOX_CHECK_EQ(events.size(), 3u);

        if (events.size() == 3)
        {
            DSBOX_CHECK_EQ(events[0].name, "alpha");
            DSBOX_CHECK_EQ(events[1].name, "alpha");
            DSBOX_CHECK_EQ(events[2].name, "zeta");
        }

        bool foundAlphaPayload = false;

        for (const auto& event : events)
        {
            if (event.name != "alpha" || event.payloadType != std::type_index(typeid(Payload)))
                continue;

            foundAlphaPayload = true;

            DSBOX_CHECK_EQ(event.subscribers.size(), 2u);

            if (event.subscribers.size() == 2)
            {
                DSBOX_CHECK_EQ(event.subscribers[0].ownerName, "m1");
                DSBOX_CHECK_EQ(event.subscribers[1].ownerName, "m2");
            }
        }

        DSBOX_CHECK(foundAlphaPayload);

        const auto handlers = comm.listHandlers();

        DSBOX_CHECK_EQ(handlers.size(), 2u);

        bool foundIntHandler = false;

        for (const auto& handler : handlers)
        {
            DSBOX_CHECK_EQ(handler.name, "beta");
            DSBOX_CHECK(handler.payloadType == std::type_index(typeid(Payload)));

            if (handler.returnType == std::type_index(typeid(int)))
            {
                foundIntHandler = true;
                DSBOX_CHECK(handler.owner == &m3);
                DSBOX_CHECK_EQ(handler.ownerName, "m3");
            }
            else
            {
                DSBOX_CHECK(handler.owner == dsbox::Anonymous);
            }
        }

        DSBOX_CHECK(foundIntHandler);
    }

    void testHandledEventSubscribersListingAndListHandledEvents()
    {
        dsbox::CommunicationManager comm;

        TestModule modA("module.a");
        TestModule modC("module.c");

        const auto a = comm.registerHandledEvent<int(Payload)>("he", [](Payload p) { return p.value; }, &modA);
        const auto b = comm.registerHandledEvent<int(Payload)>("he", [](Payload p) { return p.value; });
        comm.registerHandledEvent<TypeA(Payload)>("he", [](Payload p) { return TypeA{ p.value }; }, &modC);

        DSBOX_CHECK(comm.hasHandledEventSubscribers<int(Payload)>("he"));
        DSBOX_CHECK_EQ(comm.handledEventSubscriberCount<int(Payload)>("he"), 2u);

        auto list = comm.handledEventSubscribers<int(Payload)>("he");
        DSBOX_CHECK_EQ(list.size(), 2u);

        if (list.size() == 2)
        {
            DSBOX_CHECK_EQ(list[0].id, a);
            DSBOX_CHECK_EQ(list[1].id, b);
        }

        const auto handledEvents = comm.listHandledEvents();

        // he/int(Payload), he/TypeA(Payload) -- sorted by name, then
        // payload type, then return type.
        DSBOX_CHECK_EQ(handledEvents.size(), 2u);

        bool foundIntOne = false;

        for (const auto& info : handledEvents)
        {
            DSBOX_CHECK_EQ(info.name, "he");

            if (info.returnType == std::type_index(typeid(int)))
            {
                foundIntOne = true;
                DSBOX_CHECK_EQ(info.subscribers.size(), 2u);
            }
            else
            {
                DSBOX_CHECK_EQ(info.subscribers.size(), 1u);
                DSBOX_CHECK(info.subscribers[0].owner == &modC);
            }
        }

        DSBOX_CHECK(foundIntOne);

        // A plain event/handler under the same name is unrelated.
        DSBOX_CHECK(comm.subscribers<Payload>("he").empty());
    }

    /*
     * ScopedSubscription
     */

    void testScopedSubscriptionUnsubscribesOnDestruction()
    {
        dsbox::CommunicationManager comm;

        int count = 0;
        TestModule scopedOwner("scoped");

        {
            auto sub = comm.registerEventScoped<Payload>("e", [&](Payload) { ++count; }, &scopedOwner);

            DSBOX_CHECK(sub);
            DSBOX_CHECK_EQ(comm.subscriberCount<Payload>("e"), 1u);

            comm.emit("e", Payload{});
            DSBOX_CHECK_EQ(count, 1);
        }

        comm.emit("e", Payload{});

        DSBOX_CHECK_EQ(count, 1);
        DSBOX_CHECK_EQ(comm.subscriberCount<Payload>("e"), 0u);
    }

    void testScopedSubscriptionMoveAndReset()
    {
        dsbox::CommunicationManager comm;

        auto a = comm.registerEventScoped<Payload>("e1", [](Payload) {});
        auto b = comm.registerEventScoped<Payload>("e2", [](Payload) {});

        // Move construction transfers ownership.
        dsbox::ScopedSubscription moved(std::move(a));
        DSBOX_CHECK(!a);
        DSBOX_CHECK(moved);
        DSBOX_CHECK(comm.hasSubscribers<Payload>("e1"));

        // Move assignment first drops what the target already owned.
        moved = std::move(b);
        DSBOX_CHECK(!comm.hasSubscribers<Payload>("e1"));
        DSBOX_CHECK(comm.hasSubscribers<Payload>("e2"));

        moved.reset();
        DSBOX_CHECK(!moved);
        DSBOX_CHECK(!comm.hasSubscribers<Payload>("e2"));

        // Harmless when already empty.
        moved.reset();
    }

    void testScopedSubscriptionRelease()
    {
        dsbox::CommunicationManager comm;

        int count = 0;
        dsbox::SubscriptionId id = dsbox::kInvalidSubscription;

        {
            auto sub = comm.registerEventScoped<Payload>("e", [&](Payload) { ++count; });
            id = sub.release();
            DSBOX_CHECK(!sub);
        }

        // Released, so destruction did not unsubscribe.
        comm.emit("e", Payload{});
        DSBOX_CHECK_EQ(count, 1);

        DSBOX_CHECK(comm.unregisterEvent(id));
    }

    void testScopedHandledEventSubscriptionUnsubscribesOnDestruction()
    {
        dsbox::CommunicationManager comm;

        TestModule scopedOwner("scoped");

        {
            auto sub = comm.registerHandledEventScoped<int(Payload)>(
                "e", [](Payload p) { return p.value; }, &scopedOwner);

            DSBOX_CHECK(sub);
            DSBOX_CHECK_EQ(comm.handledEventSubscriberCount<int(Payload)>("e"), 1u);

            const auto results = comm.publishHandledEvent<int(Payload)>("e", Payload{ 4 });
            DSBOX_CHECK_EQ(results.size(), 1u);
        }

        // A ScopedSubscription from registerHandledEventScoped() must call
        // unregisterHandledEvent(), not unregisterEvent() -- the two keep
        // separate id spaces, so the wrong one would silently do nothing.
        DSBOX_CHECK_EQ(comm.handledEventSubscriberCount<int(Payload)>("e"), 0u);
        DSBOX_CHECK(comm.listHandledEvents().empty());
    }

    /*
     * Owners and unregisterOwner
     */

    void testRegistrationsWithoutOwnerAreAnonymous()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<int(Payload)>("h", [](Payload) { return 1; });
        comm.registerEvent<Payload>("e", [](Payload) {});

        const auto handlers = comm.listHandlers();
        const auto subscribers = comm.subscribers<Payload>("e");

        DSBOX_CHECK_EQ(handlers.size(), 1u);
        DSBOX_CHECK_EQ(subscribers.size(), 1u);

        if (handlers.size() != 1 || subscribers.size() != 1)
            return;

        DSBOX_CHECK(handlers[0].owner == dsbox::Anonymous);
        DSBOX_CHECK(handlers[0].owner == nullptr);
        DSBOX_CHECK_EQ(handlers[0].ownerName, "Anonymous");

        DSBOX_CHECK(subscribers[0].owner == dsbox::Anonymous);
        DSBOX_CHECK_EQ(subscribers[0].ownerName, "Anonymous");
    }

    void testUnregisterOwner()
    {
        dsbox::CommunicationManager comm;

        TestModule a("a");
        TestModule b("b");

        int aCalls = 0;
        int bCalls = 0;
        int anonymousCalls = 0;

        comm.registerHandler<int(Payload)>("calc", [](Payload) { return 1; }, &a);
        comm.registerHandler<int(TypeA)>("calc", [](TypeA) { return 2; }, &b);

        const auto onlyA = comm.registerEvent<Payload>("only-a", [&](Payload) { ++aCalls; }, &a);
        comm.registerEvent<Payload>("shared", [&](Payload) { ++aCalls; }, &a);
        comm.registerEvent<Payload>("shared", [&](Payload) { ++bCalls; }, &b);
        comm.registerEvent<TypeA>("shared", [&](TypeA) { ++anonymousCalls; });

        const auto onlyAHandled = comm.registerHandledEvent<int(Payload)>("only-a-handled", [](Payload p) { return p.value; }, &a);

        auto calc = comm.getHandler<int(Payload)>("calc");
        DSBOX_CHECK(calc);

        // One handler + two event subscriptions + one handled-event
        // subscription.
        const auto removed = comm.unregisterOwner(&a);
        DSBOX_CHECK_EQ(removed, 4u);

        DSBOX_CHECK(!comm.hasHandledEventSubscribers<int(Payload)>("only-a-handled"));
        DSBOX_CHECK(comm.listHandledEvents().empty());
        DSBOX_CHECK(!comm.unregisterHandledEvent(onlyAHandled));

        // a's handler is gone (and cached handles to it are invalid) ...
        DSBOX_CHECK(!comm.hasHandler<int(Payload)>("calc"));
        DSBOX_CHECK(!calc);

        // ... b's handler with the same name but another signature is not.
        DSBOX_CHECK(comm.hasHandler<int(TypeA)>("calc"));

        // An event left without subscribers is dropped, one that still has
        // other owners' subscribers is kept.
        DSBOX_CHECK(!comm.hasSubscribers<Payload>("only-a"));
        DSBOX_CHECK_EQ(comm.subscriberCount<Payload>("shared"), 1u);
        DSBOX_CHECK_EQ(comm.subscriberCount<TypeA>("shared"), 1u);
        DSBOX_CHECK_EQ(comm.listEvents().size(), 2u);

        comm.emit("only-a", Payload{});
        comm.emit("shared", Payload{});
        comm.emit("shared", TypeA{});

        DSBOX_CHECK_EQ(aCalls, 0);
        DSBOX_CHECK_EQ(bCalls, 1);
        DSBOX_CHECK_EQ(anonymousCalls, 1);

        // The subscription ids that belonged to a are dead too.
        DSBOX_CHECK(!comm.unregisterEvent(onlyA));

        // Idempotent.
        const auto again = comm.unregisterOwner(&a);
        DSBOX_CHECK_EQ(again, 0u);
    }

    void testUnregisterAnonymousOwner()
    {
        dsbox::CommunicationManager comm;

        TestModule a("a");

        comm.registerEvent<Payload>("e", [](Payload) {}, &a);
        comm.registerEvent<Payload>("e", [](Payload) {});
        comm.registerHandler<int(Payload)>("h", [](Payload) { return 1; });

        const auto removed = comm.unregisterOwner(dsbox::Anonymous);
        DSBOX_CHECK_EQ(removed, 2u);

        // Only the module's subscription is left.
        DSBOX_CHECK(!comm.hasHandler<int(Payload)>("h"));
        DSBOX_CHECK_EQ(comm.subscriberCount<Payload>("e"), 1u);
    }

    void testUnregisterOwnerOfUnknownModuleDoesNothing()
    {
        dsbox::CommunicationManager comm;

        TestModule a("a");
        TestModule stranger("stranger");

        comm.registerEvent<Payload>("e", [](Payload) {}, &a);

        const auto removed = comm.unregisterOwner(&stranger);
        DSBOX_CHECK_EQ(removed, 0u);
        DSBOX_CHECK_EQ(comm.subscriberCount<Payload>("e"), 1u);
    }

    void testUnregisterOwnerMayReenterManagerFromDestructors()
    {
        dsbox::CommunicationManager comm;

        TestModule a("a");
        auto state = std::make_shared<ReentrantState>();

        const auto victim = comm.registerEvent<Payload>("victim", [](Payload) {});
        comm.registerEvent<Payload>("trigger", UnsubscribeWhenDestroyed{ &comm, state }, &a);

        state->armed = true;
        state->victim = victim;

        // Destroying a's callback re-enters the manager; must not deadlock.
        const auto removed = comm.unregisterOwner(&a);

        state->armed = false;

        DSBOX_CHECK_EQ(removed, 1u);
        DSBOX_CHECK(!comm.hasSubscribers<Payload>("victim"));
    }

    /*
     * comm.debug.* events
     */

    void testDebugEventsReportChanges()
    {
        dsbox::CommunicationManager comm;

        TestModule watcher("watcher");
        TestModule a("mod.a");

        std::vector<std::string> log;

        comm.registerEvent<dsbox::HandlerInfo>(dsbox::kDebugHandlerRegistered,
            [&](dsbox::HandlerInfo info) { log.push_back("handler+ " + info.name + " " + info.ownerName); }, &watcher);

        comm.registerEvent<dsbox::HandlerInfo>(dsbox::kDebugHandlerUnregistered,
            [&](dsbox::HandlerInfo info) { log.push_back("handler- " + info.name + " " + info.ownerName); }, &watcher);

        comm.registerEvent<dsbox::SubscriptionInfo>(dsbox::kDebugEventSubscribed,
            [&](dsbox::SubscriptionInfo info) { log.push_back("sub+ " + info.name + " " + info.subscriber.ownerName); }, &watcher);

        comm.registerEvent<dsbox::SubscriptionInfo>(dsbox::kDebugEventUnsubscribed,
            [&](dsbox::SubscriptionInfo info) { log.push_back("sub- " + info.name + " " + info.subscriber.ownerName); }, &watcher);

        comm.registerEvent<dsbox::HandledSubscriptionInfo>(dsbox::kDebugHandledEventSubscribed,
            [&](dsbox::HandledSubscriptionInfo info) { log.push_back("hsub+ " + info.name + " " + info.subscriber.ownerName); }, &watcher);

        comm.registerEvent<dsbox::HandledSubscriptionInfo>(dsbox::kDebugHandledEventUnsubscribed,
            [&](dsbox::HandledSubscriptionInfo info) { log.push_back("hsub- " + info.name + " " + info.subscriber.ownerName); }, &watcher);

        comm.registerEvent<dsbox::OwnerUnregisteredInfo>(dsbox::kDebugOwnerUnregistered,
            [&](dsbox::OwnerUnregisteredInfo info)
            {
                log.push_back("owner- " + info.ownerName
                    + " h=" + std::to_string(info.handlers)
                    + " s=" + std::to_string(info.subscriptions)
                    + " hs=" + std::to_string(info.handledEventSubscriptions));
            }, &watcher);

        // Setting up the listeners is itself not reported.
        DSBOX_CHECK(log.empty());

        comm.registerHandler<int(Payload)>("calc", [](Payload) { return 1; }, &a);

        const auto tick = comm.registerEvent<Payload>("tick", [](Payload) {}, &a);
        comm.unregisterEvent(tick);

        comm.registerEvent<Payload>("tick2", [](Payload) {}, &a);

        const auto htick = comm.registerHandledEvent<int(Payload)>("htick", [](Payload p) { return p.value; }, &a);
        comm.unregisterHandledEvent(htick);

        comm.registerHandledEvent<int(Payload)>("htick2", [](Payload p) { return p.value; }, &a);

        comm.registerHandler<int(TypeA)>("x", [](TypeA) { return 1; });
        comm.unregisterHandler<int(TypeA)>("x");

        comm.unregisterOwner(&a);

        DSBOX_CHECK_EQ(log.size(), 13u);

        if (log.size() != 13)
            return;

        DSBOX_CHECK_EQ(log[0], "handler+ calc mod.a");
        DSBOX_CHECK_EQ(log[1], "sub+ tick mod.a");
        DSBOX_CHECK_EQ(log[2], "sub- tick mod.a");
        DSBOX_CHECK_EQ(log[3], "sub+ tick2 mod.a");
        DSBOX_CHECK_EQ(log[4], "hsub+ htick mod.a");
        DSBOX_CHECK_EQ(log[5], "hsub- htick mod.a");
        DSBOX_CHECK_EQ(log[6], "hsub+ htick2 mod.a");
        DSBOX_CHECK_EQ(log[7], "handler+ x Anonymous");
        DSBOX_CHECK_EQ(log[8], "handler- x Anonymous");

        // unregisterOwner(): each removed item, handlers first, then
        // events, then handled events, then one summary.
        DSBOX_CHECK_EQ(log[9], "handler- calc mod.a");
        DSBOX_CHECK_EQ(log[10], "sub- tick2 mod.a");
        DSBOX_CHECK_EQ(log[11], "hsub- htick2 mod.a");
        DSBOX_CHECK_EQ(log[12], "owner- mod.a h=1 s=1 hs=1");
    }

    void testDebugEventsIgnoreDebugTopics()
    {
        dsbox::CommunicationManager comm;

        int subscribed = 0;
        int handlerRegistered = 0;

        comm.registerEvent<dsbox::SubscriptionInfo>(dsbox::kDebugEventSubscribed,
            [&](dsbox::SubscriptionInfo) { ++subscribed; });

        comm.registerEvent<dsbox::HandlerInfo>(dsbox::kDebugHandlerRegistered,
            [&](dsbox::HandlerInfo) { ++handlerRegistered; });

        // Changes to comm.debug.* topics are never reported, so listening
        // to the debug events can't feed back into them.
        comm.registerEvent<Payload>("comm.debug.custom", [](Payload) {});
        comm.registerHandler<int(Payload)>("comm.debug.custom_handler", [](Payload) { return 0; });

        DSBOX_CHECK_EQ(subscribed, 0);
        DSBOX_CHECK_EQ(handlerRegistered, 0);

        comm.registerEvent<Payload>("normal", [](Payload) {});
        comm.registerHandler<int(Payload)>("normal", [](Payload) { return 0; });

        DSBOX_CHECK_EQ(subscribed, 1);
        DSBOX_CHECK_EQ(handlerRegistered, 1);
    }

    void testDebugListenerCannotBreakRegistration()
    {
        dsbox::CommunicationManager comm;

        comm.registerEvent<dsbox::SubscriptionInfo>(dsbox::kDebugEventSubscribed,
            [](dsbox::SubscriptionInfo) { throw std::runtime_error("boom"); });

        comm.registerEvent<dsbox::HandlerInfo>(dsbox::kDebugHandlerRegistered,
            [](dsbox::HandlerInfo) { throw std::runtime_error("boom"); });

        int count = 0;

        const auto id = comm.registerEvent<Payload>("e", [&](Payload) { ++count; });
        DSBOX_CHECK(id != dsbox::kInvalidSubscription);

        comm.registerHandler<int(Payload)>("h", [](Payload) { return 7; });
        DSBOX_CHECK_EQ(comm.getHandler<int(Payload)>("h")(Payload{}), 7);

        comm.emit("e", Payload{});
        DSBOX_CHECK_EQ(count, 1);
    }

    void testDebugListenerSeesTheChangeAndMayCallTheManager()
    {
        dsbox::CommunicationManager comm;

        bool visible = false;

        // Sent after the lock was released: the change is already in
        // effect and the listener can query the manager (no deadlock).
        comm.registerEvent<dsbox::HandlerInfo>(dsbox::kDebugHandlerRegistered,
            [&](dsbox::HandlerInfo)
            {
                visible = comm.hasHandler<int(Payload)>("calc");
            });

        comm.registerHandler<int(Payload)>("calc", [](Payload) { return 1; });

        DSBOX_CHECK(visible);
    }

    /*
     * Concurrency
     */

    // Several threads continuously subscribe/unsubscribe while others
    // emit and inspect. Mostly meant to be run under a race detector /
    // sanitizer; on its own it checks nothing crashes or deadlocks and
    // that the bookkeeping ends up consistent.
    void testConcurrentRegisterUnregisterEmit()
    {
        dsbox::CommunicationManager comm;

        constexpr int kWorkers = 4;
        constexpr int kIterations = 2000;

        std::atomic<bool> stop{ false };
        std::atomic<long> delivered{ 0 };

        std::vector<std::thread> emitters;

        for (int i = 0; i < 2; ++i)
        {
            emitters.emplace_back([&]
            {
                while (!stop.load())
                {
                    comm.emit("e", Payload{ 1 });
                    (void)comm.subscriberCount<Payload>("e");
                    (void)comm.listEvents();
                }
            });
        }

        std::vector<std::thread> workers;

        for (int i = 0; i < kWorkers; ++i)
        {
            workers.emplace_back([&]
            {
                for (int n = 0; n < kIterations; ++n)
                {
                    auto sub = comm.registerEventScoped<Payload>("e", [&](Payload) { ++delivered; });
                    DSBOX_CHECK(sub);
                }
            });
        }

        for (auto& worker : workers)
            worker.join();

        stop.store(true);

        for (auto& emitter : emitters)
            emitter.join();

        DSBOX_CHECK_EQ(comm.subscriberCount<Payload>("e"), 0u);
        DSBOX_CHECK(comm.listEvents().empty());
    }
}

void runCommunicationManagerTests()
{
    using namespace dsbox_tests;

    runTest("HandlerCanBeRegisteredAndCalled", testHandlerCanBeRegisteredAndCalled);
    runTest("SameNameDifferentReturnTypes", testSameNameDifferentReturnTypes);
    runTest("MissingHandler", testMissingHandler);
    runTest("DuplicateHandler", testDuplicateHandler);
    runTest("EventMultipleHandlers", testEventMultipleHandlers);
    runTest("Emit", testEmit);
    runTest("EventsDifferentPayloadTypes", testEventsDifferentPayloadTypes);
    runTest("CachedHandler", testCachedHandler);
    runTest("CommunicationManagersAreIndependent", testCommunicationManagersAreIndependent);

    runTest("UnregisterHandler", testUnregisterHandler);
    runTest("UnregisterHandlerMatchesExactSignature", testUnregisterHandlerMatchesExactSignature);
    runTest("HandlerHandleInvalidAfterUnregister", testHandlerHandleInvalidAfterUnregister);
    runTest("HandlerCanUnregisterItselfWhileRunning", testHandlerCanUnregisterItselfWhileRunning);
    runTest("HandlesBecomeInvalidWhenManagerIsDestroyed", testHandlesBecomeInvalidWhenManagerIsDestroyed);

    runTest("RegisterEventReturnsDistinctIds", testRegisterEventReturnsDistinctIds);
    runTest("UnregisterEvent", testUnregisterEvent);
    runTest("UnregisterEventOnlyAffectsItsOwnPayloadType", testUnregisterEventOnlyAffectsItsOwnPayloadType);
    runTest("UnregisteringLastSubscriberDropsTheEvent", testUnregisteringLastSubscriberDropsTheEvent);
    runTest("EventHandleSeesLaterSubscribers", testEventHandleSeesLaterSubscribers);
    runTest("UnsubscribeFromInsideCallback", testUnsubscribeFromInsideCallback);
    runTest("SubscribeFromInsideCallbackSeesNextEmitOnly", testSubscribeFromInsideCallbackSeesNextEmitOnly);
    runTest("RemovedCallbackMayReenterManagerFromItsDestructor", testRemovedCallbackMayReenterManagerFromItsDestructor);

    runTest("HandledEventMultipleSubscribersInOrder", testHandledEventMultipleSubscribersInOrder);
    runTest("PublishHandledEventCapturesExceptionsPerSubscriber", testPublishHandledEventCapturesExceptionsPerSubscriber);
    runTest("PublishHandledEventWithNoSubscribersReturnsEmpty", testPublishHandledEventWithNoSubscribersReturnsEmpty);
    runTest("HandledEventDifferentReturnTypesAreDifferentEndpoints", testHandledEventDifferentReturnTypesAreDifferentEndpoints);
    runTest("UnregisterHandledEvent", testUnregisterHandledEvent);
    runTest("UnregisteringLastHandledEventSubscriberDropsIt", testUnregisteringLastHandledEventSubscriberDropsIt);
    runTest("GetHandledEventHandleSeesLaterSubscribersAndBecomesInvalid", testGetHandledEventHandleSeesLaterSubscribersAndBecomesInvalid);

    runTest("SubscribersListing", testSubscribersListing);
    runTest("ListEventsAndHandlers", testListEventsAndHandlers);
    runTest("HandledEventSubscribersListingAndListHandledEvents", testHandledEventSubscribersListingAndListHandledEvents);

    runTest("ScopedSubscriptionUnsubscribesOnDestruction", testScopedSubscriptionUnsubscribesOnDestruction);
    runTest("ScopedSubscriptionMoveAndReset", testScopedSubscriptionMoveAndReset);
    runTest("ScopedSubscriptionRelease", testScopedSubscriptionRelease);
    runTest("ScopedHandledEventSubscriptionUnsubscribesOnDestruction", testScopedHandledEventSubscriptionUnsubscribesOnDestruction);

    runTest("RegistrationsWithoutOwnerAreAnonymous", testRegistrationsWithoutOwnerAreAnonymous);
    runTest("UnregisterOwner", testUnregisterOwner);
    runTest("UnregisterAnonymousOwner", testUnregisterAnonymousOwner);
    runTest("UnregisterOwnerOfUnknownModuleDoesNothing", testUnregisterOwnerOfUnknownModuleDoesNothing);
    runTest("UnregisterOwnerMayReenterManagerFromDestructors", testUnregisterOwnerMayReenterManagerFromDestructors);

    runTest("DebugEventsReportChanges", testDebugEventsReportChanges);
    runTest("DebugEventsIgnoreDebugTopics", testDebugEventsIgnoreDebugTopics);
    runTest("DebugListenerCannotBreakRegistration", testDebugListenerCannotBreakRegistration);
    runTest("DebugListenerSeesTheChangeAndMayCallTheManager", testDebugListenerSeesTheChangeAndMayCallTheManager);

    runTest("ConcurrentRegisterUnregisterEmit", testConcurrentRegisterUnregisterEmit);
}
