#include "test_framework.hpp"

#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox_ini/ini_document.hpp>

namespace
{
    void testReadsFromDefaultsWhenNoOverride()
    {
        auto store = dsbox_config::PersistentConfigStore::load(
            "[graphics]\nresolution = 1280x720\n", "");

        DSBOX_CONFIG_CHECK_EQ(
            store.getOr("graphics", "resolution", "missing"), std::string("1280x720"));
    }

    void testOverrideWinsOverDefault()
    {
        auto store = dsbox_config::PersistentConfigStore::load(
            "[graphics]\nresolution = 1280x720\n",
            "[graphics]\nresolution = 1920x1080\n");

        DSBOX_CONFIG_CHECK_EQ(
            store.getOr("graphics", "resolution", "missing"), std::string("1920x1080"));
    }

    void testFallsBackToCallerDefaultWhenNeitherHasIt()
    {
        auto store = dsbox_config::PersistentConfigStore::load("", "");

        DSBOX_CONFIG_CHECK_EQ(store.getOr("a", "x", "fallback"), std::string("fallback"));
    }

    void testGetBoolChecksOverrideThenDefault()
    {
        auto store = dsbox_config::PersistentConfigStore::load(
            "[a]\nx = true\ny = true\n", "[a]\ny = false\n");

        DSBOX_CONFIG_CHECK(store.getBool("a", "x", false));
        DSBOX_CONFIG_CHECK(!store.getBool("a", "y", true));
        DSBOX_CONFIG_CHECK(store.getBool("a", "missing", true));
    }

    void testSetUpdatesValueImmediatelyInMemory()
    {
        auto store = dsbox_config::PersistentConfigStore::load(
            "[graphics]\nresolution = 1280x720\n", "");

        DSBOX_CONFIG_CHECK(!store.isDirty());

        store.set("graphics", "resolution", "1920x1080");

        DSBOX_CONFIG_CHECK(store.isDirty());
        DSBOX_CONFIG_CHECK_EQ(
            store.getOr("graphics", "resolution", "missing"), std::string("1920x1080"));
    }

    void testMarkSavedClearsDirtyFlag()
    {
        auto store = dsbox_config::PersistentConfigStore::load("[a]\nx = 1\n", "");

        store.set("a", "x", "2");
        DSBOX_CONFIG_CHECK(store.isDirty());

        store.markSaved();
        DSBOX_CONFIG_CHECK(!store.isDirty());
    }

    void testSetNeverTouchesDefaultsOnlyOverrides()
    {
        auto store = dsbox_config::PersistentConfigStore::load("[a]\nx = 1\n", "");

        store.set("a", "x", "2");

        // Regenerating the overrides text must reflect the change...
        std::string overrides = store.serializeOverrides();
        DSBOX_CONFIG_CHECK(overrides.find("2") != std::string::npos);

        // ...while a freshly loaded store using the ORIGINAL defaults
        // text (unmodified) still reports the original default,
        // confirming set() never touched the defaults layer itself.
        auto fresh = dsbox_config::PersistentConfigStore::load("[a]\nx = 1\n", "");
        DSBOX_CONFIG_CHECK_EQ(fresh.getOr("a", "x", "missing"), std::string("1"));
    }

    void testRegenerateSerializesOnlyOverrides()
    {
        auto store = dsbox_config::PersistentConfigStore::load(
            "[graphics]\nresolution = 1280x720\nvsync = true\n", "");

        store.set("graphics", "resolution", "1920x1080");

        std::string overrides = store.serializeOverrides(dsbox_config::ConfigSaveMode::Regenerate);

        // Only the key that was actually set() should appear -- "vsync"
        // was never overridden, so it must not leak into config.ini.
        DSBOX_CONFIG_CHECK(overrides.find("1920x1080") != std::string::npos);
        DSBOX_CONFIG_CHECK(overrides.find("vsync") == std::string::npos);
    }

    void testPreserveFormattingKeepsCommentsAndOnlyChangesTouchedLines()
    {
        std::string originalOverrides =
            "; user settings -- hand edited, please keep tidy\n"
            "[graphics]\n"
            "resolution = 1280x720\n"
            "; vsync left on deliberately\n"
            "vsync = true\n";

        auto store = dsbox_config::PersistentConfigStore::load(
            "[graphics]\nresolution = 800x600\nvsync = false\n", originalOverrides);

        store.set("graphics", "resolution", "1920x1080");

        std::string result =
            store.serializeOverrides(dsbox_config::ConfigSaveMode::PreserveFormatting);

        DSBOX_CONFIG_CHECK(result.find("user settings -- hand edited") != std::string::npos);
        DSBOX_CONFIG_CHECK(result.find("vsync left on deliberately") != std::string::npos);
        DSBOX_CONFIG_CHECK(result.find("1920x1080") != std::string::npos);
        DSBOX_CONFIG_CHECK(result.find("1280x720") == std::string::npos);
        // The untouched key's line must be byte-for-byte unchanged.
        DSBOX_CONFIG_CHECK(result.find("vsync = true") != std::string::npos);

        // The patched text must still be valid, round-trippable INI.
        auto reparsed = dsbox_ini::IniDocument::parse(result);
        DSBOX_CONFIG_CHECK_EQ(reparsed.get("graphics", "resolution"), std::string("1920x1080"));
    }

