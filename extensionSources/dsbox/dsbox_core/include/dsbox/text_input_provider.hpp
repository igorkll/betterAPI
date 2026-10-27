#pragma once

#include <dsbox/api.hpp>
#include <dsbox/window_provider.hpp>

#include <string>

namespace dsbox
{
    // Contract for starting/stopping IME composition for a window and
    // positioning the platform's IME candidate window. Deliberately a
    // separate interface from IWindowProvider -- not every
    // window-owning module necessarily wants to take on IME plumbing,
    // and every existing IWindowProvider consumer that never touches
    // text input is unaffected by this contract's existence.
    // Implemented by dsbox_sdl::WindowModule alongside IWindowProvider;
    // consumed by dsbox_ui, which otherwise has no dependency on
    // dsbox_sdl at all -- same decoupling pattern as
    // dsbox_vulkan/IWindowProvider.
    //
    // window_module.hpp already documents why text input isn't part of
    // WindowModule's own event surface: it "belongs to a higher-level
    // UI module that can own IME composition state". This interface
    // plus TextInputEvent/TextEditingEvent below are that seam.
    class DSBOX_API ITextInputProvider
    {
    public:
        virtual ~ITextInputProvider() = default;

        // Begins IME composition for `handle`. No-op if already
        // started for that window. Composition results arrive later as
        // "window.text_input"/"window.text_editing" events (below),
        // never as a return value here.
        virtual void startTextInput(WindowHandle handle) = 0;

        virtual void stopTextInput(WindowHandle handle) = 0;

        // Tells the platform IME where to draw its candidate window --
        // typically the caret's current screen position, in the same
        // coordinate space as MouseButtonEvent's x/y for that window.
        virtual void setTextInputRect(WindowHandle handle, int x, int y, int width, int height) = 0;
    };

    // Published as "window.text_input" -- committed text, ready to
    // insert as-is (already resolved through IME composition, or a
    // plain keypress on backends without IME).
    struct TextInputEvent
    {
        WindowHandle handle = kInvalidWindowHandle;
        std::string text;
    };

    // Published as "window.text_editing" -- IME composition preview,
    // not yet committed. `cursor`/`selectionLength` are UTF-8 byte
    // offsets into `text`. An empty `text` signals composition ended
    // without committing anything (e.g. Escape).
    struct TextEditingEvent
    {
        WindowHandle handle = kInvalidWindowHandle;
        std::string text;
        int cursor = 0;
        int selectionLength = 0;
    };
}
