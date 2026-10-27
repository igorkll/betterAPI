#pragma once

#include <dsbox_ui/api.hpp>
#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/element.hpp>
#include <dsbox_ui/event.hpp>
#include <dsbox_ui/length.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <dsbox/context.hpp>
#include <dsbox/module.hpp>
#include <dsbox/text_input_provider.hpp>
#include <dsbox/window_provider.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dsbox_ui
{
    // Owns exactly one window's UI: its element tree, focus/hover
    // state, and the DisplayList produced from the last relayout+paint
    // pass. One instance per dsbox::WindowHandle, created and owned by
    // UiModule -- never constructed directly by application code.
    //
    // Threading: every method below except currentDisplayList() is
    // meant to be called from the control thread -- the same thread
    // dsbox_sdl::WindowModule already calls back on for
    // "window.key"/"window.mouse_button"/etc (see input.hpp). That is
    // deliberate: WindowUiTree's own event handling, dirty-tracking and
    // relayout all run there, so none of it ever blocks the thread
    // pumping the window's native event queue for longer than one
    // event's own dispatch takes.
    //
    // currentDisplayList() is the one method safe to call from any
    // other thread (in particular, the Vulkan/presentation thread
    // driving dsbox_vulkan_gui): it returns the latest committed
    // snapshot via a non-blocking atomic load, never the tree being
    // actively mutated. The control thread publishes a new snapshot
    // (an atomic store, not a lock) each time a pass actually changed
    // something -- so the two threads only ever communicate through
    // that single pointer, never wait on each other, and the
    // presentation thread simply keeps presenting the previous
    // snapshot on any frame where nothing new has been committed yet.
    //
    // WindowUiTree also implements IUiElementActions (element.hpp),
    // passing itself into every handleEvent() call it makes -- so an
    // element reacting to, say, a click can call back into setFocus()/
    // markDirty() from inside that very call. m_treeMutex is a
    // std::recursive_mutex specifically to make that safe: dispatchEvent()/
    // setFocus() already hold it for the duration of the handleEvent()
    // call that might turn around and call requestFocus()/markDirty()
    // right back into this same object, on the same thread -- a plain
    // std::mutex would deadlock on exactly that call.
    class DSBOX_UI_API WindowUiTree final : public IUiElementActions
    {
    public:
        // `textInput`, if non-null, is used for startTextInput()/
        // stopTextInput()/setTextInputRect() calls as focus moves
        // in/out of an ITextInputTarget-implementing element (see
        // element.hpp) -- pass nullptr on a backend with no
        // dsbox::ITextInputProvider (focus still works, this tree
        // simply never asks for IME composition on that window's
        // behalf). Not retained past this constructor except as the
        // pointer itself -- ownership stays with whoever passed it in
        // (typically dsbox_sdl::WindowModule, which outlives every
        // WindowUiTree built against it).
        WindowUiTree(dsbox::WindowHandle handle, std::unique_ptr<IUiElement> root,
                     dsbox::ITextInputProvider* textInput = nullptr);
        ~WindowUiTree();

        WindowUiTree(const WindowUiTree&) = delete;
        WindowUiTree& operator=(const WindowUiTree&) = delete;

        dsbox::WindowHandle handle() const noexcept
        {
            return m_handle;
        }

        IUiElement& root() noexcept
        {
            return *m_root;
        }

        // Requires a *stable* id -- see UiElementIdMode. Asserts (debug
        // builds) if `target`'s owning element reports
        // UiElementIdMode::Index, since a position-based id is not
        // safe to hold onto past the current pass: by the time this
        // tree next dispatches an event, the same value could already
        // mean a different element.
        void setFocus(UiElementId target);
        UiElementId focusedElement() const noexcept
        {
            return m_focusedElement;
        }

        // Marks `target` dirty for the next tick() -- see
        // UiDirtyLevel's own doc (element.hpp) for exactly what `level`
        // changes. WindowUiTree does not walk the tree looking for
        // changes on its own -- an element or whatever's driving it
        // calls this explicitly. A UiDirtyLevel::Layout call also
        // records `target` itself (see m_dirtyElements) for tick()'s
        // own per-subtree scoping -- see IUiElement::
        // prepareIncrementalRelayout() (element.hpp) for how that id is
        // actually used; Repaint still repaints the whole tree's
        // DisplayList regardless of `target` (there is no cheaper unit
        // than "the whole display list" to publish a new snapshot at).
        void markDirty(UiElementId target, UiDirtyLevel level = UiDirtyLevel::Layout) override;

        // -- IUiElementActions --

        // Thin forwarding wrapper -- see setFocus() above for the
        // actual contract. Named differently here only because
        // IUiElementActions's own vocabulary ("an element requesting
        // something of its tree") reads more naturally as
        // requestFocus() than as this class's own setFocus(), which
        // predates IUiElementActions and is kept as the public name
        // external callers (tests, application code) already use.
        void requestFocus(UiElementId id) override { setFocus(id); }

        // See IUiElementActions::setPointerCapture()/
        // releasePointerCapture() for the contract; see
        // dispatchEvent() for how capture actually changes routing.
        void setPointerCapture(UiElementId id) override;
        void releasePointerCapture(UiElementId id) override;

        // Runs a hit-test + capture/target/bubble dispatch for one raw
        // input event, translating it into the corresponding
        // UiEventPayload alternative first (see event.hpp). This is
        // what UiModule calls from its own "window.key"/
        // "window.mouse_button"/etc subscriptions for the window this
        // tree belongs to.
        //
        // Returns the event as actually delivered: `target` resolved to
        // whatever the hit-test (or focus, for keyboard/text events)
        // found, and `stopPropagation`/`phase` reflecting where
        // dispatch stopped. UiModule re-publishes this returned copy
        // (not the caller's original `event`) on the ui.window.*/
        // ui.event bus topics, so subscribers there see the resolved
        // target rather than kInvalidUiElementId.
        UiEvent dispatchEvent(const UiEvent& event);

        // Re-paints (always, if anything is dirty at all) and
        // conditionally re-runs measure()/arrange() first -- skipped
        // entirely when every markDirty() call since the last tick()
        // was UiDirtyLevel::Repaint (see that enum's own doc,
        // element.hpp): a pressed/hover color change, say, genuinely
        // cannot move or resize anything, so there is nothing for
        // measure()/arrange() to do that paint() doesn't already
        // reflect from the PREVIOUS pass's arrange() results.
        //
        // When a Layout-level measure()/arrange() pass does need to
        // run, it is scoped to whatever actually changed wherever
        // possible: m_dirtyElements (accumulated by markDirty() since
        // the last pass) is handed to IUiElement::
        // prepareIncrementalRelayout() (element.hpp) first, which lets
        // an element that implements real caching (Container being the
        // one built-in example -- see its own override, container.cpp)
        // skip recomputing a subtree nothing in it was ever markDirty()'d
        // against. This is NOT full dependency-aware incremental layout
        // -- any element without its own override (every leaf type
        // that has no children; see prepareIncrementalRelayout()'s own
        // default, element.hpp -- and any CONTAINER-ish type, such as
        // ScrollView/ListView/Dropdown, that has children but hasn't
        // opted in) simply reports "always dirty", so its own ancestor
        // chain never gets to skip anything either; only a tree built
        // mostly out of Container (the composition element this
        // toolkit's own design leans on, see Agent.md) benefits in
        // practice. A window resize bypasses this scoping entirely
        // (IUiElement::forceFullRelayout(), element.hpp) and forces a
        // full pass, since a resize can change ANY element's effective
        // constraints (a Percent-sized child, say) even when nothing
        // about it was ever specifically markDirty()'d. Publishes the
        // result as a new snapshot for currentDisplayList() to see
        // either way.
        //
        // Also treats a `windowSize` different from the LAST call's as
        // Layout-dirty on its own, even if nothing else called
        // markDirty() -- this is what makes a resize actually reflow
        // the tree, rather than relying on every caller remembering to
        // markDirty() the root whenever the window changes size. A
        // no-op only if neither markDirty() was called nor
        // `windowSize` changed since the last tick(). `windowSize` is
        // this window's current client area, forming the root
        // UiConstraints for the pass.
        //
        // v1 does not diff the new DisplayList against the previous
        // one before publishing -- any dirty tick() commits a new
        // snapshot even if painting happened to produce byte-identical
        // output. Skipping a genuinely unchanged frame is a possible
        // follow-up, not a change this method's callers need to care
        // about either way.
        void tick(IUiRenderBackend& backend, UiSize windowSize);

        // Safe to call from any thread (see class comment). May return
        // nullptr if tick() has never produced a first snapshot yet.
        std::shared_ptr<const DisplayList> currentDisplayList() const;

    private:
        dsbox::WindowHandle m_handle;
        std::unique_ptr<IUiElement> m_root;
        dsbox::ITextInputProvider* m_textInput = nullptr;
        bool m_textInputActive = false; // true between a startTextInput() and the matching stopTextInput() -- see setFocus()

        std::recursive_mutex m_treeMutex; // recursive -- see this class's own comment above for why
        UiElementId m_focusedElement = kInvalidUiElementId;

        // kInvalidUiElementId means no pointer capture is active. See
        // IUiElementActions::setPointerCapture() and dispatchEvent().
        UiElementId m_pointerCaptureTarget = kInvalidUiElementId;
        bool m_dirty = true; // starts dirty so the very first tick() always lays out once
        bool m_needsRelayout = true; // see tick()'s own "two-tier dirty" comment -- starts true for the same reason m_dirty does
        UiSize m_lastWindowSize{}; // {0,0} sentinel deliberately differs from any real window size, so the first tick() also counts as a "size changed"

        // Root-to-deepest ids of whatever's currently hovered, as of
        // the last pointer-position event this tree dispatched -- see
        // updateHoverState()'s own comment (window_ui_tree.cpp) for
        // exactly how/when this changes. Empty means nothing is
        // hovered (including: no pointer event has been dispatched
        // yet).
        std::vector<UiElementId> m_hoveredPath;

        // Every id markDirty()'d at UiDirtyLevel::Layout since the
        // last tick() that actually committed a relayout pass --
        // consumed (and cleared) by tick() itself, handed to
        // IUiElement::prepareIncrementalRelayout() (element.hpp) to
        // scope that pass to whatever actually changed. See tick()'s
        // own declaration for the full per-subtree picture.
        std::unordered_set<UiElementId> m_dirtyElements;

        // Ends whatever gesture `m_pointerCaptureTarget` is currently
        // mid-way through, if any, by dispatching a PointerCancelEvent
        // (event.hpp) to the capturing element and then clearing
        // capture -- a no-op if nothing currently holds capture. Caller
        // must already hold m_treeMutex. Called from
        // releasePointerCapture() (an explicit, non-auto-release end to
        // a gesture -- see that event's own doc for why this is the
        // right moment to fire it) and from setFocus() (a focus change
        // is as good a reason as any for an in-progress gesture to not
        // complete normally).
        void cancelPointerCapture();

        // Diffs `newPath` (this event's own hit-tested or capture-
        // routed path -- see dispatchEvent()'s own pointer-payload
        // branch) against m_hoveredPath, dispatches exactly one
        // PointerLeaveEvent to each element that fell out of the
        // hovered chain and exactly one PointerEnterEvent to each
        // newly-hovered one, and updates m_hoveredPath to match. Called
        // from dispatchEvent() for every pointer-position event
        // (PointerDownEvent/PointerMoveEvent/PointerUpEvent), BEFORE
        // that event's own Capture/Target/Bubble walk -- see event.hpp's
        // own PointerEnterEvent/PointerLeaveEvent doc for why these are
        // synthesized rather than backend-sourced, and why they never
        // bubble.
        void updateHoverState(const std::vector<IUiElement*>& newPath);

        std::atomic<std::shared_ptr<const DisplayList>> m_currentList;
    };

    // The dsbox_ui module itself. Owns one WindowUiTree per window that
    // has been given one (createTree()), subscribes once on the bus for
    // every window's "window.key"/"window.mouse_button"/
    // "window.mouse_motion"/"window.mouse_wheel"/"window.text_input"/
    // "window.text_editing" events (see input.hpp and
    // text_input_provider.hpp), and routes each to the right tree by
    // its WindowHandle -- rather than one subscription per window,
    // mirroring how dsbox_sdl itself publishes those events.
    class DSBOX_UI_API UiModule final : public dsbox::IModule
    {
    public:
        std::string_view name() const override
        {
            return "dsbox.ui";
        }

        // No entries here: dsbox_core (Input, IWindowProvider,
        // ITextInputProvider, CommunicationManager) is a library this
        // module links against and calls into directly, not itself a
        // registered IModule -- same as VulkanModule, which needs an
        // IWindowProvider at runtime but takes it as an explicit
        // attachToWindow() parameter rather than listing a dependency
        // here. UiModule follows the same shape: createTree() takes
        // whatever ITextInputProvider it needs directly as a parameter
        // instead of resolving one through Context.

        void onInit(dsbox::Context& context) override;
        void onShutdown(dsbox::Context& context) override;

        // Creates and takes ownership of a new WindowUiTree for
        // `handle`, rooted at `root`. `textInput`, if non-null, is used
        // for startTextInput()/stopTextInput()/setTextInputRect() calls
        // this window's tree issues as focus moves in/out of a
        // text-accepting element -- pass nullptr on a backend that has
        // no ITextInputProvider (the tree simply never composes IME
        // text for that window). Replaces any existing tree already
        // registered for `handle`.
        WindowUiTree& createTree(
            dsbox::WindowHandle handle,
            std::unique_ptr<IUiElement> root,
            dsbox::ITextInputProvider* textInput = nullptr);

        void destroyTree(dsbox::WindowHandle handle);

        // nullptr if no tree has been created for `handle` (or it was
        // already destroyed).
        //
        // Thread-safety note: the returned pointer is only guaranteed
        // valid for as long as nothing calls destroyTree() for the
        // same handle concurrently. In practice this holds because
        // every caller -- UiModule's own "window.*" subscriptions
        // (onInit) included -- runs on the control thread by
        // convention (see WindowUiTree's class comment); destroyTree()
        // must likewise only ever be called from there, never from the
        // presentation/Vulkan thread.
        WindowUiTree* findTree(dsbox::WindowHandle handle);

    private:
        dsbox::Context* m_context = nullptr;

        std::mutex m_treesMutex;
        std::unordered_map<dsbox::WindowHandle, std::unique_ptr<WindowUiTree>> m_trees;
    };
}
