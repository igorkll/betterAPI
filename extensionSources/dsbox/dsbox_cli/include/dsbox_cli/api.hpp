#pragma once

#if defined(_WIN32) || defined(_WIN64)
    #if defined(DSBOX_CLI_EXPORTING)
        #define DSBOX_CLI_API __declspec(dllexport)
    #else
        #define DSBOX_CLI_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) && __GNUC__ >= 4
    #define DSBOX_CLI_API __attribute__((visibility("default")))
#else
    #define DSBOX_CLI_API
#endif
