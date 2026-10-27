#include <dsbox_ui/window_ui_tree.hpp>

#include <algorithm>
#include <cassert>
#include <vector>

namespace dsbox_ui
{
    namespace
    {
        // Depth-first search for the element with `target` id. Fills
        // `outPath` with every element from `node` down to (and
        // including) the match, root-first, on success. Returns false
        // (leaving outPath untouched) if no descendant of `node` --
        // including `node` itself -- has that id.
        //
        // O(tree size) per call: fine for v1's "relayout/dispatch
        // touches the whole tree anyway" approach (see WindowUiTree::
        // tick()'s own comment); a follow-up that makes relayout truly
        // incremental should give WindowUiTree an id -> element index
        // instead of repeating this walk per event.
        bool findPath(IUiElement& node, UiElementId target, std::vector<IUiElement*>& outPath)
        {
            outPath.push_back(&node);

            if (node.id() == target)
                return true;

            for (auto& child : node.children())
            {
                if (child && findPath(*child, target, outPath))
                    return true;
            }

            outPath.pop_back();
            return false;
        }

        // Same idea as findPath(), but by position (`point`) instead of
        // by id: walks down through whichever child's bounds() contains
        // `point`, picking the LAST such child at each level so the
        // deepest, most specific element under the point wins (an inner
        // element drawn on top of its own container is preferred over
        // the container itself). `node` itself is always included, on
        // the assumption the caller only starts this from something
        // that already contains `point` (WindowUiTree::dispatchEvent()
        // starts it from the root, whose bounds() is the whole window).
        void hitTestPath(IUiElement& node, UiPoint point, std::vector<IUiElement*>& outPath)
        {
            outPath.push_back(&node);

            for (auto& child : node.children())
            {
                if (child && child->bounds().contains(point))
                {
                    hitTestPath(*child, point, outPath);
                    return;
                }
            }
        }

        UiPoint pointerPositionOf(const UiEventPayload& payload)
        {
            if (auto* down = std::get_if<PointerDownEvent>(&payload))
                return { down->x, down->y };
            if (auto* up = std::get_if<PointerUpEvent>(&payload))
                return { up->x, up->y };
            if (auto* move = std::get_if<PointerMoveEvent>(&payload))
                return { move->x, move->y };

            return { 0.0f, 0.0f };
        }

        bool isPointerPayload(const UiEventPayload& payload)
        {
            return std::holds_alternative<PointerDownEvent>(payload)
                || std::holds_alternative<PointerUpEvent>(payload)
                || std::holds_alternative<PointerMoveEvent>(payload);
        }

        // Depth-first, children in declaration order -- see
        // IFocusTarget's own doc (element.hpp) for why this is the
        // traversal order and not some separately-specified one.
        // Appends every element under `node` (`node` itself included)
        // for which dynamic_cast<IFocusTarget*> succeeds AND
        // isFocusable() is true.
        void collectFocusables(IUiElement& node, std::vector<IUiElement*>& out)
        {
            if (auto* focusTarget = dynamic_cast<IFocusTarget*>(&node); focusTarget && focusTarget->isFocusable())
                out.push_back(&node);

            for (auto& child : node.children())
                if (child)
                    collectFocusables(*child, out);
        }

        bool isTabKeyDown(const UiEventPayload& payload)
        {
            const auto* keyDown = std::get_if<KeyDownEvent>(&payload);
            return keyDown && keyDown->key == dsbox::KeyCode::Tab;
        }
    }

    WindowUiTree::WindowUiTree(dsbox::WindowHandle handle, std::unique_ptr<IUiElement> root, dsbox::ITextInputProvider* textInput)
        : m_handle(handle)
        , m_root(std::move(root))
        , m_textInput(textInput)
    {
    }

    // Declared (not left implicit) and defined here, out-of-line, for
    // the same reason Input's destructor is -- see input.hpp's own
    // comment: an implicit special member of a DSBOX_UI_API class can
    // otherwise fail to actually get exported from the DLL on MSVC if
    // nothing inside this module's own build happens to odr-use it.
    WindowUiTree::~WindowUiTree() = default;

