#pragma once

#if defined(_WIN32) || defined(_WIN64)
    #if defined(BETTER_DSBOX_EXPORTING)
        #define BETTER_DSBOX_API __declspec(dllexport)
    #else
        #define BETTER_DSBOX_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) && __GNUC__ >= 4
    #define BETTER_DSBOX_API __attribute__((visibility("default")))
#else
    #define BETTER_DSBOX_API
#endif

#if defined(_WIN32) || defined(_WIN64)
    #define BETTER_DSBOX_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
    #define BETTER_DSBOX_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
    #define BETTER_DSBOX_PLUGIN_EXPORT
#endif
