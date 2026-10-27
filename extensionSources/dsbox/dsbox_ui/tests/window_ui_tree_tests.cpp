#include "test_framework.hpp"

#include <dsbox_ui/container.hpp>
#include <dsbox_ui/text_field.hpp>
#include <dsbox_ui/window_ui_tree.hpp>

#include <dsbox/text_input_provider.hpp>

#include <memory>
#include <utility>
#include <variant>
#include <vector>

using namespace dsbox_ui;

namespace
{
    // Records every handleEvent() call made against ANY TestElement that
    // shares this log, in the exact order dispatch made them -- lets a
    // test assert on capture/target/bubble ordering across the whole
    // path, not just what one element individually saw.
    struct DispatchLogEntry
    {
        UiElementId elementId;
        UiEventPhase phase;
    };

    using DispatchLog = std::vector<DispatchLogEntry>;

    // Minimal IUiElement double: bounds are set directly by the test
    // (via setTestBounds()) rather than computed by arrange(), since no
    // ILayoutStrategy implementation exists yet -- these tests are
    // about WindowUiTree's hit-testing/dispatch/tick mechanics, not
    // about layout.
    class TestElement final : public IUiElement
    {
    public:
        explicit TestElement(UiElementId id, DispatchLog& log, UiElementIdMode mode = UiElementIdMode::Instance)
            : m_id(id), m_mode(mode), m_log(log)
        {
        }

        UiElementId id() const noexcept override { return m_id; }
        UiElementIdMode idMode() const noexcept override { return m_mode; }

        void addChild(std::unique_ptr<IUiElement> child)
        {
            m_children.push_back(std::move(child));
        }

        void setTestBounds(UiRect rect) { m_bounds = rect; }

        // If set, handleEvent() returns true (consumed) when called
        // during this phase, false otherwise. Default: never consumes.
        void setConsumeOnPhase(UiEventPhase phase) { m_consumeOnPhase = phase; m_hasConsumeOnPhase = true; }

        UiSize measure(const UiConstraints&, IUiRenderBackend&) override
        {
            ++measureCount;
            return { m_bounds.width, m_bounds.height };
        }

        // Counts measure() calls -- see testRepaintOnlyDirtySkipsMeasureAndArrange()
        // below for why: markDirty()'s own UiDirtyLevel (element.hpp)
        // is otherwise invisible from outside WindowUiTree.
        int measureCount = 0;

        void arrange(const UiRect& rect, IUiRenderBackend&) override { m_bounds = rect; }
        UiRect bounds() const noexcept override { return m_bounds; }

        void paint(DisplayListBuilder& out) const override
        {
            out.rect(m_bounds, UiColor{});
            for (auto& child : m_children)
                child->paint(out);
        }

        bool handleEvent(const UiEvent& event, IUiElementActions&) override
        {
            // Hover tracking (PointerEnterEvent/PointerLeaveEvent --
            // see WindowUiTree::updateHoverState()) is synthesized and
            // dispatched separately, BEFORE the main event below, for
            // every pointer-position event this tree sees -- it isn't
            // what these dispatch-order tests are about, so it's left
            // out of the shared log entirely; every assertion on log
            // size/order below predates hover tracking and means
            // exactly what it says only once Enter/Leave noise is
            // excluded.
            if (std::holds_alternative<PointerEnterEvent>(event.payload) ||
                std::holds_alternative<PointerLeaveEvent>(event.payload))
                return false;

            m_log.push_back({ m_id, event.phase });
            return m_hasConsumeOnPhase && event.phase == m_consumeOnPhase;
        }

        std::span<std::unique_ptr<IUiElement>> children() override
        {
            return { m_children.data(), m_children.size() };
        }

    private:
        UiElementId m_id;
        UiElementIdMode m_mode;
        DispatchLog& m_log;
        UiRect m_bounds;
        std::vector<std::unique_ptr<IUiElement>> m_children;
        UiEventPhase m_consumeOnPhase = UiEventPhase::Target;
        bool m_hasConsumeOnPhase = false;
    };