    void WindowUiTree::setFocus(UiElementId target)
    {
        std::lock_guard<std::recursive_mutex> lock(m_treeMutex);

        if (target == m_focusedElement)
            return;

        // A focus change ends any in-progress pointer gesture early --
        // see PointerCancelEvent's own doc (event.hpp) and
        // cancelPointerCapture()'s own doc (window_ui_tree.hpp) for
        // why. Done FIRST, before anything else below, so a
        // PointerCancelEvent handler that itself reacts by changing
        // focus again isn't fighting the focus change already under
        // way here.
        cancelPointerCapture();

        std::vector<IUiElement*> path;

        // A focus target the tree can't find is a no-op rather than a
        // hard error -- the caller may be reacting to something (e.g. a
        // click) against a tree snapshot that's since been rebuilt.
        if (target != kInvalidUiElementId && !findPath(*m_root, target, path))
            return;

        if (!path.empty())
        {
            assert(path.back()->idMode() != UiElementIdMode::Index
                && "UiElementIdMode::Index is only valid for the duration of a single pass; "
                   "it must not be handed to setFocus(), which needs the id to still mean "
                   "the same element on every future call.");
        }

        if (m_focusedElement != kInvalidUiElementId)
        {
            std::vector<IUiElement*> oldPath;
            if (findPath(*m_root, m_focusedElement, oldPath) && !oldPath.empty())
                oldPath.back()->handleEvent(UiEvent{ m_handle, m_focusedElement, UiEventPhase::Target, false, FocusOutEvent{} }, *this);
        }

        // Stop IME composition for whatever was focused before -- do
        // this AFTER the FocusOutEvent above (so a text field reacting
        // to losing focus, e.g. to commit/validate, still sees
        // m_textInputActive true for the duration of its own handler)
        // but BEFORE m_focusedElement is reassigned below.
        if (m_textInputActive && m_textInput)
        {
            m_textInput->stopTextInput(m_handle);
            m_textInputActive = false;
        }

        m_focusedElement = target;

        if (!path.empty())
            path.back()->handleEvent(UiEvent{ m_handle, target, UiEventPhase::Target, false, FocusInEvent{} }, *this);

        // Start IME composition for the newly focused element, if it
        // wants text input at all -- see ITextInputTarget's own doc
        // (element.hpp). Done AFTER the FocusInEvent above so a text
        // field can, from inside its own FocusInEvent handler, change
        // whatever textInputRect() will report (e.g. after moving its
        // caret to the end) before setTextInputRect() below reads it.
        if (!path.empty() && m_textInput)
        {
            if (auto* textTarget = dynamic_cast<ITextInputTarget*>(path.back()); textTarget && textTarget->acceptsTextInput())
            {
                m_textInput->startTextInput(m_handle);
                m_textInputActive = true;

                const UiRect rect = textTarget->textInputRect();
                m_textInput->setTextInputRect(
                    m_handle, static_cast<int>(rect.x), static_cast<int>(rect.y),
                    static_cast<int>(rect.width), static_cast<int>(rect.height));
            }
        }

        m_dirty = true;
    }

    void WindowUiTree::markDirty(UiElementId target, UiDirtyLevel level)
    {
        std::lock_guard<std::recursive_mutex> lock(m_treeMutex);
        m_dirty = true;

        if (level == UiDirtyLevel::Layout)
        {
            m_needsRelayout = true;

            // Recorded for tick()'s own per-subtree scoping -- see
            // this method's own declaration (window_ui_tree.hpp) and
            // IUiElement::prepareIncrementalRelayout() (element.hpp).
            // kInvalidUiElementId is never a real element's id() so
            // inserting it would just be dead weight in the set -- it
            // could never match anything prepareIncrementalRelayout()
            // checks against.
            if (target != kInvalidUiElementId)
                m_dirtyElements.insert(target);
        }
    }

