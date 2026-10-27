#pragma once

// For symbols a plugin exports to be found by GetProcAddress/dlsym at
// runtime -- DSBox_CreateModule, DSBox_DestroyModule,
// DSBox_GetRequiredCoreVersion, and any other extern "C" plugin ABI
// entry point. Unlike dsbox::DSBOX_API (which switches between
// dllexport and dllimport depending on whether YOU are building
// DSBoxCore itself), a plugin is never linked against by an import
// library -- the host finds these symbols purely by name at runtime --
// so there is no "importing" side and the export must always be
// dllexport, regardless of what the plugin's own build defines.
//
// Using dsbox::DSBOX_API for a plugin's own exports is a common mistake:
// since the plugin's build never defines DSBOX_EXPORTING (that flag is
// private to DSBoxCore's own build), DSBOX_API would resolve to
// dllimport in the plugin's translation unit -- which does not properly
// export the symbol from the plugin's own DLL on MSVC.
//
//   #include <dsbox/plugin_export.hpp>
//
//   DSBOX_PLUGIN_EXPORT dsbox::IModule* DSBox_CreateModule()
//   {
//       return new MyModule();
//   }
#if defined(_WIN32) || defined(_WIN64)
    #define DSBOX_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
    #define DSBOX_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#else
    #define DSBOX_PLUGIN_EXPORT extern "C"
#endif
