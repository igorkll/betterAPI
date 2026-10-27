#pragma once

#include <dsbox_ini/api.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dsbox_ini
{
    // Thrown for genuinely malformed input: a non-blank, non-comment line
    // that is neither a "[section]" header nor a "key = value" pair, a
    // key repeated within the same section, or an unparsable boolean
    // value requested via getBool().
    class DSBOX_INI_API IniParseError : public std::runtime_error
    {
    public:
        explicit IniParseError(const std::string& message)
            : std::runtime_error(message)
        {
        }
    };

    // A minimal, dependency-free INI-style document parser:
    //
    //   ; comment
    //   # also a comment
    //   [section]
    //   key = value
    //
    // - Leading/trailing whitespace around keys and values is trimmed.
    // - Keys appearing before any "[section]" header live in the ""
    //   (default) section.
    // - getBool() accepts true/false/1/0/yes/no/on/off, case-insensitive.
    //
    // This class only knows about generic INI syntax; it has no idea what
    // any particular section or key means (that's up to the consumer,
    // e.g. dsbox_config's ModuleConfigLoader).
    class DSBOX_INI_API IniDocument
    {
    public:
        // Throws IniParseError on the first malformed line or duplicate
        // key it encounters.
        static IniDocument parse(std::string_view text);

        bool hasSection(std::string_view section) const;
        bool hasKey(std::string_view section, std::string_view key) const;

        // Throws IniParseError if the key is missing.
        const std::string& get(std::string_view section, std::string_view key) const;

        std::string getOr(
            std::string_view section,
            std::string_view key,
            std::string defaultValue) const;

        // Returns `defaultValue` if the key is absent; throws
        // IniParseError if the key is present but its value isn't one of
        // the recognized boolean spellings.
        bool getBool(
            std::string_view section,
            std::string_view key,
            bool defaultValue) const;

        // Keys within `section`, in the order they appeared in the
        // source text. Empty if the section does not exist.
        std::vector<std::string> keys(std::string_view section) const;

        // Declared section names, in file order. Does not include the
        // implicit "" default section.
        std::vector<std::string> sections() const;

        // Inserts a new key or updates an existing one. Creates the
        // section first if it doesn't exist yet (including the implicit
        // "" default section, if this document wasn't produced by
        // parse()).
        void set(std::string_view section, std::string_view key, std::string value);

        // Reconstructs INI text from this document's current sections
        // and keys, in their current order. This is always a clean,
        // fresh rewrite -- comments and original formatting are never
        // preserved, even for a document that came from parse(). See
        // dsbox_config::PersistentConfigStore for a formatting-preserving
        // alternative where that matters.
        std::string serialize() const;

    private:
        using KeyValues = std::vector<std::pair<std::string, std::string>>;

        std::vector<std::pair<std::string, KeyValues>> m_sections;

        KeyValues* findSection(std::string_view section);
        const KeyValues* findSection(std::string_view section) const;
    };
}