    void WindowUiTree::updateHoverState(const std::vector<IUiElement*>& newPath)
    {
        // Caller (dispatchEvent()) already holds m_treeMutex.
        std::vector<UiElementId> newIds;
        newIds.reserve(newPath.size());
        for (IUiElement* element : newPath)
            newIds.push_back(element->id());

        // Root-first (both paths start at m_root), so the first
        // mismatch is where the hovered chain actually diverges --
        // everything before it is still hovered and gets no event at
        // all, same as a real mouseenter/mouseleave pair wouldn't fire
        // for an ancestor the pointer never actually left.
        std::size_t commonLen = 0;
        while (commonLen < newIds.size() && commonLen < m_hoveredPath.size()
               && newIds[commonLen] == m_hoveredPath[commonLen])
        {
            ++commonLen;
        }

        // Leave, deepest-first (mirrors a real mouseleave sequence: the
        // innermost element the pointer left stops being hovered before
        // any of its ancestors do). Re-finds each element by id rather
        // than keeping the OLD path's raw pointers around -- the tree
        // may have been mutated (clearChildren(), a handler swapping a
        // panel, ...) in whatever happened between the pointer event
        // that last updated m_hoveredPath and this one; an id that no
        // longer resolves to anything simply gets no Leave event, same
        // "not found is a quiet no-op" convention setFocus() already
        // uses for the same reason.
        for (std::size_t i = m_hoveredPath.size(); i-- > commonLen; )
        {
            std::vector<IUiElement*> stalePath;
            if (findPath(*m_root, m_hoveredPath[i], stalePath) && !stalePath.empty())
            {
                stalePath.back()->handleEvent(
                    UiEvent{ m_handle, m_hoveredPath[i], UiEventPhase::Target, false, PointerLeaveEvent{} },
                    *this);
            }
        }

        // Enter, shallow-to-deep (mirrors mouseenter: an outer element
        // becomes hovered before whatever's nested inside it does).
        // `newPath`'s own pointers are live right now -- this is the
        // very path dispatchEvent() just computed for the real event
        // about to follow, not something carried over from a previous
        // call.
        for (std::size_t i = commonLen; i < newPath.size(); ++i)
        {
            newPath[i]->handleEvent(
                UiEvent{ m_handle, newIds[i], UiEventPhase::Target, false, PointerEnterEvent{} },
                *this);
        }

        m_hoveredPath = std::move(newIds);
    }

    void WindowUiTree::setPointerCapture(UiElementId id)
    {
        std::lock_guard<std::recursive_mutex> lock(m_treeMutex);
        m_pointerCaptureTarget = id;
    }

    void WindowUiTree::releasePointerCapture(UiElementId id)
    {
        std::lock_guard<std::recursive_mutex> lock(m_treeMutex);
        if (m_pointerCaptureTarget == id)
            cancelPointerCapture();
    }

    void WindowUiTree::cancelPointerCapture()
    {
        // Caller already holds m_treeMutex.
        if (m_pointerCaptureTarget == kInvalidUiElementId)
            return;

        const UiElementId target = m_pointerCaptureTarget;

        // Cleared BEFORE dispatch -- same reasoning dispatchEvent()'s
        // own PointerUpEvent auto-release uses (window_ui_tree.cpp):
        // a PointerCancelEvent handler that reacts by starting a NEW
        // gesture (calling setPointerCapture() again) must not have
        // that immediately undone by this line running afterward.
        m_pointerCaptureTarget = kInvalidUiElementId;

        std::vector<IUiElement*> capturedPath;
        if (findPath(*m_root, target, capturedPath) && !capturedPath.empty())
        {
            capturedPath.back()->handleEvent(
                UiEvent{ m_handle, target, UiEventPhase::Target, false, PointerCancelEvent{} }, *this);
        }
    }

