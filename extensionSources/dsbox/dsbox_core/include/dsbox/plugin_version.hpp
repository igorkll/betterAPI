#pragma once

#include <dsbox/module_loader.hpp>
#include <dsbox/plugin_export.hpp>

// Convenience macros for a plugin to declare which DSBox core version(s)
// it was built against. Place exactly ONE of these once in a plugin's
// .cpp, alongside DSBox_CreateModule/DSBox_DestroyModule. If none is
// used, the plugin is treated as compatible with any core version (the
// same as every plugin written before this system existed).
//
//   #include <dsbox/plugin_version.hpp>
//
//   DSBOX_REQUIRE_CORE_VERSION_MAJOR(1)        // any 1.x.y
//   DSBOX_REQUIRE_CORE_VERSION_MINOR(1, 2)     // any 1.2.z
//   DSBOX_REQUIRE_CORE_VERSION_EXACT(1, 2, 3)  // exactly 1.2.3
//   DSBOX_REQUIRE_CORE_VERSION_RANGE(1,0,0, 2,0,0)  // 1.0.0 through 2.0.0 inclusive
//
// ModuleManager::loadDynamic() checks this before calling
// DSBox_CreateModule() at all, so an incompatible plugin fails with a
// clear message instead of risking undefined behavior from an actual ABI
// mismatch.

#define DSBOX_REQUIRE_CORE_VERSION_MAJOR(REQUIRED_MAJOR)                     \
    DSBOX_PLUGIN_EXPORT void DSBox_GetRequiredCoreVersion(                   \
        ::dsbox::DSBoxVersionRequirementC* out)                             \
    {                                                                        \
        *out = ::dsbox::DSBoxVersionRequirementC{                            \
            1, (REQUIRED_MAJOR), 0, 0, 0, 0, 0 };                            \
    }

#define DSBOX_REQUIRE_CORE_VERSION_MINOR(REQUIRED_MAJOR, REQUIRED_MINOR)     \
    DSBOX_PLUGIN_EXPORT void DSBox_GetRequiredCoreVersion(                   \
        ::dsbox::DSBoxVersionRequirementC* out)                             \
    {                                                                        \
        *out = ::dsbox::DSBoxVersionRequirementC{                            \
            2, (REQUIRED_MAJOR), (REQUIRED_MINOR), 0, 0, 0, 0 };             \
    }

#define DSBOX_REQUIRE_CORE_VERSION_EXACT(REQUIRED_MAJOR, REQUIRED_MINOR, REQUIRED_PATCH) \
    DSBOX_PLUGIN_EXPORT void DSBox_GetRequiredCoreVersion(                   \
        ::dsbox::DSBoxVersionRequirementC* out)                             \
    {                                                                        \
        *out = ::dsbox::DSBoxVersionRequirementC{                            \
            3, (REQUIRED_MAJOR), (REQUIRED_MINOR), (REQUIRED_PATCH), 0, 0, 0 }; \
    }

#define DSBOX_REQUIRE_CORE_VERSION_RANGE(                                    \
    MIN_MAJOR, MIN_MINOR, MIN_PATCH, MAX_MAJOR, MAX_MINOR, MAX_PATCH)        \
    DSBOX_PLUGIN_EXPORT void DSBox_GetRequiredCoreVersion(                   \
        ::dsbox::DSBoxVersionRequirementC* out)                             \
    {                                                                        \
        *out = ::dsbox::DSBoxVersionRequirementC{                            \
            4, (MIN_MAJOR), (MIN_MINOR), (MIN_PATCH),                        \
            (MAX_MAJOR), (MAX_MINOR), (MAX_PATCH) };                         \
    }