    // measureText()/loadFont() etc. are never exercised by these tests
    // -- no Text/Image element appears in any tree built here (see
    // text_tests.cpp/image_tests.cpp for those) -- this backend exists
    // only so WindowUiTree::tick() has something to pass to measure().
    class DummyRenderBackend final : public IUiRenderBackend
    {
    public:
        FontHandle loadFont(std::string_view, float) override { return kInvalidFontHandle; }
        TextMetrics measureText(FontHandle, std::string_view) override { return {}; }
        std::vector<ShapedGlyph> shapeText(FontHandle, std::string_view) override { return {}; }
        ImageHandle registerImage(std::uint32_t, std::uint32_t, const std::uint8_t*) override { return kInvalidImageHandle; }
        void submit(const DisplayList&) override {}
    };

    // Fake dsbox::ITextInputProvider -- just counts calls, so a test
    // can assert WindowUiTree actually calls start/stop/
    // setTextInputRect() at the right moments (see WindowUiTree::
    // setFocus()'s own IME wiring) without needing a real platform IME
    // anywhere in this test binary.
    class FakeTextInputProvider final : public dsbox::ITextInputProvider
    {
    public:
        int startCount = 0;
        int stopCount = 0;
        int setRectCount = 0;

        void startTextInput(dsbox::WindowHandle) override { ++startCount; }
        void stopTextInput(dsbox::WindowHandle) override { ++stopCount; }
        void setTextInputRect(dsbox::WindowHandle, int, int, int, int) override { ++setRectCount; }
    };

    // Builds:
    //   root   (id 1) bounds {0,0,100,100}
    //     mid  (id 2) bounds {10,10,50,50}
    //       leaf (id 3) bounds {15,15,10,10}
    // `log` is shared by all three so a test can assert call order.
    std::unique_ptr<TestElement> buildThreeLevelTree(DispatchLog& log, TestElement** outMid = nullptr, TestElement** outLeaf = nullptr)
    {
        auto root = std::make_unique<TestElement>(1, log);
        root->setTestBounds({ 0.0f, 0.0f, 100.0f, 100.0f });

        auto mid = std::make_unique<TestElement>(2, log);
        mid->setTestBounds({ 10.0f, 10.0f, 50.0f, 50.0f });

        auto leaf = std::make_unique<TestElement>(3, log);
        leaf->setTestBounds({ 15.0f, 15.0f, 10.0f, 10.0f });

        TestElement* midPtr = mid.get();
        TestElement* leafPtr = leaf.get();

        mid->addChild(std::move(leaf));
        root->addChild(std::move(mid));

        if (outMid) *outMid = midPtr;
        if (outLeaf) *outLeaf = leafPtr;

        return root;
    }

