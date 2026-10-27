#pragma once

#if defined(_WIN32) || defined(_WIN64)
    #if defined(DSBOX_MOD_RUNTIME_EXPORTING)
        #define DSBOX_MOD_RUNTIME_API __declspec(dllexport)
    #else
        #define DSBOX_MOD_RUNTIME_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) && __GNUC__ >= 4
    #define DSBOX_MOD_RUNTIME_API __attribute__((visibility("default")))
#else
    #define DSBOX_MOD_RUNTIME_API
#endif

#if defined(_WIN32) || defined(_WIN64)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

#ifdef __cplusplus
#if defined(_WIN32) || defined(_WIN64)
namespace dsbox
{
    class Context;
}

extern "C"
{
    // Returns the active runtime context, or nullptr before initialization
    // and once shutdown has begun. The pointer is invalid after shutdown.
    DSBOX_MOD_RUNTIME_API dsbox::Context* __cdecl GetDsboxContext();

    // Start the runtime once. Safe to call more than once; concurrent calls
    // wait for the first initialization to finish.
    DSBOX_MOD_RUNTIME_API BOOL WINAPI DSBoxRuntimeInitialize();

    // Stop modules and release runtime state. Call outside DllMain, after
    // initialization returns, and before unloading the runtime DLL.
    DSBOX_MOD_RUNTIME_API void WINAPI DSBoxRuntimeShutdown();
}
#endif
#endif
