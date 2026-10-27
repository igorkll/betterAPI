#include <dsbox/version.hpp>

#ifndef DSBOX_VERSION_MAJOR
#   define DSBOX_VERSION_MAJOR 0
#endif

#ifndef DSBOX_VERSION_MINOR
#   define DSBOX_VERSION_MINOR 0
#endif

#ifndef DSBOX_VERSION_PATCH
#   define DSBOX_VERSION_PATCH 0
#endif

#ifndef DSBOX_VERSION_STRING
#   define DSBOX_VERSION_STRING "0.0.0"
#endif

namespace dsbox
{
    unsigned int versionMajor() noexcept { return DSBOX_VERSION_MAJOR; }
    unsigned int versionMinor() noexcept { return DSBOX_VERSION_MINOR; }
    unsigned int versionPatch() noexcept { return DSBOX_VERSION_PATCH; }
    const char* versionString() noexcept { return DSBOX_VERSION_STRING; }
}