    void testPointerDispatchHitsDeepestElementAndBubbles()
    {
        DispatchLog log;
        auto root = buildThreeLevelTree(log);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));

        UiEvent input{};
        input.handle = dsbox::WindowHandle{ 1 };
        input.payload = PointerDownEvent{ 20.0f, 20.0f, dsbox::MouseButton::Left };

        UiEvent resolved = tree.dispatchEvent(input);

        DSBOX_UI_CHECK_EQ(resolved.target, static_cast<UiElementId>(3));

        // Expected order: root(Capture), mid(Capture), leaf(Target),
        // mid(Bubble), root(Bubble).
        DSBOX_UI_CHECK_EQ(log.size(), static_cast<std::size_t>(5));
        if (log.size() == 5)
        {
            DSBOX_UI_CHECK(log[0].elementId == 1 && log[0].phase == UiEventPhase::Capture);
            DSBOX_UI_CHECK(log[1].elementId == 2 && log[1].phase == UiEventPhase::Capture);
            DSBOX_UI_CHECK(log[2].elementId == 3 && log[2].phase == UiEventPhase::Target);
            DSBOX_UI_CHECK(log[3].elementId == 2 && log[3].phase == UiEventPhase::Bubble);
            DSBOX_UI_CHECK(log[4].elementId == 1 && log[4].phase == UiEventPhase::Bubble);
        }
    }

    void testStopPropagationHaltsFurtherDispatch()
    {
        DispatchLog log;
        TestElement* mid = nullptr;
        auto root = buildThreeLevelTree(log, &mid);
        mid->setConsumeOnPhase(UiEventPhase::Capture);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));

        UiEvent input{};
        input.handle = dsbox::WindowHandle{ 1 };
        input.payload = PointerDownEvent{ 20.0f, 20.0f, dsbox::MouseButton::Left };

        UiEvent resolved = tree.dispatchEvent(input);

        // Target is still resolved from the hit-test even though
        // dispatch stopped before reaching it.
        DSBOX_UI_CHECK_EQ(resolved.target, static_cast<UiElementId>(3));
        DSBOX_UI_CHECK(resolved.stopPropagation);

        // Only root(Capture) and mid(Capture) should have run.
        DSBOX_UI_CHECK_EQ(log.size(), static_cast<std::size_t>(2));
        if (log.size() == 2)
        {
            DSBOX_UI_CHECK(log[0].elementId == 1 && log[0].phase == UiEventPhase::Capture);
            DSBOX_UI_CHECK(log[1].elementId == 2 && log[1].phase == UiEventPhase::Capture);
        }
    }

    void testSetFocusFiresFocusInAndFocusOut()
    {
        DispatchLog log;
        TestElement* mid = nullptr;
        auto root = buildThreeLevelTree(log, &mid);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));

        tree.setFocus(2);
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), static_cast<UiElementId>(2));
        DSBOX_UI_CHECK_EQ(log.size(), static_cast<std::size_t>(1));
        if (!log.empty())
            DSBOX_UI_CHECK(log.back().elementId == 2 && log.back().phase == UiEventPhase::Target);

        // Unknown id: no-op, focus unchanged, nothing new logged.
        const std::size_t sizeBeforeUnknown = log.size();
        tree.setFocus(999);
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), static_cast<UiElementId>(2));
        DSBOX_UI_CHECK_EQ(log.size(), sizeBeforeUnknown);

        // Clearing focus: FocusOut for the previously-focused element,
        // no FocusIn (kInvalidUiElementId has nothing to focus).
        tree.setFocus(kInvalidUiElementId);
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), kInvalidUiElementId);
        DSBOX_UI_CHECK_EQ(log.size(), sizeBeforeUnknown + 1);
        if (log.size() == sizeBeforeUnknown + 1)
            DSBOX_UI_CHECK(log.back().elementId == 2 && log.back().phase == UiEventPhase::Target);
    }

    void testKeyboardEventTargetsFocusedElement()
    {
        DispatchLog log;
        auto root = buildThreeLevelTree(log);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        tree.setFocus(2);
        log.clear(); // drop the FocusIn entry from setFocus() itself

        UiEvent input{};
        input.handle = dsbox::WindowHandle{ 1 };
        input.payload = KeyDownEvent{ dsbox::KeyCode::A, dsbox::KeyModifier::None, false };

        UiEvent resolved = tree.dispatchEvent(input);

        DSBOX_UI_CHECK_EQ(resolved.target, static_cast<UiElementId>(2));

        // Path is [root, mid] -- capture: root only; target: mid;
        // bubble: root only.
        DSBOX_UI_CHECK_EQ(log.size(), static_cast<std::size_t>(3));
        if (log.size() == 3)
        {
            DSBOX_UI_CHECK(log[0].elementId == 1 && log[0].phase == UiEventPhase::Capture);
            DSBOX_UI_CHECK(log[1].elementId == 2 && log[1].phase == UiEventPhase::Target);
            DSBOX_UI_CHECK(log[2].elementId == 1 && log[2].phase == UiEventPhase::Bubble);
        }
    }

    // Regression test for tick()'s own two-tier dirty tracking (see
    // its declaration, window_ui_tree.hpp, and UiDirtyLevel's doc,
    // element.hpp): a markDirty() call at UiDirtyLevel::Repaint must
    // still publish a new DisplayList snapshot (paint() always reruns
    // when anything is dirty) but must NOT re-run measure()/arrange()
    // -- TestElement::measureCount is how this test can tell the
    // difference from outside WindowUiTree itself. A subsequent
    // UiDirtyLevel::Layout call (the default) must still force a real
    // measure()/arrange() pass, same as every markDirty() call did
    // before this distinction existed.
    void testRepaintOnlyDirtySkipsMeasureAndArrange()
    {
        DispatchLog log;
        TestElement* mid = nullptr;
        auto root = buildThreeLevelTree(log, &mid);
        TestElement* rootPtr = root.get();

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        DummyRenderBackend backend;

        tree.tick(backend, UiSize{ 100.0f, 100.0f });
        const int measureCountAfterFirstTick = rootPtr->measureCount;
        DSBOX_UI_CHECK(measureCountAfterFirstTick > 0); // sanity: the first tick() always lays out

        auto firstSnapshot = tree.currentDisplayList();

        tree.markDirty(mid->id(), UiDirtyLevel::Repaint);
        tree.tick(backend, UiSize{ 100.0f, 100.0f });

        // A new snapshot WAS published (paint() still reran)...
        auto secondSnapshot = tree.currentDisplayList();
        DSBOX_UI_CHECK(secondSnapshot.get() != firstSnapshot.get());
        // ...but measure() did NOT run again.
        DSBOX_UI_CHECK_EQ(rootPtr->measureCount, measureCountAfterFirstTick);

        tree.markDirty(mid->id()); // defaults to UiDirtyLevel::Layout
        tree.tick(backend, UiSize{ 100.0f, 100.0f });

        // A Layout-level call DOES force measure() to run again.
        DSBOX_UI_CHECK(rootPtr->measureCount > measureCountAfterFirstTick);
    }

    void testTickOnlyPublishesANewSnapshotWhenDirty()
    {
        DispatchLog log;
        auto root = buildThreeLevelTree(log);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        DummyRenderBackend backend;

        DSBOX_UI_CHECK(tree.currentDisplayList() == nullptr);

        tree.tick(backend, UiSize{ 100.0f, 100.0f });
        auto first = tree.currentDisplayList();
        DSBOX_UI_CHECK(first != nullptr);

        // Not dirty anymore -- a second tick() should be a no-op.
        tree.tick(backend, UiSize{ 100.0f, 100.0f });
        auto second = tree.currentDisplayList();
        DSBOX_UI_CHECK(first.get() == second.get());

        tree.markDirty(3);
        tree.tick(backend, UiSize{ 100.0f, 100.0f });
        auto third = tree.currentDisplayList();
        DSBOX_UI_CHECK(third != nullptr);
        DSBOX_UI_CHECK(third.get() != second.get());
    }

    void testTabTraversalCyclesThroughFocusablesAndWrapsBothWays()
    {
        // Three focusable Containers directly under a non-focusable
        // root -- collectFocusables() (window_ui_tree.cpp) walks by
        // dynamic_cast<IFocusTarget*>, not by any special "is a
        // Container" knowledge, so this also doubles as the one place
        // Container's own IFocusTarget implementation gets exercised
        // through the real Tab path rather than isFocusable() in
        // isolation (see container_tests.cpp for that).
        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");

        auto& a = static_cast<Container&>(root->addChild(std::make_unique<Container>(2, UiElementIdMode::Unique, "a")));
        a.setFocusable(true);
        auto& b = static_cast<Container&>(root->addChild(std::make_unique<Container>(3, UiElementIdMode::Unique, "b")));
        b.setFocusable(true);
        auto& c = static_cast<Container&>(root->addChild(std::make_unique<Container>(4, UiElementIdMode::Unique, "c")));
        c.setFocusable(true);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), kInvalidUiElementId);

        auto tab = [](bool shift)
        {
            UiEvent event{};
            event.handle = dsbox::WindowHandle{ 1 };
            event.payload = KeyDownEvent{ dsbox::KeyCode::Tab, shift ? dsbox::KeyModifier::Shift : dsbox::KeyModifier::None, false };
            return event;
        };

        // Nothing focused yet: Tab goes to the first focusable.
        tree.dispatchEvent(tab(false));
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), static_cast<UiElementId>(2));

        tree.dispatchEvent(tab(false));
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), static_cast<UiElementId>(3));

        tree.dispatchEvent(tab(false));
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), static_cast<UiElementId>(4));

        // Past the last -> wraps to the first.
        tree.dispatchEvent(tab(false));
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), static_cast<UiElementId>(2));

        // Shift+Tab: before the first -> wraps to the last.
        tree.dispatchEvent(tab(true));
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), static_cast<UiElementId>(4));

        tree.dispatchEvent(tab(true));
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), static_cast<UiElementId>(3));
    }

    // Regression test for "releasing the mouse button off-screen leaves
    // it stuck pressed": once something captures the pointer (as
    // Container now does on PointerDownEvent -- see container.cpp),
    // every subsequent pointer event must route to the capturing
    // element regardless of where it hit-tests, including coordinates
    // well outside every element's bounds (simulating the cursor
    // having left the window). Capture must then auto-release the
    // moment the captured element actually receives a PointerUpEvent,
    // so dispatch goes back to normal position-based hit-testing
    // afterward.
    void testPointerCaptureRoutesEventsToCapturedElementRegardlessOfPosition()
    {
        DispatchLog log;
        TestElement* mid = nullptr;
        TestElement* leaf = nullptr;
        auto root = buildThreeLevelTree(log, &mid, &leaf);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));

        // A PointerDownEvent at leaf's own position hit-tests to leaf
        // normally; a real element (Container) would call
        // setPointerCapture(leaf->id()) from inside its own
        // PointerDownEvent handler (see container.cpp) -- simulated
        // directly here since TestElement itself doesn't.
        UiEvent down{};
        down.handle = dsbox::WindowHandle{ 1 };
        down.payload = PointerDownEvent{ 20.0f, 20.0f, dsbox::MouseButton::Left };
        UiEvent resolvedDown = tree.dispatchEvent(down);
        DSBOX_UI_CHECK_EQ(resolvedDown.target, static_cast<UiElementId>(3)); // leaf

        tree.setPointerCapture(leaf->id());
        log.clear();

        // Move far outside every element's bounds -- without capture
        // this would hit nothing (root's own bounds top out at 100,100).
        UiEvent move{};
        move.handle = dsbox::WindowHandle{ 1 };
        move.payload = PointerMoveEvent{ 500.0f, 500.0f, 0.0f, 0.0f };
        UiEvent resolvedMove = tree.dispatchEvent(move);
        DSBOX_UI_CHECK_EQ(resolvedMove.target, static_cast<UiElementId>(3)); // still leaf
        DSBOX_UI_CHECK(!log.empty() && log.back().elementId == 3);

        log.clear();

        // Release, also far outside every element's bounds.
        UiEvent up{};
        up.handle = dsbox::WindowHandle{ 1 };
        up.payload = PointerUpEvent{ 500.0f, 500.0f, dsbox::MouseButton::Left };
        UiEvent resolvedUp = tree.dispatchEvent(up);
        DSBOX_UI_CHECK_EQ(resolvedUp.target, static_cast<UiElementId>(3)); // still leaf -- capture held for the up event itself
        DSBOX_UI_CHECK(!log.empty() && log.back().elementId == 3);

        log.clear();

        // Capture must have auto-released: a move now at mid's own
        // position (but outside leaf's) hit-tests back to mid, not leaf.
        UiEvent moveAfterRelease{};
        moveAfterRelease.handle = dsbox::WindowHandle{ 1 };
        moveAfterRelease.payload = PointerMoveEvent{ 12.0f, 12.0f, 0.0f, 0.0f }; // inside mid {10,10,50,50}, outside leaf {15,15,10,10}
        UiEvent resolvedAfter = tree.dispatchEvent(moveAfterRelease);
        DSBOX_UI_CHECK_EQ(resolvedAfter.target, static_cast<UiElementId>(2)); // mid, not leaf
    }

    // Regression test for cancelPointerCapture() (window_ui_tree.cpp)
    // via the explicit releasePointerCapture() path: calling it for
    // whatever element currently holds capture must dispatch exactly
    // one PointerCancelEvent at Target phase to that element, and must
    // actually clear capture (so routing goes back to normal
    // hit-testing afterward) -- see PointerCancelEvent's own doc
    // (event.hpp) for why this is the right moment to fire it.
    void testReleasePointerCaptureFiresPointerCancelEvent()
    {
        DispatchLog log;
        TestElement* mid = nullptr;
        TestElement* leaf = nullptr;
        auto root = buildThreeLevelTree(log, &mid, &leaf);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        tree.setPointerCapture(leaf->id());
        log.clear();

        tree.releasePointerCapture(leaf->id());

        DSBOX_UI_CHECK_EQ(log.size(), static_cast<std::size_t>(1));
        if (!log.empty())
            DSBOX_UI_CHECK(log[0].elementId == 3 && log[0].phase == UiEventPhase::Target);

        // Capture is actually cleared -- a move at mid's own position
        // (but outside leaf's) now hit-tests back to mid, not leaf.
        log.clear();
        UiEvent moveAfterRelease{};
        moveAfterRelease.handle = dsbox::WindowHandle{ 1 };
        moveAfterRelease.payload = PointerMoveEvent{ 12.0f, 12.0f, 0.0f, 0.0f };
        UiEvent resolved = tree.dispatchEvent(moveAfterRelease);
        DSBOX_UI_CHECK_EQ(resolved.target, static_cast<UiElementId>(2));

        // releasePointerCapture() for an id that doesn't hold capture
        // (nothing does, now) is a documented no-op -- no crash, no
        // new log entry.
        log.clear();
        tree.releasePointerCapture(leaf->id());
        DSBOX_UI_CHECK(log.empty());
    }

    // Regression test for cancelPointerCapture() via the setFocus()
    // path: a focus change while something holds pointer capture must
    // end that gesture with a PointerCancelEvent, same as an explicit
    // releasePointerCapture() call -- see setFocus()'s own ordering
    // comment (window_ui_tree.cpp) for why this runs BEFORE the
    // FocusOut/FocusIn pair below it.
    void testSetFocusCancelsActivePointerCapture()
    {
        DispatchLog log;
        TestElement* mid = nullptr;
        TestElement* leaf = nullptr;
        auto root = buildThreeLevelTree(log, &mid, &leaf);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        tree.setPointerCapture(leaf->id());
        log.clear();

        tree.setFocus(mid->id());

        // First entry is the cancel (to leaf, the captured element);
        // later entries are the ordinary FocusIn for mid -- what
        // matters here is only that the cancel happened and happened
        // first.
        DSBOX_UI_CHECK(!log.empty());
        if (!log.empty())
            DSBOX_UI_CHECK(log[0].elementId == 3 && log[0].phase == UiEventPhase::Target);
    }

    void testFocusingTextFieldStartsTextInputAndBlurStopsIt()
    {
        FakeTextInputProvider provider;

        auto field = std::make_unique<TextField>(1, UiElementIdMode::Unique, "field");
        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(field), &provider);

        DSBOX_UI_CHECK_EQ(provider.startCount, 0);

        tree.setFocus(1);
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), static_cast<UiElementId>(1));
        DSBOX_UI_CHECK_EQ(provider.startCount, 1);
        DSBOX_UI_CHECK_EQ(provider.stopCount, 0);
        DSBOX_UI_CHECK(provider.setRectCount >= 1); // at least the one setFocus() itself issues

        tree.setFocus(kInvalidUiElementId);
        DSBOX_UI_CHECK_EQ(provider.startCount, 1); // unchanged -- only stop happened
        DSBOX_UI_CHECK_EQ(provider.stopCount, 1);

        // Re-focusing starts it again -- this is a toggle, not a
        // one-shot.
        tree.setFocus(1);
        DSBOX_UI_CHECK_EQ(provider.startCount, 2);
    }
}

