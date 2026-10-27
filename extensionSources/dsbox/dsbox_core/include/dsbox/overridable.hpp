#pragma once

#include <dsbox/communication_manager.hpp>

#include <functional>
#include <string>
#include <utility>

namespace dsbox
{
    // Wraps a named handler slot with a built-in fallback: if some module
    // has registered a handler for `name` on the bus, calling this forwards
    // to it; otherwise it runs the fallback implementation supplied at
    // construction. This is the "default behavior with override hook"
    // pattern -- e.g. a ConsoleModule that writes to std::cout by default
    // but defers to whatever the app registers for "console.write".
    //
    // Because CommunicationManager::registerHandler rejects a second
    // registration for the same key, at most one module can ever hold the
    // override, which matches the intended "replace", not "chain", semantics.
    // The override holder can give it up again with
    // CommunicationManager::unregisterHandler(), after which calls go back
    // to the fallback -- even one racing with the unregistration.
    template<typename Signature>
    class Overridable;

    template<typename RT, typename PT>
    class Overridable<RT(PT)>
    {
    public:
        using Fallback = std::function<RT(PT)>;

        Overridable(CommunicationManager& comm, std::string name, Fallback fallback)
            : m_comm(comm)
            , m_name(std::move(name))
            , m_fallback(std::move(fallback))
        {
        }

        RT operator()(PT payload) const
        {
            // pin() takes a strong reference in the same step that
            // finds out whether an override exists. A separate
            // "if (handler) handler(...)" would leave a window in which
            // another thread could unregisterHandler() in between, and
            // this call would then throw instead of using the fallback.
            if (auto entry = m_comm.getHandler<RT(PT)>(m_name).pin())
                return entry->function(std::move(payload));

            return m_fallback(std::move(payload));
        }

        const std::string& name() const noexcept
        {
            return m_name;
        }

        bool isOverridden() const
        {
            return static_cast<bool>(m_comm.getHandler<RT(PT)>(m_name));
        }

    private:
        CommunicationManager& m_comm;
        std::string m_name;
        Fallback m_fallback;
    };
}
