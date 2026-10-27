#pragma once

#include <dsbox_ui/length.hpp>

#include <dsbox/input.hpp>
#include <dsbox/window_provider.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace dsbox_ui
{
    // Identifies an element within one WindowUiTree. See
    // UiElementIdMode (element.hpp) for what stays true about a given
    // id across frames -- that depends on the mode the element was
    // created with, NOT on this type itself.
    using UiElementId = std::uint64_t;
    constexpr UiElementId kInvalidUiElementId = 0;

    // Where a UiEvent currently is in its traversal of the tree.
    // Capture runs root -> target (lets an ancestor -- e.g. a drag
    // handle's owning container -- intercept before a descendant ever
    // sees the event); Bubble runs target -> root afterward, the usual
    // way a click "reaches" a button inside several nested containers.
    // Target is the single call where target == the element the event
    // actually hit; IUiElement::handleEvent() sees Capture and Bubble
    // calls too (for every ancestor along the path), not just Target.
    enum class UiEventPhase
    {
        Capture,
        Target,
        Bubble,
    };

    struct PointerDownEvent
    {
        float x = 0.0f;
        float y = 0.0f;
        dsbox::MouseButton button = dsbox::MouseButton::Unknown;
    };

    struct PointerUpEvent
    {
        float x = 0.0f;
        float y = 0.0f;
        dsbox::MouseButton button = dsbox::MouseButton::Unknown;
    };

    struct PointerMoveEvent
    {
        float x = 0.0f;
        float y = 0.0f;
        float deltaX = 0.0f;
        float deltaY = 0.0f;
    };

    struct PointerWheelEvent
    {
        float scrollX = 0.0f;
        float scrollY = 0.0f;
    };

    // Synthesized by WindowUiTree itself from consecutive pointer-
    // position events (PointerDownEvent/PointerMoveEvent/
    // PointerUpEvent -- see dispatchEvent()'s own updateHoverState()) --
    // never produced by a backend/raw input bus the way the events
    // above are. Mirrors HTML's non-bubbling mouseenter/mouseleave,
    // not the bubbling mouseover/mouseout: WindowUiTree dispatches
    // each of these directly to exactly the one element whose hover
    // state changed, always at UiEventPhase::Target, never walking
    // Capture/Bubble for it the way a real hit-tested event does. An
    // element that newly became part of the hovered path (the hit-
    // tested element itself, AND every ancestor on its path that
    // wasn't already hovered) gets exactly one PointerEnterEvent; an
    // element that fell OUT of the hovered path gets exactly one
    // PointerLeaveEvent. See Container::setHoverColor() (container.hpp)
    // for the one built-in element that reacts to these so far.
    struct PointerEnterEvent {};
    struct PointerLeaveEvent {};

    // Synthesized by WindowUiTree itself (see cancelPointerCapture(),
    // window_ui_tree.cpp) to end an in-progress pointer gesture WITHOUT
    // a matching PointerUpEvent ever having happened -- the answer to
    // the "v1 has no PointerCancelEvent" gap this toolkit's own design
    // notes used to call out (losing focus some other way mid-press,
    // say). Dispatched directly to whichever element currently holds
    // pointer capture (see IUiElementActions::setPointerCapture()), at
    // UiEventPhase::Target only -- never hit-tested, never bubbled,
    // same delivery model PointerEnterEvent/PointerLeaveEvent use and
    // for the same reason: there is no "where" for a cancellation, only
    // a "who". Fired automatically whenever WindowUiTree::setFocus()
    // changes focus away while a gesture is in progress (a dialog
    // stealing focus mid-press, say), and whenever
    // IUiElementActions::releasePointerCapture() is called by anything
    // OTHER than the normal auto-release-on-PointerUpEvent path inside
    // dispatchEvent() itself (which clears capture directly and fires
    // the real PointerUpEvent instead -- never this). An element that
    // was mid-press (Container being the one built-in example -- see
    // its own handleEvent()) should react to this exactly like it would
    // react to "the press didn't complete": clear its own pressed/
    // active state WITHOUT firing whatever a completed click would
    // have fired.
    struct PointerCancelEvent {};

    struct KeyDownEvent
    {
        dsbox::KeyCode key = dsbox::KeyCode::Unknown;
        dsbox::KeyModifier modifiers = dsbox::KeyModifier::None;
        bool repeat = false;
    };

    struct KeyUpEvent
    {
        dsbox::KeyCode key = dsbox::KeyCode::Unknown;
        dsbox::KeyModifier modifiers = dsbox::KeyModifier::None;
    };

    // Committed text -- mirrors what a future dsbox::ITextInputProvider
    // implementation (dsbox_sdl) reports as "window.text_input".
    struct TextInputEvent
    {
        std::string text;
    };

    // IME composition preview -- mirrors "window.text_editing".
    // `cursor`/`selectionLength` are UTF-8 byte offsets into `text`.
    struct TextEditingEvent
    {
        std::string text;
        int cursor = 0;
        int selectionLength = 0;
    };

    struct FocusInEvent {};
    struct FocusOutEvent {};

    using UiEventPayload = std::variant<
        PointerDownEvent, PointerUpEvent, PointerMoveEvent, PointerWheelEvent,
        PointerEnterEvent, PointerLeaveEvent, PointerCancelEvent,
        KeyDownEvent, KeyUpEvent,
        TextInputEvent, TextEditingEvent,
        FocusInEvent, FocusOutEvent
    >;

    // A single UI-level event, already translated from raw
    // dsbox::Input/dsbox::ITextInputProvider terms into tree terms
    // (hit-tested target, capture/bubble phase). `handle` identifies
    // which window's tree produced this event -- present even when a
    // subscriber already picked the per-window topic below, since a
    // handler taken off that topic still needs to know it if it's
    // shared across several WindowUiTree instances.
    struct UiEvent
    {
        dsbox::WindowHandle handle = dsbox::kInvalidWindowHandle;
        UiElementId target = kInvalidUiElementId;
        UiEventPhase phase = UiEventPhase::Target;

        // Set by a handler (via IUiElement::handleEvent()'s return
        // value; see element.hpp) to stop the walk from continuing
        // further along its current phase. WindowUiTree owns actually
        // honoring this -- IUiElement itself never touches this field.
        bool stopPropagation = false;

        UiEventPayload payload;
    };

    // Bus topic for subscribers that only care about one window's UI
    // events -- registering here means never being handed (and never
    // paying to filter out) another window's traffic. WindowUiTree
    // emits every UiEvent it produces under this topic for its own
    // handle.
    inline std::string uiEventTopic(dsbox::WindowHandle handle)
    {
        return "ui.window." + std::to_string(handle) + ".event";
    }

    // Bus topic that receives the SAME UiEvent as every window's own
    // uiEventTopic(), for subscribers that want all windows at once
    // (a global debug overlay, a single application-level router).
    // WindowUiTree emits each event under both topics; `handle` inside
    // UiEvent remains the way to tell them apart once received here.
    inline constexpr std::string_view kUiEventTopicAny = "ui.event";
}