void runWindowUiTreeTests()
{
    dsbox_ui_tests::runTest("PointerDispatchHitsDeepestElementAndBubbles", testPointerDispatchHitsDeepestElementAndBubbles);
    dsbox_ui_tests::runTest("StopPropagationHaltsFurtherDispatch", testStopPropagationHaltsFurtherDispatch);
    dsbox_ui_tests::runTest("SetFocusFiresFocusInAndFocusOut", testSetFocusFiresFocusInAndFocusOut);
    dsbox_ui_tests::runTest("KeyboardEventTargetsFocusedElement", testKeyboardEventTargetsFocusedElement);
    dsbox_ui_tests::runTest("RepaintOnlyDirtySkipsMeasureAndArrange", testRepaintOnlyDirtySkipsMeasureAndArrange);
    dsbox_ui_tests::runTest("TickOnlyPublishesANewSnapshotWhenDirty", testTickOnlyPublishesANewSnapshotWhenDirty);
    dsbox_ui_tests::runTest("TabTraversalCyclesThroughFocusablesAndWrapsBothWays", testTabTraversalCyclesThroughFocusablesAndWrapsBothWays);
    dsbox_ui_tests::runTest("PointerCaptureRoutesEventsToCapturedElementRegardlessOfPosition", testPointerCaptureRoutesEventsToCapturedElementRegardlessOfPosition);
    dsbox_ui_tests::runTest("ReleasePointerCaptureFiresPointerCancelEvent", testReleasePointerCaptureFiresPointerCancelEvent);
    dsbox_ui_tests::runTest("SetFocusCancelsActivePointerCapture", testSetFocusCancelsActivePointerCapture);
    dsbox_ui_tests::runTest("FocusingTextFieldStartsTextInputAndBlurStopsIt", testFocusingTextFieldStartsTextInputAndBlurStopsIt);
}
