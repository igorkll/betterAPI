# DSBox runtime DLL

This source builds `DSBoxModRuntime.dll`, which the standalone
`DSBoxModLoader.exe` loads into the game process. The loader calls the exported
`DSBoxRuntimeInitialize` after `LoadLibraryW` returns; the export starts a
worker thread and waits until configuration and the configured DSBox modules
have finished loading before it returns.

The worker creates the DSBox `Context` and `ModuleManager`, resolves the
DSBox output root (the directory containing `bin/`, `config/`, and `shaders/`)
from the runtime DLL's location, reads `<root>/config/modules.ini`, and loads
the configured `IModule` plugins from `<root>/modules/`.

The host can call the exported `DSBoxRuntimeShutdown()` after initialization
has returned and before unloading the DLL. It stops the modules, joins the
runtime worker, and releases the manager, context, and log. Call it outside
`DllMain`, and ensure no host code is still using the pointer returned by
`GetDsboxContext()`.
