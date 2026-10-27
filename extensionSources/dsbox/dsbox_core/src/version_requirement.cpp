#include <dsbox/version_requirement.hpp>

namespace dsbox
{
    std::string Version::toString() const
    {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    VersionRequirement VersionRequirement::anyMinorPatch(unsigned major)
    {
        VersionRequirement req;
        req.m_mode = Mode::AnyMinorPatch;
        req.m_major = major;
        return req;
    }

    VersionRequirement VersionRequirement::anyPatch(unsigned major, unsigned minor)
    {
        VersionRequirement req;
        req.m_mode = Mode::AnyPatch;
        req.m_major = major;
        req.m_minor = minor;
        return req;
    }

    VersionRequirement VersionRequirement::exact(Version version)
    {
        VersionRequirement req;
        req.m_mode = Mode::Exact;
        req.m_min = version;
        return req;
    }

    VersionRequirement VersionRequirement::range(Version minInclusive, Version maxInclusive)
    {
        VersionRequirement req;
        req.m_mode = Mode::Range;
        req.m_min = minInclusive;
        req.m_max = maxInclusive;
        return req;
    }

    VersionRequirement VersionRequirement::any()
    {
        return VersionRequirement{};
    }

    bool VersionRequirement::isSatisfiedBy(Version version) const
    {
        switch (m_mode)
        {
            case Mode::Any:
                return true;
            case Mode::AnyMinorPatch:
                return version.major == m_major;
            case Mode::AnyPatch:
                return version.major == m_major && version.minor == m_minor;
            case Mode::Exact:
                return version == m_min;
            case Mode::Range:
                return version >= m_min && version <= m_max;
        }

        return false;
    }

    std::string VersionRequirement::describe() const
    {
        switch (m_mode)
        {
            case Mode::Any:
                return "any";
            case Mode::AnyMinorPatch:
                return "^" + std::to_string(m_major);
            case Mode::AnyPatch:
                return "~" + std::to_string(m_major) + "." + std::to_string(m_minor);
            case Mode::Exact:
                return "==" + m_min.toString();
            case Mode::Range:
                return "[" + m_min.toString() + ", " + m_max.toString() + "]";
        }

        return "?";
    }
}