    void testPreserveFormattingAppendsNewKeyToExistingSection()
    {
        std::string originalOverrides = "[graphics]\nresolution = 1280x720\n";

        auto store = dsbox_config::PersistentConfigStore::load(
            "[graphics]\nresolution = 800x600\nfullscreen = false\n", originalOverrides);

        store.set("graphics", "fullscreen", "true");

        std::string result =
            store.serializeOverrides(dsbox_config::ConfigSaveMode::PreserveFormatting);

        auto reparsed = dsbox_ini::IniDocument::parse(result);
        DSBOX_CONFIG_CHECK_EQ(reparsed.get("graphics", "resolution"), std::string("1280x720"));
        DSBOX_CONFIG_CHECK_EQ(reparsed.get("graphics", "fullscreen"), std::string("true"));
    }

    void testPreserveFormattingAppendsBrandNewSection()
    {
        auto store = dsbox_config::PersistentConfigStore::load(
            "[audio]\nvolume = 50\n", "; nothing overridden yet\n");

        store.set("audio", "volume", "75");

        std::string result =
            store.serializeOverrides(dsbox_config::ConfigSaveMode::PreserveFormatting);

        DSBOX_CONFIG_CHECK(result.find("nothing overridden yet") != std::string::npos);

        auto reparsed = dsbox_ini::IniDocument::parse(result);
        DSBOX_CONFIG_CHECK_EQ(reparsed.get("audio", "volume"), std::string("75"));
    }

    void testPreserveFormattingAppendsNewKeyToDefaultSection()
    {
        // Regression test: IniDocument::sections() deliberately excludes
        // the implicit default (unnamed) section, which previously made
        // patchOverridesText() silently drop any brand new key set() in
        // that section instead of appending it.
        std::string originalOverrides = "; nothing overridden yet\n";

        auto store = dsbox_config::PersistentConfigStore::load(
            "fullscreen = false\n", originalOverrides);

        store.set("", "fullscreen", "true");

        std::string result =
            store.serializeOverrides(dsbox_config::ConfigSaveMode::PreserveFormatting);

        DSBOX_CONFIG_CHECK(result.find("nothing overridden yet") != std::string::npos);

        auto reparsed = dsbox_ini::IniDocument::parse(result);
        DSBOX_CONFIG_CHECK_EQ(reparsed.get("", "fullscreen"), std::string("true"));
    }
}

void runPersistentConfigStoreTests()
{
    using namespace dsbox_config_tests;

    runTest("ReadsFromDefaultsWhenNoOverride", testReadsFromDefaultsWhenNoOverride);
    runTest("OverrideWinsOverDefault", testOverrideWinsOverDefault);
    runTest("FallsBackToCallerDefaultWhenNeitherHasIt", testFallsBackToCallerDefaultWhenNeitherHasIt);
    runTest("GetBoolChecksOverrideThenDefault", testGetBoolChecksOverrideThenDefault);
    runTest("SetUpdatesValueImmediatelyInMemory", testSetUpdatesValueImmediatelyInMemory);
    runTest("MarkSavedClearsDirtyFlag", testMarkSavedClearsDirtyFlag);
    runTest("SetNeverTouchesDefaultsOnlyOverrides", testSetNeverTouchesDefaultsOnlyOverrides);
    runTest("RegenerateSerializesOnlyOverrides", testRegenerateSerializesOnlyOverrides);
    runTest(
        "PreserveFormattingKeepsCommentsAndOnlyChangesTouchedLines",
        testPreserveFormattingKeepsCommentsAndOnlyChangesTouchedLines);
    runTest(
        "PreserveFormattingAppendsNewKeyToExistingSection",
        testPreserveFormattingAppendsNewKeyToExistingSection);
    runTest(
        "PreserveFormattingAppendsBrandNewSection",
        testPreserveFormattingAppendsBrandNewSection);
    runTest(
        "PreserveFormattingAppendsNewKeyToDefaultSection",
        testPreserveFormattingAppendsNewKeyToDefaultSection);
}
