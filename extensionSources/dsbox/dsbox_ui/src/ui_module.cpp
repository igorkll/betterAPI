#include <dsbox_ui/window_ui_tree.hpp>

#include <dsbox/input.hpp>
#include <dsbox/text_input_provider.hpp>

#include <utility>

namespace dsbox_ui
{
    namespace
    {
        // Re-publishes an already-dispatched UiEvent on both bus
        // topics described in event.hpp -- the per-window one first
        // (the common case: something listening to exactly this
        // window), then the catch-all. Called with dispatchEvent()'s
        // RETURN VALUE, never the raw UiEvent a subscription lambda
        // below builds before dispatch -- only the returned copy has
        // `target`/`phase`/`stopPropagation` actually resolved.
        void publishUiEvent(dsbox::Context& context, const UiEvent& resolved)
        {
            context.comm().emit<UiEvent>(uiEventTopic(resolved.handle), resolved);
            context.comm().emit<UiEvent>(std::string(kUiEventTopicAny), resolved);
        }
    }

    void UiModule::onInit(dsbox::Context& context)
    {
        m_context = &context;

        // Six subscriptions total, one per raw event type this module
        // understands how to translate -- not one per window. A tree
        // is looked up by the handle already carried on every one of
        // these raw events (see input.hpp/text_input_provider.hpp), the
        // same way dsbox_sdl itself publishes them; a window with no
        // tree yet (createTree() not called for it) simply has its
        // events dropped here.

        context.comm().registerEvent<dsbox::KeyEvent>("window.key",
            [this](dsbox::KeyEvent raw)
            {
                WindowUiTree* tree = findTree(raw.handle);
                if (!tree)
                    return;

                UiEventPayload payload = raw.pressed
                    ? UiEventPayload{ KeyDownEvent{ raw.key, raw.modifiers, raw.repeat } }
                    : UiEventPayload{ KeyUpEvent{ raw.key, raw.modifiers } };

                UiEvent event{ raw.handle, kInvalidUiElementId, UiEventPhase::Target, false, std::move(payload) };
                publishUiEvent(*m_context, tree->dispatchEvent(event));
            });

        context.comm().registerEvent<dsbox::MouseButtonEvent>("window.mouse_button",
            [this](dsbox::MouseButtonEvent raw)
            {
                WindowUiTree* tree = findTree(raw.handle);
                if (!tree)
                    return;

                UiEventPayload payload = raw.pressed
                    ? UiEventPayload{ PointerDownEvent{ static_cast<float>(raw.x), static_cast<float>(raw.y), raw.button } }
                    : UiEventPayload{ PointerUpEvent{ static_cast<float>(raw.x), static_cast<float>(raw.y), raw.button } };

                UiEvent event{ raw.handle, kInvalidUiElementId, UiEventPhase::Target, false, std::move(payload) };
                publishUiEvent(*m_context, tree->dispatchEvent(event));
            });

        context.comm().registerEvent<dsbox::MouseMotionEvent>("window.mouse_motion",
            [this](dsbox::MouseMotionEvent raw)
            {
                WindowUiTree* tree = findTree(raw.handle);
                if (!tree)
                    return;

                UiEventPayload payload = PointerMoveEvent{
                    static_cast<float>(raw.x), static_cast<float>(raw.y),
                    static_cast<float>(raw.deltaX), static_cast<float>(raw.deltaY)
                };

                UiEvent event{ raw.handle, kInvalidUiElementId, UiEventPhase::Target, false, std::move(payload) };
                publishUiEvent(*m_context, tree->dispatchEvent(event));
            });

        context.comm().registerEvent<dsbox::MouseWheelEvent>("window.mouse_wheel",
            [this](dsbox::MouseWheelEvent raw)
            {
                WindowUiTree* tree = findTree(raw.handle);
                if (!tree)
                    return;

                UiEvent event{
                    raw.handle, kInvalidUiElementId, UiEventPhase::Target, false,
                    PointerWheelEvent{ raw.scrollX, raw.scrollY }
                };
                publishUiEvent(*m_context, tree->dispatchEvent(event));
            });

        context.comm().registerEvent<dsbox::TextInputEvent>("window.text_input",
            [this](dsbox::TextInputEvent raw)
            {
                WindowUiTree* tree = findTree(raw.handle);
                if (!tree)
                    return;

                UiEvent event{
                    raw.handle, kInvalidUiElementId, UiEventPhase::Target, false,
                    TextInputEvent{ std::move(raw.text) }
                };
                publishUiEvent(*m_context, tree->dispatchEvent(event));
            });

        context.comm().registerEvent<dsbox::TextEditingEvent>("window.text_editing",
            [this](dsbox::TextEditingEvent raw)
            {
                WindowUiTree* tree = findTree(raw.handle);
                if (!tree)
                    return;

                UiEvent event{
                    raw.handle, kInvalidUiElementId, UiEventPhase::Target, false,
                    TextEditingEvent{ std::move(raw.text), raw.cursor, raw.selectionLength }
                };
                publishUiEvent(*m_context, tree->dispatchEvent(event));
            });
    }

    void UiModule::onShutdown(dsbox::Context& /*context*/)
    {
        std::lock_guard<std::mutex> lock(m_treesMutex);
        m_trees.clear();
        m_context = nullptr;
    }

    WindowUiTree& UiModule::createTree(
        dsbox::WindowHandle handle,
        std::unique_ptr<IUiElement> root,
        dsbox::ITextInputProvider* textInput)
    {
        std::lock_guard<std::mutex> lock(m_treesMutex);

        auto tree = std::make_unique<WindowUiTree>(handle, std::move(root), textInput);
        WindowUiTree& ref = *tree;
        m_trees[handle] = std::move(tree);
        return ref;
    }

    void UiModule::destroyTree(dsbox::WindowHandle handle)
    {
        std::lock_guard<std::mutex> lock(m_treesMutex);
        m_trees.erase(handle);
    }

    WindowUiTree* UiModule::findTree(dsbox::WindowHandle handle)
    {
        std::lock_guard<std::mutex> lock(m_treesMutex);
        auto it = m_trees.find(handle);
        return it != m_trees.end() ? it->second.get() : nullptr;
    }
}
