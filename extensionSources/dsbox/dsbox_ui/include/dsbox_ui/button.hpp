#pragma once

#include <dsbox_ui/api.hpp>
#include <dsbox_ui/container.hpp>
#include <dsbox_ui/text.hpp>

#include <memory>
#include <string>

namespace dsbox_ui
{
    // What makeButton() (below) hands back: the assembled root plus a
    // raw pointer to the label it built, so a caller can still reach
    // Text's own setters (setColor(), etc.) directly. `label` is owned
    // by `root` (its one and only child) -- valid for exactly as long
    // as `root`, or wherever `root` ends up (e.g. moved into a
    // Container::addChild() call), stays alive.
    struct ButtonParts
    {
        std::unique_ptr<Container> root;
        Text* label = nullptr;
    };

    // Convenience factory, not a new IUiElement type: assembles the
    // exact Container+Text pattern Container's own "-- Interactivity --"
    // section (container.hpp) already documents as how a clickable/
    // focusable element is built -- a "Button" in dsbox_ui is simply
    // that pattern, pre-wired with sensible defaults (padding, a
    // vertically- and horizontally-centered label via StackLayout's
    // CrossAlign::Center, a pressed-color swap) instead of every call
    // site assembling it by hand, the way dsbox_ui_demo's own main.cpp
    // did for cardA/cardB before this existed. The returned root is
    // already focusable with a StackLayout set; attach behavior with
    // Container::setOnClick() same as any other Container.
    //
    // `containerId`/`labelId` follow the same caller-assigns-ids model
    // every other dsbox_ui element uses (see Container's own "v1
    // identity limitation") -- both must be unique within whatever
    // tree this ends up part of.
    //
    // v1 sizing limitation: the returned root's width defaults to a
    // fixed 120px (see Container's own "v1 sizing limitation" --
    // there is still no content-fit/"hug the label" sizing anywhere in
    // dsbox_ui). Call setPreferredWidth() on the returned root
    // afterward for a different width; setPreferredHeight() likewise
    // (defaults to 40px).
    DSBOX_UI_API ButtonParts makeButton(
        UiElementId containerId, UiElementId labelId, std::string label,
        UiElementIdMode idMode = UiElementIdMode::Instance);
}
