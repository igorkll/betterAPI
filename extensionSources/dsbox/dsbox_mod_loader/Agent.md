# DSBox mod loader

This source builds `DSBoxModLoader.exe`, the Steam launch shim. The loader
starts the game suspended, loads `bin/framework/mod/DSBoxModRuntime.dll`, and calls its
`DSBoxRuntimeInitialize` export after `LoadLibraryW` returns.

Before loading the runtime, the launcher adds `bin/framework/core/` to the
target process's DLL search path so DSBoxCore, DSBoxConfig, and DSBoxIni
resolve from their shared framework directory. The loader EXE is emitted to the `fin-out/`
root, outside `bin/executables/`. If loading or initialization fails, it logs
the reason and resumes the game unmodified.

The launcher uses documented Win32 process APIs only. Known anti-cheat
processes cause it to skip loading and start the game unmodified.
