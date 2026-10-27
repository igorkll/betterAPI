#pragma once

#include <dsbox/module.hpp>

#include <string_view>

namespace dsbox
{
    // "No owner": what handlers/subscriptions belong to when they are
    // registered without a module (the host application, tests, ...).
    //
    // Just a null pointer with a name -- `owner` arguments of the
    // CommunicationManager are plain `const IModule*`, and inside a module
    // you pass `this`:
    //
    //   comm.registerEvent<KeyEvent>("window.key", cb, this);
    //   comm.unregisterOwner(this);     // drop everything this module registered
    //
    // The pointer is only an identity: the CommunicationManager never
    // dereferences it except once, at registration time, to copy the
    // module's name for diagnostics. So a module must be fully constructed
    // (in practice: registering from onInit() or later) when it passes
    // itself, but an owner pointer left behind by a destroyed module is
    // harmless.
    inline constexpr const IModule* Anonymous = nullptr;

    // Name reported for the anonymous owner in diagnostics
    // (SubscriberInfo::ownerName, HandlerInfo::ownerName, debug events).
    inline constexpr std::string_view kAnonymousOwnerName = "Anonymous";
}
