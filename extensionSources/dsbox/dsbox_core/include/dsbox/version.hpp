#pragma once

#include <dsbox/api.hpp>

namespace dsbox
{
    // Accessor functions rather than exported data symbols: a
    // /DELAYLOAD'd DLL (see cmake/DSBoxDelayLoad.cmake) can only defer
    // resolving imported FUNCTIONS -- the linker rejects a delay-loaded
    // import of a data symbol outright (LNK1194), since there is no call
    // site for delayimp to hook. Any executable that delay-loads
    // DSBoxCore.dll (every DSBox:: host, by convention) must therefore
    // reach these through a function call, never through a plain
    // `extern const` global.
    DSBOX_API unsigned int versionMajor() noexcept;
    DSBOX_API unsigned int versionMinor() noexcept;
    DSBOX_API unsigned int versionPatch() noexcept;
    DSBOX_API const char* versionString() noexcept;
}