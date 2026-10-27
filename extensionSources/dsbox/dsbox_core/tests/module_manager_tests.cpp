#include "test_framework.hpp"

#include <dsbox/context.hpp>
#include <dsbox/module_manager.hpp>
#include <dsbox/overridable.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    // A module that records lifecycle calls into a shared log, so tests
    // can assert on ordering. Can optionally throw from onInit/onStart to
    // exercise ModuleManager's error-handling paths.
    class RecordingModule final : public dsbox::IModule
    {
    public:
        RecordingModule(
            std::string name,
            std::vector<std::string> deps,
            std::vector<std::string>& log,
            bool throwOnInit = false,
            bool throwOnStart = false)
            : m_name(std::move(name))
            , m_deps(std::move(deps))
            , m_log(log)
            , m_throwOnInit(throwOnInit)
            , m_throwOnStart(throwOnStart)
        {
        }

        std::string_view name() const override
        {
            return m_name;
        }

        std::vector<std::string> dependencies() const override
        {
            return m_deps;
        }

        void onInit(dsbox::Context&) override
        {
            m_log.push_back(m_name + ".init");

            if (m_throwOnInit)
                throw std::runtime_error(m_name + " failed to init");
        }

        void onStart(dsbox::Context&) override
        {
            m_log.push_back(m_name + ".start");

            if (m_throwOnStart)
                throw std::runtime_error(m_name + " failed to start");
        }

        void onStop(dsbox::Context&) override { m_log.push_back(m_name + ".stop"); }
        void onShutdown(dsbox::Context&) override { m_log.push_back(m_name + ".shutdown"); }

    private:
        std::string m_name;
        std::vector<std::string> m_deps;
        std::vector<std::string>& m_log;
        bool m_throwOnInit;
        bool m_throwOnStart;
    };

    std::size_t indexOf(const std::vector<std::string>& log, const std::string& entry)
    {
        for (std::size_t i = 0; i < log.size(); ++i)
            if (log[i] == entry)
                return i;

        return static_cast<std::size_t>(-1);
    }

    void testStartsInDependencyOrder()
    {
        std::vector<std::string> log;

        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        // "b" depends on "a", so "a" must fully init+start before "b" inits.
        modules.registerModule(std::make_unique<RecordingModule>("a", std::vector<std::string>{}, log));
        modules.registerModule(std::make_unique<RecordingModule>("b", std::vector<std::string>{ "a" }, log));

        modules.startAll();

        DSBOX_CHECK(indexOf(log, "a.init") < indexOf(log, "b.init"));
        DSBOX_CHECK(indexOf(log, "a.start") < indexOf(log, "b.start"));
        // Both onInit calls happen before any onStart call.
        DSBOX_CHECK(indexOf(log, "b.init") < indexOf(log, "a.start"));
    }

    void testStopsInReverseOrder()
    {
        std::vector<std::string> log;

        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        modules.registerModule(std::make_unique<RecordingModule>("a", std::vector<std::string>{}, log));
        modules.registerModule(std::make_unique<RecordingModule>("b", std::vector<std::string>{ "a" }, log));

        modules.startAll();
        modules.stopAll();

        DSBOX_CHECK(indexOf(log, "b.stop") < indexOf(log, "a.stop"));
        DSBOX_CHECK(indexOf(log, "b.shutdown") < indexOf(log, "a.shutdown"));
        // Both onStop calls happen before any onShutdown call.
        DSBOX_CHECK(indexOf(log, "a.stop") < indexOf(log, "b.shutdown"));
    }

    void testMissingDependencyThrows()
    {
        std::vector<std::string> log;

        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        modules.registerModule(
            std::make_unique<RecordingModule>("a", std::vector<std::string>{ "ghost" }, log));

        bool thrown = false;
        try
        {
            modules.startAll();
        }
        catch (const dsbox::ModuleError&)
        {
            thrown = true;
        }

        DSBOX_CHECK(thrown);
        DSBOX_CHECK(log.empty());
    }

    void testCycleThrows()
    {
        std::vector<std::string> log;

        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        modules.registerModule(std::make_unique<RecordingModule>("a", std::vector<std::string>{ "b" }, log));
        modules.registerModule(std::make_unique<RecordingModule>("b", std::vector<std::string>{ "a" }, log));

        bool thrown = false;
        try
        {
            modules.startAll();
        }
        catch (const dsbox::ModuleError&)
        {
            thrown = true;
        }

        DSBOX_CHECK(thrown);
        DSBOX_CHECK(log.empty());
    }

    void testDuplicateNameThrows()
    {
        std::vector<std::string> log;

        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        modules.registerModule(std::make_unique<RecordingModule>("a", std::vector<std::string>{}, log));

        bool thrown = false;
        try
        {
            modules.registerModule(std::make_unique<RecordingModule>("a", std::vector<std::string>{}, log));
        }
        catch (const dsbox::ModuleError&)
        {
            thrown = true;
        }

        DSBOX_CHECK(thrown);
    }

    void testUnloadRejectsWhileDependentsRunning()
    {
        std::vector<std::string> log;

        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        modules.registerModule(std::make_unique<RecordingModule>("a", std::vector<std::string>{}, log));
        modules.registerModule(std::make_unique<RecordingModule>("b", std::vector<std::string>{ "a" }, log));

        modules.startAll();

        bool thrown = false;
        try
        {
            modules.unload("a");
        }
        catch (const dsbox::ModuleError&)
        {
            thrown = true;
        }

        DSBOX_CHECK(thrown);
        DSBOX_CHECK(modules.has("a"));
    }

    void testReloadReRunsLifecycle()
    {
        std::vector<std::string> log;

        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        modules.registerModule(std::make_unique<RecordingModule>("a", std::vector<std::string>{}, log));

        modules.startAll();
        log.clear();

        modules.reload("a");

        DSBOX_CHECK_EQ(log.size(), std::size_t{ 4 });
        DSBOX_CHECK_EQ(log[0], std::string("a.stop"));
        DSBOX_CHECK_EQ(log[1], std::string("a.shutdown"));
        DSBOX_CHECK_EQ(log[2], std::string("a.init"));
        DSBOX_CHECK_EQ(log[3], std::string("a.start"));

        DSBOX_CHECK(modules.stateOf("a") == dsbox::ModuleState::Started);
    }

    // Without an error handler, a module failing during startAll() still
    // aborts the whole thing (backward-compatible default) -- but the
    // dependency order must already be recorded for cleanup by then, so
    // stopAll() still tears down whatever DID start in the correct
    // reverse-dependency order.
    void testAbortStillRecordsOrderForCleanup()
    {
        std::vector<std::string> log;

        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        modules.registerModule(std::make_unique<RecordingModule>("a", std::vector<std::string>{}, log));
        modules.registerModule(std::make_unique<RecordingModule>("b", std::vector<std::string>{ "a" }, log));
        modules.registerModule(
            std::make_unique<RecordingModule>(
                "c", std::vector<std::string>{ "b" }, log, /*throwOnInit=*/true));

        bool thrown = false;
        try
        {
            modules.startAll();
        }
        catch (const std::exception&)
        {
            thrown = true;
        }

        DSBOX_CHECK(thrown);

        // "c" fails during the onInit phase, before the onStart phase for
        // ANY module begins (two full passes: all onInit, then all
        // onStart) -- so "a" and "b" only reach Initialized, not Started.
        DSBOX_CHECK(modules.stateOf("a") == dsbox::ModuleState::Initialized);
        DSBOX_CHECK(modules.stateOf("b") == dsbox::ModuleState::Initialized);

        log.clear();
        modules.stopAll();

        // Neither module was ever Started, so onStop never applies -- but
        // both still get onShutdown to release whatever onInit acquired,
        // in reverse dependency order, same as any other cleanup.
        DSBOX_CHECK(indexOf(log, "a.stop") == static_cast<std::size_t>(-1));
        DSBOX_CHECK(indexOf(log, "b.stop") == static_cast<std::size_t>(-1));
        DSBOX_CHECK(indexOf(log, "b.shutdown") < indexOf(log, "a.shutdown"));
    }

    // A Skip-returning error handler isolates the failing module and
    // everything that transitively depends on it, and lets startAll()
    // finish starting everything else instead of aborting.
    void testSkipIsolatesFailingModuleAndItsDependents()
    {
        std::vector<std::string> log;

        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        modules.registerModule(std::make_unique<RecordingModule>("a", std::vector<std::string>{}, log));
        modules.registerModule(
            std::make_unique<RecordingModule>(
                "b", std::vector<std::string>{ "a" }, log, /*throwOnInit=*/true));
        modules.registerModule(std::make_unique<RecordingModule>("c", std::vector<std::string>{ "b" }, log));
        modules.registerModule(std::make_unique<RecordingModule>("d", std::vector<std::string>{}, log));

        std::vector<std::string> failedNames;

        modules.startAll(
            [&](std::string_view name, const std::exception&)
            {
                failedNames.push_back(std::string(name));
                return dsbox::ModuleErrorAction::Skip;
            });

        DSBOX_CHECK_EQ(failedNames.size(), std::size_t{ 1 });
        DSBOX_CHECK_EQ(failedNames[0], std::string("b"));

        DSBOX_CHECK(modules.stateOf("a") == dsbox::ModuleState::Started);
        DSBOX_CHECK(modules.stateOf("d") == dsbox::ModuleState::Started);

        // "b" failed and "c" transitively depends on it -- both stay
        // untouched (Registered), never receiving any lifecycle callback.
        DSBOX_CHECK(modules.stateOf("b") == dsbox::ModuleState::Registered);
        DSBOX_CHECK(modules.stateOf("c") == dsbox::ModuleState::Registered);
        DSBOX_CHECK(indexOf(log, "c.init") == static_cast<std::size_t>(-1));
    }

    // A module that fails during onStart (after onInit already
    // succeeded) gets a matching onShutdown so it isn't left
    // half-initialized, even though it never received onStop.
    void testSkipDuringOnStartShutsDownThatModule()
    {
        std::vector<std::string> log;

        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        modules.registerModule(
            std::make_unique<RecordingModule>(
                "a", std::vector<std::string>{}, log, false, /*throwOnStart=*/true));

        modules.startAll(
            [](std::string_view, const std::exception&) { return dsbox::ModuleErrorAction::Skip; });

        DSBOX_CHECK(modules.stateOf("a") == dsbox::ModuleState::ShutDown);
        DSBOX_CHECK(indexOf(log, "a.init") < indexOf(log, "a.start"));
        DSBOX_CHECK(indexOf(log, "a.start") < indexOf(log, "a.shutdown"));
        DSBOX_CHECK(indexOf(log, "a.stop") == static_cast<std::size_t>(-1));
    }

    // Exercises the "default behavior with override hook" pattern that
    // Overridable<> is meant to support (e.g. dsbox_console::ConsoleModule).
    void testOverridableFallsBackThenDefersToOverride()
    {
        dsbox::CommunicationManager comm;

        std::string sink;

        dsbox::Overridable<void(std::string)> write(
            comm,
            "console.write",
            [&](std::string text) { sink = "default:" + text; });

        write("hello");
        DSBOX_CHECK_EQ(sink, std::string("default:hello"));
        DSBOX_CHECK(!write.isOverridden());

        comm.registerHandler<void(std::string)>(
            "console.write",
            [&](std::string text) { sink = "override:" + text; });

        write("world");
        DSBOX_CHECK_EQ(sink, std::string("override:world"));
        DSBOX_CHECK(write.isOverridden());
    }
}

void runModuleManagerTests()
{
    using namespace dsbox_tests;

    runTest("StartsInDependencyOrder", testStartsInDependencyOrder);
    runTest("StopsInReverseOrder", testStopsInReverseOrder);
    runTest("MissingDependencyThrows", testMissingDependencyThrows);
    runTest("CycleThrows", testCycleThrows);
    runTest("DuplicateNameThrows", testDuplicateNameThrows);
    runTest("UnloadRejectsWhileDependentsRunning", testUnloadRejectsWhileDependentsRunning);
    runTest("ReloadReRunsLifecycle", testReloadReRunsLifecycle);
    runTest("AbortStillRecordsOrderForCleanup", testAbortStillRecordsOrderForCleanup);
    runTest("SkipIsolatesFailingModuleAndItsDependents", testSkipIsolatesFailingModuleAndItsDependents);
    runTest("SkipDuringOnStartShutsDownThatModule", testSkipDuringOnStartShutsDownThatModule);
    runTest("OverridableFallsBackThenDefersToOverride", testOverridableFallsBackThenDefersToOverride);
}
