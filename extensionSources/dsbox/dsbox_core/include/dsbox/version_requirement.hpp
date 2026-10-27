#pragma once

#include <dsbox/api.hpp>

#include <string>

namespace dsbox
{
    // major.minor.patch, comparable lexicographically (major first).
    struct DSBOX_API Version
    {
        unsigned major = 0;
        unsigned minor = 0;
        unsigned patch = 0;

        constexpr Version() = default;

        constexpr Version(unsigned majorVersion, unsigned minorVersion, unsigned patchVersion) noexcept
            : major(majorVersion)
            , minor(minorVersion)
            , patch(patchVersion)
        {
        }

        constexpr bool operator==(const Version& other) const noexcept
        {
            return major == other.major && minor == other.minor && patch == other.patch;
        }

        constexpr bool operator!=(const Version& other) const noexcept
        {
            return !(*this == other);
        }

        constexpr bool operator<(const Version& other) const noexcept
        {
            if (major != other.major)
                return major < other.major;

            if (minor != other.minor)
                return minor < other.minor;

            return patch < other.patch;
        }

        constexpr bool operator<=(const Version& other) const noexcept { return !(other < *this); }
        constexpr bool operator>(const Version& other) const noexcept { return other < *this; }
        constexpr bool operator>=(const Version& other) const noexcept { return !(*this < other); }

        std::string toString() const;
    };

    // A constraint a plugin can declare against the host's DSBox core
    // version, checked by ModuleManager::loadDynamic() before the
    // plugin's C++ factory is ever called (see module_loader.hpp and
    // plugin_version.hpp). Build one with the named factory functions
    // below rather than constructing it directly.
    class DSBOX_API VersionRequirement
    {
    public:
        // Accepts core X.*.* -- any minor/patch of major version X.
        // ("caret" range, in semver terms.)
        static VersionRequirement anyMinorPatch(unsigned major);

        // Accepts core X.Y.* -- any patch of major.minor X.Y.
        // ("tilde" range, in semver terms.)
        static VersionRequirement anyPatch(unsigned major, unsigned minor);

        // Accepts exactly X.Y.Z.
        static VersionRequirement exact(Version version);

        // Accepts any version in [minInclusive, maxInclusive].
        static VersionRequirement range(Version minInclusive, Version maxInclusive);

        // No constraint -- matches any version. This is the effective
        // requirement for a plugin that doesn't export
        // DSBox_GetRequiredCoreVersion at all, so existing plugins that
        // predate this system keep loading unchanged.
        static VersionRequirement any();

        bool isSatisfiedBy(Version version) const;

        // Human-readable description for diagnostics, e.g. "^1", "~1.2",
        // "==1.2.3", "[1.0.0, 2.0.0]", "any".
        std::string describe() const;

    private:
        enum class Mode
        {
            Any,
            AnyMinorPatch,
            AnyPatch,
            Exact,
            Range,
        };

        Mode m_mode = Mode::Any;
        Version m_min;
        Version m_max;
        unsigned m_major = 0;
        unsigned m_minor = 0;
    };
}