    UiEvent WindowUiTree::dispatchEvent(const UiEvent& event)
    {
        std::lock_guard<std::recursive_mutex> lock(m_treeMutex);

        // Tab / Shift+Tab: intercepted here, before the normal hit-
        // test/focus dispatch below, and never forwarded to any
        // element's handleEvent() at all -- same reasoning browsers
        // use for default Tab handling. setFocus() below does its own
        // FocusOut/FocusIn dispatch and IME start/stop, so this simply
        // decides WHICH element to focus next.
        if (isTabKeyDown(event.payload))
        {
            std::vector<IUiElement*> focusables;
            collectFocusables(*m_root, focusables);

            UiEvent working = event;
            working.target = m_focusedElement;
            working.phase = UiEventPhase::Target;

            if (!focusables.empty())
            {
                const auto* keyDown = std::get_if<KeyDownEvent>(&event.payload);
                const bool shiftHeld = keyDown && dsbox::hasModifier(keyDown->modifiers, dsbox::KeyModifier::Shift);

                auto currentIt = std::find_if(focusables.begin(), focusables.end(),
                    [this](IUiElement* element) { return element->id() == m_focusedElement; });

                std::size_t nextIndex = 0;
                if (currentIt != focusables.end())
                {
                    const std::size_t currentIndex = static_cast<std::size_t>(currentIt - focusables.begin());
                    // Wraps both ways (Tab past the last -> first,
                    // Shift+Tab before the first -> last), same as a
                    // browser's own Tab-order cycling.
                    nextIndex = shiftHeld
                        ? (currentIndex == 0 ? focusables.size() - 1 : currentIndex - 1)
                        : (currentIndex + 1) % focusables.size();
                }
                else
                {
                    // Nothing focused yet (or the focused element is no
                    // longer focusable/present): Tab goes to the first
                    // focusable, Shift+Tab to the last.
                    nextIndex = shiftHeld ? focusables.size() - 1 : 0;
                }

                working.target = focusables[nextIndex]->id();
            }

            working.stopPropagation = true; // Tab is always fully consumed by traversal, never bubbled to any element
            setFocus(working.target);
            return working;
        }

        std::vector<IUiElement*> path;

        if (isPointerPayload(event.payload))
        {
            // Pointer capture (see IUiElementActions::
            // setPointerCapture()) overrides the normal position-based
            // hit test entirely while it's active: every pointer event,
            // including ones whose coordinates now fall outside every
            // element's bounds (the cursor having left the window, or
            // even gone negative/off-screen), still routes straight to
            // the capturing element at Target phase. This is what lets
            // e.g. Container::m_pressed (see its own PointerDownEvent
            // handler) reliably see the PointerUpEvent that ends its
            // press, instead of that event hitting whatever happens to
            // be at those coordinates -- or nothing at all.
            if (m_pointerCaptureTarget != kInvalidUiElementId &&
                findPath(*m_root, m_pointerCaptureTarget, path))
            {
                // findPath() already populated `path` root -> capture
                // target; nothing further to do here.
            }
            else
            {
                hitTestPath(*m_root, pointerPositionOf(event.payload), path);
            }

            // Hover tracking rides along every pointer-position event
            // (not just PointerMoveEvent) so a PointerDownEvent/
            // PointerUpEvent that happens to be the first positional
            // event in a while (no preceding move -- a touch screen,
            // say) still establishes/updates hover correctly rather
            // than waiting for a move that may never come. Uses
            // whichever `path` was just computed above -- capture-
            // routed or hit-tested -- so while pointer capture is
            // active, hover simply follows the captured element's own
            // path rather than the raw cursor position, same as
            // `working.target` does for the event itself just below.
            updateHoverState(path);
        }
        else
        {
            // Keyboard/text/focus events target whatever's currently
            // focused; fall back to the root so a window with nothing
            // focused yet still receives e.g. a stray keypress.
            UiElementId target = (m_focusedElement != kInvalidUiElementId) ? m_focusedElement : m_root->id();
            if (!findPath(*m_root, target, path))
                path = { m_root.get() };
        }

        UiEvent working = event;

        if (path.empty())
            return working;

        working.target = path.back()->id();

        // Auto-release: once a PointerUpEvent has been routed to the
        // capturing element (which `path` is guaranteed to end at here,
        // since the capture branch above only takes effect when
        // findPath() actually found m_pointerCaptureTarget), capture
        // ends -- a single press/drag/release gesture is over. Cleared
        // BEFORE dispatch below so a handler that calls
        // setPointerCapture() again from within this very
        // PointerUpEvent (starting a new gesture) isn't immediately
        // undone by this line.
        if (std::holds_alternative<PointerUpEvent>(event.payload) &&
            working.target == m_pointerCaptureTarget)
        {
            m_pointerCaptureTarget = kInvalidUiElementId;
        }

        // Capture: root -> just before target.
        working.phase = UiEventPhase::Capture;
        for (std::size_t i = 0; i + 1 < path.size(); ++i)
        {
            if (path[i]->handleEvent(working, *this))
            {
                working.stopPropagation = true;
                return working;
            }
        }

        // Target.
        working.phase = UiEventPhase::Target;
        if (path.back()->handleEvent(working, *this))
        {
            working.stopPropagation = true;
            return working;
        }

        // Bubble: just before target -> root.
        working.phase = UiEventPhase::Bubble;
        for (std::size_t i = path.size(); i-- > 0;)
        {
            if (i + 1 == path.size())
                continue; // target itself already handled above

            if (path[i]->handleEvent(working, *this))
            {
                working.stopPropagation = true;
                return working;
            }
        }

        return working;
    }

