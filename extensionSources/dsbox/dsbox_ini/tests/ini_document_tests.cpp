#include "test_framework.hpp"

#include <dsbox_ini/ini_document.hpp>

namespace
{
    void testBasicParsing()
    {
        auto doc = dsbox_ini::IniDocument::parse(
            "; comment\n"
            "[modules]\n"
            "console.path = plugins/console.dll\n"
            "console.enabled = true\n"
        );

        DSBOX_INI_CHECK(doc.hasSection("modules"));
        DSBOX_INI_CHECK(!doc.hasSection("nope"));

        DSBOX_INI_CHECK_EQ(doc.get("modules", "console.path"), std::string("plugins/console.dll"));
        DSBOX_INI_CHECK(doc.getBool("modules", "console.enabled", false));
    }

    void testCommentsAndBlankLinesIgnored()
    {
        auto doc = dsbox_ini::IniDocument::parse(
            "# hash comment\n"
            "\n"
            "   \n"
            "[a]\n"
            "; another comment\n"
            "x = 1\n"
        );

        DSBOX_INI_CHECK_EQ(doc.get("a", "x"), std::string("1"));
    }

    void testDefaultSectionForLeadingKeys()
    {
        auto doc = dsbox_ini::IniDocument::parse("root = value\n[a]\nx = 1\n");

        DSBOX_INI_CHECK_EQ(doc.getOr("", "root", "missing"), std::string("value"));
    }

    void testTrimsWhitespace()
    {
        auto doc = dsbox_ini::IniDocument::parse("[a]\n   key   =   value with spaces   \n");

        DSBOX_INI_CHECK_EQ(doc.get("a", "key"), std::string("value with spaces"));
    }

    void testGetOrDefault()
    {
        auto doc = dsbox_ini::IniDocument::parse("[a]\nx = 1\n");

        DSBOX_INI_CHECK_EQ(doc.getOr("a", "missing", "fallback"), std::string("fallback"));
        DSBOX_INI_CHECK_EQ(doc.getOr("missingSection", "x", "fallback"), std::string("fallback"));
    }

    void testGetThrowsOnMissingKey()
    {
        auto doc = dsbox_ini::IniDocument::parse("[a]\nx = 1\n");

        bool thrown = false;
        try
        {
            doc.get("a", "missing");
        }
        catch (const dsbox_ini::IniParseError&)
        {
            thrown = true;
        }

        DSBOX_INI_CHECK(thrown);
    }

    void testGetBoolVariants()
    {
        auto doc = dsbox_ini::IniDocument::parse(
            "[a]\n"
            "t1 = true\ny1 = yes\no1 = on\nn1 = 1\n"
            "t0 = false\ny0 = no\no0 = off\nn0 = 0\n"
        );

        DSBOX_INI_CHECK(doc.getBool("a", "t1", false));
        DSBOX_INI_CHECK(doc.getBool("a", "y1", false));
        DSBOX_INI_CHECK(doc.getBool("a", "o1", false));
        DSBOX_INI_CHECK(doc.getBool("a", "n1", false));

        DSBOX_INI_CHECK(!doc.getBool("a", "t0", true));
        DSBOX_INI_CHECK(!doc.getBool("a", "y0", true));
        DSBOX_INI_CHECK(!doc.getBool("a", "o0", true));
        DSBOX_INI_CHECK(!doc.getBool("a", "n0", true));

        DSBOX_INI_CHECK(doc.getBool("a", "missing", true));
        DSBOX_INI_CHECK(!doc.getBool("a", "missing", false));
    }

    void testGetBoolThrowsOnGarbage()
    {
        auto doc = dsbox_ini::IniDocument::parse("[a]\nx = maybe\n");

        bool thrown = false;
        try
        {
            doc.getBool("a", "x", false);
        }
        catch (const dsbox_ini::IniParseError&)
        {
            thrown = true;
        }

        DSBOX_INI_CHECK(thrown);
    }

    void testDuplicateKeyThrows()
    {
        bool thrown = false;
        try
        {
            dsbox_ini::IniDocument::parse("[a]\nx = 1\nx = 2\n");
        }
        catch (const dsbox_ini::IniParseError&)
        {
            thrown = true;
        }

        DSBOX_INI_CHECK(thrown);
    }

    void testDuplicateSectionThrows()
    {
        bool thrown = false;
        try
        {
            dsbox_ini::IniDocument::parse("[a]\nx = 1\n[a]\ny = 2\n");
        }
        catch (const dsbox_ini::IniParseError&)
        {
            thrown = true;
        }

        DSBOX_INI_CHECK(thrown);
    }

    void testMalformedLineThrows()
    {
        bool thrown = false;
        try
        {
            dsbox_ini::IniDocument::parse("[a]\nthis is not key value\n");
        }
        catch (const dsbox_ini::IniParseError&)
        {
            thrown = true;
        }

        DSBOX_INI_CHECK(thrown);
    }

