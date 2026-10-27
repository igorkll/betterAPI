#include <dsbox/text_input_provider.hpp>

// Deliberately just the include above, nothing else -- same reasoning
// as window_provider.cpp (see that file's own comment in full).
// ITextInputProvider is a pure-virtual interface with only an implicit
// default constructor and a defaulted virtual destructor; without SOME
// translation unit inside DSBoxCore's own build including this header,
// MSVC never emits (and therefore never exports)
// ITextInputProvider::ITextInputProvider()/~ITextInputProvider() from
// DSBoxCore.dll, and any downstream module implementing the interface
// (dsbox_sdl::WindowModule) fails to link with LNK2019 on those two
// symbols specifically. See text_input_provider.hpp for the interface
// itself.
