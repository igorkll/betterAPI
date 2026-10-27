#pragma once

#include <dsbox/api.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace dsbox
{
    class Context;

    // Base interface every DSBox module implements, regardless of whether
    // it is compiled statically into the binary or loaded at runtime from
    // a shared library (.dll / .so).
    //
    // Lifecycle contract (driven by ModuleManager, in dependency order):
    //   onInit     - register handlers/events on the bus, allocate own state.
    //                Must NOT call into other modules yet: they may not be
    //                initialized at this point.
    //   onStart    - safe to resolve and call other modules' handlers,
    //                since every dependency has already completed onInit
    //                (and onStart, per the topological order).
    //   onStop     - stop doing work, release cross-module handles.
    //   onShutdown - release own resources. Mirrors onInit, called in
    //                reverse dependency order.
    class DSBOX_API IModule
    {
    public:
        virtual ~IModule() = default;

        // Stable unique identifier for this module. Used as the node name
        // in the dependency graph and for diagnostics.
        virtual std::string_view name() const = 0;

        // Names of modules that must be initialized/started before this
        // one, and shut down after it.
        virtual std::vector<std::string> dependencies() const
        {
            return {};
        }

        virtual void onInit(Context& context) { (void)context; }
        virtual void onStart(Context& context) { (void)context; }
        virtual void onStop(Context& context) { (void)context; }
        virtual void onShutdown(Context& context) { (void)context; }
    };
}
