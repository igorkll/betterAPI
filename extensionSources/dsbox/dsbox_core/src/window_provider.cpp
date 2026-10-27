#include <dsbox/window_provider.hpp>

// Deliberately just the include above, nothing else. IWindowProvider is
// a pure-virtual interface with only an implicit default constructor
// and a defaulted virtual destructor -- there's no actual code to write
// here.
//
// Why this TU needs to exist at all: on MSVC, a DSBOX_API (dllexport)
// class only gets its implicit special members (default constructor,
// defaulted destructor) actually emitted -- and therefore exported --
// from SOME translation unit inside DSBoxCore's own build. Nothing else
// in dsbox_core happened to include <dsbox/window_provider.hpp> (unlike
// <dsbox/module.hpp>, which module_manager.cpp/module_loader.cpp/
// static_registry.cpp all pull in for unrelated reasons), so without
// this file, DSBoxCore.dll never exports
// IWindowProvider::IWindowProvider()/~IWindowProvider(), and any
// downstream module implementing the interface (e.g.
// dsbox_sdl::WindowModule) fails to link with LNK2019 on those two
// symbols specifically. See window_provider.hpp for the interface
// itself.
