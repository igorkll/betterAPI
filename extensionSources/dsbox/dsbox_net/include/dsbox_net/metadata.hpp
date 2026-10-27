#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dsbox_net
{
    // An ordered list of name/value pairs, with case-insensitive name
    // lookup and repeats allowed -- the shape every wire protocol this
    // library targets actually uses for its "headers" (HTTP headers,
    // FTP-ish reply metadata, ...), so Request/Response carry exactly
    // this instead of each protocol inventing its own.
    //
    // Kept header-only (like dsbox_config::ModuleLoadResult) so it can
    // cross the DSBoxNet.dll boundary freely without its own export
    // macro: it is plain data plus small inline helpers, nothing a
    // protocol handler in a different DLL needs virtual dispatch for.
    //
    // Not thread-safe -- a Metadata instance is only ever touched by
    // whoever currently owns the Request/Response it belongs to, exactly
    // like every other field of those structs.
    class Metadata
    {
    public:
        using Entry = std::pair<std::string, std::string>;

        // Appends a new entry, keeping any existing entries with the
        // same name (use set() instead to replace them).
        void add(std::string name, std::string value)
        {
            m_entries.emplace_back(std::move(name), std::move(value));
        }

        // Removes every existing entry with this name, then adds one
        // entry with `value`.
        void set(std::string_view name, std::string value)
        {
            remove(name);
            add(std::string(name), std::move(value));
        }

        // The first entry's value with this name (case-insensitive), or
        // std::nullopt if there is none.
        std::optional<std::string_view> get(std::string_view name) const
        {
            for (const auto& entry : m_entries)
                if (equalsIgnoreCase(entry.first, name))
                    return std::string_view(entry.second);

            return std::nullopt;
        }

        // Every value recorded under this name, in the order they were
        // added.
        std::vector<std::string_view> getAll(std::string_view name) const
        {
            std::vector<std::string_view> result;

            for (const auto& entry : m_entries)
                if (equalsIgnoreCase(entry.first, name))
                    result.emplace_back(entry.second);

            return result;
        }

        bool has(std::string_view name) const
        {
            return get(name).has_value();
        }

        // Removes every entry with this name. Returns how many were
        // removed.
        std::size_t remove(std::string_view name)
        {
            const auto originalSize = m_entries.size();

            m_entries.erase(
                std::remove_if(
                    m_entries.begin(),
                    m_entries.end(),
                    [name](const Entry& entry) { return equalsIgnoreCase(entry.first, name); }),
                m_entries.end());

            return originalSize - m_entries.size();
        }

        void clear() noexcept
        {
            m_entries.clear();
        }

        bool empty() const noexcept
        {
            return m_entries.empty();
        }

        std::size_t size() const noexcept
        {
            return m_entries.size();
        }

        const std::vector<Entry>& entries() const noexcept
        {
            return m_entries;
        }

        std::vector<Entry>::const_iterator begin() const noexcept
        {
            return m_entries.begin();
        }

        std::vector<Entry>::const_iterator end() const noexcept
        {
            return m_entries.end();
        }

    private:
        static bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
        {
            if (a.size() != b.size())
                return false;

            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }

            return true;
        }

        std::vector<Entry> m_entries;
    };
}