    void WindowUiTree::tick(IUiRenderBackend& backend, UiSize windowSize)
    {
        std::lock_guard<std::recursive_mutex> lock(m_treeMutex);

        const bool sizeChanged =
            windowSize.width != m_lastWindowSize.width || windowSize.height != m_lastWindowSize.height;

        if (sizeChanged)
            m_needsRelayout = true; // a resize can only ever be a Layout-level change -- see this method's own declaration (window_ui_tree.hpp)

        if (!m_dirty && !sizeChanged)
            return;

        m_lastWindowSize = windowSize;

        // Two-tier dirty (see this method's own declaration,
        // window_ui_tree.hpp, and UiDirtyLevel's doc, element.hpp):
        // measure()/arrange() only run when something markDirty()'d
        // itself at UiDirtyLevel::Layout (or the window resized, just
        // above) since the last tick() actually committed a pass --
        // m_needsRelayout tracks exactly that, independently of
        // m_dirty itself. A Repaint-only pass (e.g. only a pressed/
        // hover color changed) skips straight to the paint() call
        // below, re-using whatever m_root->arrange() last recorded
        // rather than re-deriving the same boxes over again. When this
        // DOES run, it's further scoped per-subtree below -- see this
        // method's own declaration for the full picture.
        if (m_needsRelayout)
        {
            // Per-subtree scoping (see this method's own declaration,
            // window_ui_tree.hpp): a resize can change ANY element's
            // effective constraints even when nothing about it was
            // ever specifically markDirty()'d, so it bypasses scoping
            // entirely and forces every element to recompute --
            // forceFullRelayout() does exactly that, discarding
            // whatever an earlier prepareIncrementalRelayout() call
            // left cached. Otherwise, m_dirtyElements (accumulated by
            // markDirty() since the last pass that got here) decides,
            // per element, whether its own cached measure()/arrange()
            // results can simply be reused.
            if (sizeChanged)
                m_root->forceFullRelayout();
            else
                m_root->prepareIncrementalRelayout(m_dirtyElements);

            UiConstraints rootConstraints = UiConstraints::tight(windowSize.width, windowSize.height);
            m_root->measure(rootConstraints, backend);
            m_root->arrange(UiRect{ 0.0f, 0.0f, windowSize.width, windowSize.height }, backend);

            // A relayout can move the focused element (a resize, most
            // commonly) -- keep the platform IME's candidate window
            // pointed at wherever the caret actually ended up, not
            // where it was when text input last started (see
            // setFocus()'s own comment for the OTHER half of this
            // wiring). Only meaningful right after an actual
            // measure()/arrange() pass -- a Repaint-only tick() can't
            // have moved anything, so there's nothing new to tell the
            // IME here.
            if (m_textInputActive && m_textInput)
            {
                std::vector<IUiElement*> focusedPath;
                if (findPath(*m_root, m_focusedElement, focusedPath) && !focusedPath.empty())
                {
                    if (auto* textTarget = dynamic_cast<ITextInputTarget*>(focusedPath.back()))
                    {
                        const UiRect rect = textTarget->textInputRect();
                        m_textInput->setTextInputRect(
                            m_handle, static_cast<int>(rect.x), static_cast<int>(rect.y),
                            static_cast<int>(rect.width), static_cast<int>(rect.height));
                    }
                }
            }

            m_needsRelayout = false;
            m_dirtyElements.clear(); // consumed by prepareIncrementalRelayout() above -- the next Layout-level markDirty() starts a fresh set
        }

        DisplayListBuilder builder;
        m_root->paint(builder);

        auto newList = std::make_shared<DisplayList>(builder.take());
        m_currentList.store(std::move(newList), std::memory_order_release);

        m_dirty = false;
    }

    std::shared_ptr<const DisplayList> WindowUiTree::currentDisplayList() const
    {
        return m_currentList.load(std::memory_order_acquire);
    }
}
