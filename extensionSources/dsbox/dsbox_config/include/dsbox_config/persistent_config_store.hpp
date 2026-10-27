#pragma once

#include <dsbox_config/api.hpp>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace dsbox_config
{
    // Controls how PersistentConfigStore::serializeOverrides() turns the
    // current in-memory overrides back into INI text.
    enum class ConfigSaveMode
    {
        // Default. A clean, fresh rewrite of every override currently
        // known, in the store's own section/key order. Simple and
        // predictable, but comments and any original formatting in the
        // overrides file are not preserved.
        Regenerate,

        // Patches only the values that actually changed into a copy of
        // the ORIGINAL overrides text this store was loaded with --
        // comments, blank lines, and existing key/section order are left
        // untouched. Brand new keys/sections are appended. Best-effort:
        // meant for a config.ini a person might hand-edit and annotate,
        // where preserving those annotations across a save matters more
        // than a perfectly minimal diff.
        PreserveFormatting,
    };

    // A layered, persistent key-value configuration store:
    //
    //   PersistentConfigStore::load(defaultsIniText, overridesIniText)
    //
    // Reads (via the host -- this class never touches the filesystem,
    // consistent with the rest of dsbox_config) two INI documents: a
    // read-only "defaults" layer and an "overrides" layer on top. A
    // lookup checks overrides first, falling back to defaults, then to
    // the caller-supplied default if the key is in neither.
    //
    // set() only ever updates the in-memory overrides layer. Nothing is
    // written to disk automatically -- call serializeOverrides() and
    // write its result to your overrides file (typically config.ini)
    // yourself, then markSaved(). This keeps "when do we touch disk" a
    // decision the host controls explicitly, the same way
    // ModuleConfigLoader never reads files on its own.
    //
    // Because set() only ever touches the overrides layer, and
    // serializeOverrides() only ever serializes that layer, config.ini
    // stays a minimal diff from config/default/config.ini -- a key the
    // user never touched keeps tracking whatever the shipped default is,
    // even across app updates that change that default.
    //
    // Thread safety: every method is safe to call concurrently from
    // multiple threads.
    class DSBOX_CONFIG_API PersistentConfigStore
    {
    public:
        static PersistentConfigStore load(
            std::string_view defaultsIniText,
            std::string_view overridesIniText);

        PersistentConfigStore(const PersistentConfigStore&) = delete;
        PersistentConfigStore& operator=(const PersistentConfigStore&) = delete;

        PersistentConfigStore(PersistentConfigStore&&) noexcept;
        PersistentConfigStore& operator=(PersistentConfigStore&&) noexcept;

        ~PersistentConfigStore();

        std::string getOr(
            std::string_view section,
            std::string_view key,
            std::string defaultValue) const;

        bool getBool(std::string_view section, std::string_view key, bool defaultValue) const;

        // Updates the in-memory overrides layer only. Call
        // serializeOverrides() (and write the result to disk yourself)
        // to persist this.
        void set(std::string_view section, std::string_view key, std::string value);

        // True if set() has been called since load() (or since the last
        // markSaved()).
        bool isDirty() const;

        std::string serializeOverrides(ConfigSaveMode mode = ConfigSaveMode::Regenerate) const;

        // Clears the dirty flag. Call this after successfully writing
        // serializeOverrides()'s result to disk.
        void markSaved();

    private:
        PersistentConfigStore();

        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
