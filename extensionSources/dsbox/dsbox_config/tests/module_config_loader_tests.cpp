#include "test_framework.hpp"

#include <dsbox/context.hpp>
#include <dsbox/module_manager.hpp>

#include <dsbox_config/module_config_loader.hpp>

namespace
{
    void testNoModulesSectionIsNoop()
    {
        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        auto report = dsbox_config::ModuleConfigLoader::loadFromIni(
            modules, "[other]\nx = 1\n");

        DSBOX_CONFIG_CHECK(report.results.empty());
        DSBOX_CONFIG_CHECK(report.allSucceeded());
    }

    void testDisabledEntrySkippedNotCountedAsFailure()
    {
        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        auto report = dsbox_config::ModuleConfigLoader::loadFromIni(
            modules,
            "[modules]\n"
            "telemetry.path = does/not/exist.so\n"
            "telemetry.enabled = false\n");

        DSBOX_CONFIG_CHECK_EQ(report.results.size(), std::size_t{ 1 });
        DSBOX_CONFIG_CHECK(!report.results[0].enabled);
        DSBOX_CONFIG_CHECK(!report.results[0].loaded);
        DSBOX_CONFIG_CHECK(report.allSucceeded());
        DSBOX_CONFIG_CHECK(report.failures().empty());
    }

    void testMissingLibraryRecordedAsFailure()
    {
        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        auto report = dsbox_config::ModuleConfigLoader::loadFromIni(
            modules, "[modules]\nghost.path = does/not/exist.so\n");

        DSBOX_CONFIG_CHECK_EQ(report.results.size(), std::size_t{ 1 });
        DSBOX_CONFIG_CHECK(report.results[0].enabled);
        DSBOX_CONFIG_CHECK(!report.results[0].loaded);
        DSBOX_CONFIG_CHECK(!report.results[0].error.empty());
        DSBOX_CONFIG_CHECK(!report.allSucceeded());
        DSBOX_CONFIG_CHECK_EQ(report.failures().size(), std::size_t{ 1 });
    }

    void testMalformedIniRecordedAsSingleFailure()
    {
        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        auto report = dsbox_config::ModuleConfigLoader::loadFromIni(
            modules, "this is not valid ini\n");

        DSBOX_CONFIG_CHECK_EQ(report.results.size(), std::size_t{ 1 });
        DSBOX_CONFIG_CHECK_EQ(report.results[0].alias, std::string("<ini>"));
        DSBOX_CONFIG_CHECK(!report.allSucceeded());
    }

    void testBaseDirResolvesRelativePaths()
    {
        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        auto report = dsbox_config::ModuleConfigLoader::loadFromIni(
            modules, "[modules]\nghost.path = plugin.so\n", "/opt/dsbox/plugins");

        DSBOX_CONFIG_CHECK_EQ(report.results.size(), std::size_t{ 1 });
        DSBOX_CONFIG_CHECK_EQ(report.results[0].path, std::string("/opt/dsbox/plugins/plugin.so"));
    }

    void testAbsolutePathIgnoresBaseDir()
    {
        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        auto report = dsbox_config::ModuleConfigLoader::loadFromIni(
            modules, "[modules]\nghost.path = /already/absolute.so\n", "/opt/dsbox/plugins");

        DSBOX_CONFIG_CHECK_EQ(report.results.size(), std::size_t{ 1 });
        DSBOX_CONFIG_CHECK_EQ(report.results[0].path, std::string("/already/absolute.so"));
    }

    void testInvalidEnabledValueRecordedAsFailure()
    {
        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        auto report = dsbox_config::ModuleConfigLoader::loadFromIni(
            modules,
            "[modules]\n"
            "ghost.path = plugin.so\n"
            "ghost.enabled = maybe\n");

        DSBOX_CONFIG_CHECK_EQ(report.results.size(), std::size_t{ 1 });
        DSBOX_CONFIG_CHECK(!report.results[0].loaded);
        DSBOX_CONFIG_CHECK(!report.results[0].error.empty());
        DSBOX_CONFIG_CHECK(!report.allSucceeded());
    }

    void testMultipleAliasesEachLoadedIndependently()
    {
        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        auto report = dsbox_config::ModuleConfigLoader::loadFromIni(
            modules,
            "[modules]\n"
            "a.path = missing_a.so\n"
            "b.path = missing_b.so\n");

        DSBOX_CONFIG_CHECK_EQ(report.results.size(), std::size_t{ 2 });
        DSBOX_CONFIG_CHECK_EQ(report.failures().size(), std::size_t{ 2 });
    }
}

void runModuleConfigLoaderTests()
{
    using namespace dsbox_config_tests;

    runTest("NoModulesSectionIsNoop", testNoModulesSectionIsNoop);
    runTest("DisabledEntrySkippedNotCountedAsFailure", testDisabledEntrySkippedNotCountedAsFailure);
    runTest("MissingLibraryRecordedAsFailure", testMissingLibraryRecordedAsFailure);
    runTest("MalformedIniRecordedAsSingleFailure", testMalformedIniRecordedAsSingleFailure);
    runTest("BaseDirResolvesRelativePaths", testBaseDirResolvesRelativePaths);
    runTest("AbsolutePathIgnoresBaseDir", testAbsolutePathIgnoresBaseDir);
    runTest("InvalidEnabledValueRecordedAsFailure", testInvalidEnabledValueRecordedAsFailure);
    runTest("MultipleAliasesEachLoadedIndependently", testMultipleAliasesEachLoadedIndependently);
}