    void testKeysPreservesOrder()
    {
        auto doc = dsbox_ini::IniDocument::parse("[a]\nfirst = 1\nsecond = 2\nthird = 3\n");

        auto keys = doc.keys("a");

        DSBOX_INI_CHECK_EQ(keys.size(), std::size_t{ 3 });
        DSBOX_INI_CHECK_EQ(keys[0], std::string("first"));
        DSBOX_INI_CHECK_EQ(keys[1], std::string("second"));
        DSBOX_INI_CHECK_EQ(keys[2], std::string("third"));
    }

    void testKeysOnMissingSectionIsEmpty()
    {
        auto doc = dsbox_ini::IniDocument::parse("[a]\nx = 1\n");

        DSBOX_INI_CHECK(doc.keys("missing").empty());
    }

    void testSetUpdatesExistingKey()
    {
        auto doc = dsbox_ini::IniDocument::parse("[a]\nx = 1\n");

        doc.set("a", "x", "2");

        DSBOX_INI_CHECK_EQ(doc.get("a", "x"), std::string("2"));
        DSBOX_INI_CHECK_EQ(doc.keys("a").size(), std::size_t{ 1 });
    }

    void testSetAddsNewKeyToExistingSection()
    {
        auto doc = dsbox_ini::IniDocument::parse("[a]\nx = 1\n");

        doc.set("a", "y", "2");

        DSBOX_INI_CHECK_EQ(doc.get("a", "x"), std::string("1"));
        DSBOX_INI_CHECK_EQ(doc.get("a", "y"), std::string("2"));
    }

    void testSetCreatesNewSection()
    {
        dsbox_ini::IniDocument doc;

        doc.set("fresh", "k", "v");

        DSBOX_INI_CHECK(doc.hasSection("fresh"));
        DSBOX_INI_CHECK_EQ(doc.get("fresh", "k"), std::string("v"));
    }

    void testSetOnDefaultSection()
    {
        dsbox_ini::IniDocument doc;

        doc.set("", "root", "value");

        DSBOX_INI_CHECK_EQ(doc.getOr("", "root", "missing"), std::string("value"));
    }

    void testSerializeRoundTrips()
    {
        auto doc = dsbox_ini::IniDocument::parse("[a]\nx = 1\ny = 2\n[b]\nz = 3\n");

        std::string text = doc.serialize();
        auto reparsed = dsbox_ini::IniDocument::parse(text);

        DSBOX_INI_CHECK_EQ(reparsed.get("a", "x"), std::string("1"));
        DSBOX_INI_CHECK_EQ(reparsed.get("a", "y"), std::string("2"));
        DSBOX_INI_CHECK_EQ(reparsed.get("b", "z"), std::string("3"));
    }

    void testSerializeOmitsCommentsFromOriginal()
    {
        auto doc = dsbox_ini::IniDocument::parse("; a comment\n[a]\nx = 1\n");

        std::string text = doc.serialize();

        DSBOX_INI_CHECK(text.find("comment") == std::string::npos);
    }

    void testSerializeOfEmptyDocumentIsEmpty()
    {
        dsbox_ini::IniDocument doc;

        DSBOX_INI_CHECK(doc.serialize().empty());
    }

    void testSerializeIsStableAcrossRepeatedCalls()
    {
        auto doc = dsbox_ini::IniDocument::parse("[a]\nx = 1\n");

        std::string first = doc.serialize();
        std::string second = doc.serialize();

        DSBOX_INI_CHECK_EQ(first, second);
    }
}

void runIniDocumentTests()
{
    using namespace dsbox_ini_tests;

    runTest("BasicParsing", testBasicParsing);
    runTest("CommentsAndBlankLinesIgnored", testCommentsAndBlankLinesIgnored);
    runTest("DefaultSectionForLeadingKeys", testDefaultSectionForLeadingKeys);
    runTest("TrimsWhitespace", testTrimsWhitespace);
    runTest("GetOrDefault", testGetOrDefault);
    runTest("GetThrowsOnMissingKey", testGetThrowsOnMissingKey);
    runTest("GetBoolVariants", testGetBoolVariants);
    runTest("GetBoolThrowsOnGarbage", testGetBoolThrowsOnGarbage);
    runTest("DuplicateKeyThrows", testDuplicateKeyThrows);
    runTest("DuplicateSectionThrows", testDuplicateSectionThrows);
    runTest("MalformedLineThrows", testMalformedLineThrows);
    runTest("KeysPreservesOrder", testKeysPreservesOrder);
    runTest("KeysOnMissingSectionIsEmpty", testKeysOnMissingSectionIsEmpty);
    runTest("SetUpdatesExistingKey", testSetUpdatesExistingKey);
    runTest("SetAddsNewKeyToExistingSection", testSetAddsNewKeyToExistingSection);
    runTest("SetCreatesNewSection", testSetCreatesNewSection);
    runTest("SetOnDefaultSection", testSetOnDefaultSection);
    runTest("SerializeRoundTrips", testSerializeRoundTrips);
    runTest("SerializeOmitsCommentsFromOriginal", testSerializeOmitsCommentsFromOriginal);
    runTest("SerializeOfEmptyDocumentIsEmpty", testSerializeOfEmptyDocumentIsEmpty);
    runTest("SerializeIsStableAcrossRepeatedCalls", testSerializeIsStableAcrossRepeatedCalls);
}
