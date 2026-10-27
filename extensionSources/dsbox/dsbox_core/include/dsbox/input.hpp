#pragma once

#include <dsbox/api.hpp>
#include <dsbox/communication_manager.hpp>
#include <dsbox/window_provider.hpp>

#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace dsbox
{
    // Own, backend-independent key vocabulary -- deliberately not a
    // literal SDL_Scancode/SDL_Keycode or any other backend's native
    // type: a core header should not force every consumer to include a
    // specific windowing library's headers, and a future non-SDL
    // backend (or a synthetic/test one) should be able to produce these
    // values without linking against SDL at all.
    //
    // BUT: the numeric value of every entry below is pinned to SDL's own
    // scancode number, which is itself pinned to the USB HID keyboard
    // usage page (a public, versioned, external standard -- see
    // https://usb.org/sites/default/files/hut1_5.pdf). We didn't invent
    // a parallel numbering; we gave SDL's already-stable numbers our
    // own type, since SDL is this project's reference/current backend
    // (see dsbox_sdl). This is what makes the list "finished": a
    // backend that already speaks SDL scancodes can translate with a
    // straight cast, not a hand-maintained table, so
    //   1. every scancode SDL knows about today is already valid
    //      KeyCode, named or not -- an event for a key we haven't gotten
    //      around to naming yet still carries the right numeric value,
    //      it's just spelled with a cast instead of an enumerator;
    //   2. naming one later only ever ADDS an enumerator at its
    //      already-reserved value -- it never renumbers an existing
    //      one, so it can never be a binary-breaking change for code
    //      that was already built against this header.
    // The one thing this scheme depends on is SDL itself never
    // reassigning a scancode's number -- which SDL guarantees as part
    // of its own ABI stability promise, so we're borrowing a guarantee
    // that already has to hold for SDL's own users. A backend not based
    // on SDL is free to produce these same values by whatever means it
    // likes; nothing here requires SDL at runtime.
    //
    // Don't loop over KeyCode; switch on the names you care about and
    // let everything else fall through to a default case (deliberately
    // no upper sentinel -- see the same rationale below, at the end of
    // the enum).
    enum class KeyCode : std::uint32_t
    {
        Unknown = 0,

        // -- USB HID usage page 0x07 (keyboard page) --
        A = 4, B = 5, C = 6, D = 7, E = 8, F = 9, G = 10, H = 11, I = 12,
        J = 13, K = 14, L = 15, M = 16, N = 17, O = 18, P = 19, Q = 20,
        R = 21, S = 22, T = 23, U = 24, V = 25, W = 26, X = 27, Y = 28,
        Z = 29,

        Num1 = 30, Num2 = 31, Num3 = 32, Num4 = 33, Num5 = 34,
        Num6 = 35, Num7 = 36, Num8 = 37, Num9 = 38, Num0 = 39,

        Enter = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44,

        Minus = 45, Equals = 46, LeftBracket = 47, RightBracket = 48,
        Backslash = 49, NonUsHash = 50, Semicolon = 51, Apostrophe = 52,
        Grave = 53, Comma = 54, Period = 55, Slash = 56,

        CapsLock = 57,

        F1 = 58, F2 = 59, F3 = 60, F4 = 61, F5 = 62, F6 = 63,
        F7 = 64, F8 = 65, F9 = 66, F10 = 67, F11 = 68, F12 = 69,

        PrintScreen = 70, ScrollLock = 71, Pause = 72, Insert = 73,
        Home = 74, PageUp = 75, Delete = 76, End = 77, PageDown = 78,
        Right = 79, Left = 80, Down = 81, Up = 82,

        NumLockKey = 83,
        KeypadDivide = 84, KeypadMultiply = 85, KeypadMinus = 86,
        KeypadPlus = 87, KeypadEnter = 88,
        Keypad1 = 89, Keypad2 = 90, Keypad3 = 91, Keypad4 = 92,
        Keypad5 = 93, Keypad6 = 94, Keypad7 = 95, Keypad8 = 96,
        Keypad9 = 97, Keypad0 = 98, KeypadPeriod = 99,

        NonUsBackslash = 100,
        Application = 101, // Windows contextual menu key / Mac "compose"
        Power = 102,
        KeypadEquals = 103,

        F13 = 104, F14 = 105, F15 = 106, F16 = 107, F17 = 108, F18 = 109,
        F19 = 110, F20 = 111, F21 = 112, F22 = 113, F23 = 114, F24 = 115,

        Execute = 116, Help = 117, Menu = 118, Select = 119, Stop = 120,
        Again = 121, Undo = 122, Cut = 123, Copy = 124, Paste = 125,
        Find = 126, Mute = 127, VolumeUp = 128, VolumeDown = 129,

        // 130-132 (locking caps/num/scroll lock) are reserved by SDL but
        // not currently emitted by any backend -- deliberately omitted.

        KeypadComma = 133, KeypadEqualsAs400 = 134,

        // Asian-keyboard keys (IME toggles, Yen, Hangul/Hanja/Katakana...)
        International1 = 135, International2 = 136, International3 = 137,
        International4 = 138, International5 = 139, International6 = 140,
        International7 = 141, International8 = 142, International9 = 143,
        Lang1 = 144, Lang2 = 145, Lang3 = 146, Lang4 = 147, Lang5 = 148,
        Lang6 = 149, Lang7 = 150, Lang8 = 151, Lang9 = 152,

        AltErase = 153, SysReq = 154, Cancel = 155, Clear = 156,
        Prior = 157, Return2 = 158, Separator = 159, Out = 160,
        Oper = 161, ClearAgain = 162, CrSel = 163, ExSel = 164,

        // 165-175 reserved/unused by SDL.

        Keypad00 = 176, Keypad000 = 177,
        ThousandsSeparator = 178, DecimalSeparator = 179,
        CurrencyUnit = 180, CurrencySubunit = 181,
        KeypadLeftParen = 182, KeypadRightParen = 183,
        KeypadLeftBrace = 184, KeypadRightBrace = 185,
        KeypadTab = 186, KeypadBackspace = 187,
        KeypadA = 188, KeypadB = 189, KeypadC = 190,
        KeypadD = 191, KeypadE = 192, KeypadF = 193,
        KeypadXor = 194, KeypadPower = 195, KeypadPercent = 196,
        KeypadLess = 197, KeypadGreater = 198,
        KeypadAmpersand = 199, KeypadDblAmpersand = 200,
        KeypadVerticalBar = 201, KeypadDblVerticalBar = 202,
        KeypadColon = 203, KeypadHash = 204, KeypadSpace = 205,
        KeypadAt = 206, KeypadExclam = 207,
        KeypadMemStore = 208, KeypadMemRecall = 209, KeypadMemClear = 210,
        KeypadMemAdd = 211, KeypadMemSubtract = 212,
        KeypadMemMultiply = 213, KeypadMemDivide = 214,
        KeypadPlusMinus = 215, KeypadClear = 216, KeypadClearEntry = 217,
        KeypadBinary = 218, KeypadOctal = 219, KeypadDecimal = 220,
        KeypadHexadecimal = 221,

        // 222-223 reserved/unused by SDL.

        LeftControl = 224, LeftShift = 225, LeftAlt = 226, LeftSuper = 227,
        RightControl = 228, RightShift = 229, RightAlt = 230,
        RightSuper = 231,

        // 232-256 reserved/unused by SDL.

        Mode = 257,

        // -- USB HID usage page 0x0C (consumer page), the subset SDL
        // -- currently maps -- and SDL's own "mobile keys" additions.
        Sleep = 258, Wake = 259,
        ChannelIncrement = 260, ChannelDecrement = 261,
        MediaPlay = 262, MediaPause = 263, MediaRecord = 264,
        MediaFastForward = 265, MediaRewind = 266,
        MediaNextTrack = 267, MediaPreviousTrack = 268, MediaStop = 269,
        MediaEject = 270, MediaPlayPause = 271, MediaSelect = 272,

        AcNew = 273, AcOpen = 274, AcClose = 275, AcExit = 276,
        AcSave = 277, AcPrint = 278, AcProperties = 279,
        AcSearch = 280, AcHome = 281, AcBack = 282, AcForward = 283,
        AcStop = 284, AcRefresh = 285, AcBookmarks = 286,

        SoftLeft = 287, SoftRight = 288, Call = 289, EndCall = 290,

        // Deliberately no upper sentinel (no "Count"/"Max" member): a
        // fixed upper bound is exactly the kind of thing that would
        // need editing -- and everyone iterating over it recompiling --
        // every time SDL reserves a few more numbers.
    };

    // Bitmask of modifier keys held at the time of a KeyEvent. Combine
    // with operator| and test with hasModifier().
    enum class KeyModifier : std::uint32_t
    {
        None = 0,
        Shift = 1u << 0,
        Control = 1u << 1,
        Alt = 1u << 2,
        Super = 1u << 3,
        CapsLock = 1u << 4,
        NumLock = 1u << 5,
    };

    constexpr KeyModifier operator|(KeyModifier lhs, KeyModifier rhs) noexcept
    {
        return static_cast<KeyModifier>(
            static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
    }

    constexpr KeyModifier operator&(KeyModifier lhs, KeyModifier rhs) noexcept
    {
        return static_cast<KeyModifier>(
            static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
    }

    constexpr bool hasModifier(KeyModifier mods, KeyModifier flag) noexcept
    {
        return (mods & flag) != KeyModifier::None;
    }

    enum class MouseButton : std::uint32_t
    {
        // Same rationale and same guarantee as KeyCode: these values are
        // pinned to SDL's own SDL_BUTTON_* numbers, not invented here.
        Unknown = 0,
        Left = 1,
        Middle = 2,
        Right = 3,
        X1 = 4,
        X2 = 5,
    };

    // Published on the bus by Input::setKeyState() as "window.key" (see
    // Input below) whenever that instance's InputOptions::dispatchEvents
    // is true. `handle` identifies which window the key event belongs
    // to -- a subscriber typically listens once and switches on it,
    // rather than needing a separate subscription per window/Input.
    struct KeyEvent
    {
        WindowHandle handle = kInvalidWindowHandle;
        KeyCode key = KeyCode::Unknown;
        KeyModifier modifiers = KeyModifier::None;
        bool pressed = false; // true = key down, false = key up
        bool repeat = false;  // true if this is an OS autorepeat of a held key
    };

    // Published as "window.mouse_button".
    struct MouseButtonEvent
    {
        WindowHandle handle = kInvalidWindowHandle;
        MouseButton button = MouseButton::Unknown;
        bool pressed = false;
        int x = 0;
        int y = 0;
    };

    // Published as "window.mouse_motion".
    struct MouseMotionEvent
    {
        WindowHandle handle = kInvalidWindowHandle;
        int x = 0;
        int y = 0;
        int deltaX = 0;
        int deltaY = 0;
    };

    // Published as "window.mouse_wheel". Wheel motion is delta-only by
    // nature (there's no "wheel position" to track), so unlike keys and
    // buttons it is never reflected into queryable state -- events only.
    struct MouseWheelEvent
    {
        WindowHandle handle = kInvalidWindowHandle;
        float scrollX = 0.0f;
        float scrollY = 0.0f;
    };

    // Configuration for a single Input instance, fixed for its whole
    // lifetime (no setters -- construct a new instance if you need
    // different behavior).
    struct InputOptions
    {
        // If true, setKeyState()/setMouseButtonState()/
        // setMousePosition()/setMouseWheel() publish the corresponding
        // *Event on the bus as they're called. Default true.
        bool dispatchEvents = true;

        // If true, this instance also maintains queryable state
        // (isKeyDown()/isMouseButtonDown()/mousePosition()), independent
        // of dispatchEvents -- either, both, or neither can be enabled.
        // Default false: most consumers that only react to events don't
        // need the extra bookkeeping.
        bool trackState = false;
    };

    // Owns the input state -- and is the single point of entry for
    // reporting input -- for exactly one window. One instance per
    // window (typically created and owned by whatever module owns that
    // window, e.g. dsbox_sdl::WindowModule, alongside the window
    // itself), not a singleton and not backend-specific: nothing here
    // knows or cares whether the calls below originated from a real SDL
    // event, a different windowing backend, or another module
    // synthesizing input for testing/automation/replay purposes. Same
    // call, same effect, either way -- that's the entire point of
    // pulling this out of dsbox_sdl: a synthetic setKeyState() call is
    // indistinguishable, to every consumer of "window.key" or
    // isKeyDown(), from a real keypress.
    //
    // Thread safety: every method is safe to call concurrently from any
    // thread (own internal mutex guards the tracked state; emitting on
    // CommunicationManager is itself thread-safe -- see
    // CommunicationManager::emit()). In particular, nothing stops one
    // thread reporting real input while another concurrently injects
    // synthetic input for the same instance; the two are simply
    // interleaved.
    class DSBOX_API Input
    {
    public:
        // `bus` is retained by reference -- must outlive this Input
        // (in practice, the Context it belongs to, which every module
        // that could own an Input already outlives). `handle` is
        // stamped onto every event this instance publishes and is
        // immutable for the instance's lifetime.
        Input(CommunicationManager& bus, WindowHandle handle, InputOptions options = {});

        // Declared (not left implicit) and defined out-of-line in
        // input.cpp so DSBoxCore.dll actually emits and exports it on
        // MSVC -- see window_provider.cpp's own comment for why an
        // implicit special member of a DSBOX_API class can otherwise
        // never get exported at all if nothing inside dsbox_core's own
        // build happens to odr-use it.
        ~Input();

        Input(const Input&) = delete;
        Input& operator=(const Input&) = delete;

        WindowHandle handle() const noexcept
        {
            return m_handle;
        }

        const InputOptions& options() const noexcept
        {
            return m_options;
        }

        // The following four are how input is REPORTED to this
        // instance -- called by whatever owns the real device (e.g.
        // dsbox_sdl's control thread translating an SDL_Event) or by
        // any other module emulating input for this window. Each
        // updates tracked state (if trackState) and/or emits the
        // corresponding event (if dispatchEvents), per this instance's
        // InputOptions.
        void setKeyState(KeyCode key, bool pressed, bool repeat = false, KeyModifier modifiers = KeyModifier::None);
        void setMouseButtonState(MouseButton button, bool pressed, int x, int y);
        void setMousePosition(int x, int y, int deltaX = 0, int deltaY = 0);
        void setMouseWheel(float scrollX, float scrollY);

        // The following three are how input is QUERIED. All three
        // return false (leaving out-parameters, if any, untouched) if
        // this instance's InputOptions::trackState is false -- callers
        // relying on these should construct with trackState = true.
        bool isKeyDown(KeyCode key) const;
        bool isMouseButtonDown(MouseButton button) const;
        bool mousePosition(int& outX, int& outY) const;

    private:
        CommunicationManager& m_bus;
        WindowHandle m_handle;
        InputOptions m_options;

        mutable std::mutex m_mutex;
        std::unordered_set<KeyCode> m_pressedKeys;
        std::unordered_set<MouseButton> m_pressedButtons;
        int m_mouseX = 0;
        int m_mouseY = 0;
    };
}
