#pragma once

#include <dsbox_ui/api.hpp>
#include <dsbox_ui/element.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <dsbox/context.hpp>
#include <dsbox/window_provider.hpp>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dsbox_ui
{
    // Mints UiElementIds that are Unique for the whole lifetime of
    // whatever WindowUiTree a panel ends up part of (see
    // UiElementIdMode::Unique, element.hpp) -- never reused, even
    // across a panel being torn down and a different one built in its
    // place. A host passes one of these into every PanelBuildContext
    // (see below) rather than asking each panel factory to invent its
    // own ids, so two independently written panels -- quite possibly
    // compiled into different shared libraries that have never seen
    // each other's code -- can never collide on an id.
    //
    // A trivial, thread-safe (std::atomic) implementation is provided
    // below (AtomicPanelIdAllocator) for a host that has no reason to
    // do anything fancier; this is an interface rather than a concrete
    // type only so a host CAN do something fancier (e.g. persist the
    // counter across a panel-switching rebuild for debugging) without
    // dsbox_ui needing to know about it.
    class DSBOX_UI_API IPanelIdAllocator
    {
    public:
        virtual ~IPanelIdAllocator() = default;
        virtual UiElementId nextId() = 0;
    };

    // Simple monotonic implementation of IPanelIdAllocator -- safe to
    // call nextId() from any thread. This is what most hosts want;
    // construct one, keep it alive for as long as the host window
    // itself, and hand a reference to it into every PanelBuildContext.
    class DSBOX_UI_API AtomicPanelIdAllocator final : public IPanelIdAllocator
    {
    public:
        // Starts at `first` (default 1 -- 0 is kInvalidUiElementId,
        // see event.hpp). Pass something past whatever fixed ids a
        // host's own chrome (sidebar, content area, root) already uses
        // by hand, if it uses any, so a panel's own ids can never
        // collide with the host's.
        explicit AtomicPanelIdAllocator(UiElementId first = 1) noexcept
            : m_next(first)
        {
        }

        UiElementId nextId() override
        {
            return m_next.fetch_add(1, std::memory_order_relaxed);
        }

    private:
        std::atomic<UiElementId> m_next;
    };

    // What a PanelFactory is handed to build its own content. Bundles
    // exactly what a panel typically needs and nothing it shouldn't
    // have to reach for on its own:
    //
    //   ids      - see IPanelIdAllocator above. Every element the
    //              panel constructs should get its id from here.
    //   backend  - the real IUiRenderBackend this window's tree is
    //              (and will keep being) ticked against, for anything
    //              the panel needs to do once, up front -- loadFont()/
    //              loadFontOrFallback() (render_backend.hpp),
    //              registerImage(), etc. Valid for as long as the
    //              host window itself is open; a panel may cache a
    //              FontHandle/ImageHandle it gets back across its own
    //              rebuilds (switching away and back) without calling
    //              loadFont()/registerImage() again, PROVIDED it reads
    //              that cached handle from somewhere that outlives one
    //              build() call -- e.g. a member of the module that
    //              registered the panel, not a local in the factory
    //              lambda.
    //   context  - the dsbox::Context the host application itself was
    //              constructed with. A panel reaches whatever it needs
    //              through here exactly the way any other piece of
    //              DSBox code would: context.get<SomeService>() for a
    //              published service, context.modules()->find("...")
    //              for another module's own public API (the way the
    //              built-in ADB panels in dsbox_adb_tool reach
    //              AdbModule, for instance).
    //   window   - the dsbox::WindowHandle this panel's content is
    //              being built for. Most panels have no use for this
    //              (there is only ever one host window in practice
    //              today) but it's threaded through so a future host
    //              that manages several windows doesn't need a
    //              breaking change to PanelBuildContext to support it.
    struct PanelBuildContext
    {
        IPanelIdAllocator& ids;
        IUiRenderBackend& backend;
        dsbox::Context& context;
        dsbox::WindowHandle window = dsbox::kInvalidWindowHandle;
    };

    // What a PanelFactory hands back. `root` becomes the panel's own
    // mounted subtree (typically a Container -- see container.hpp --
    // but any IUiElement works); `onDestroy`, if set, is called
    // exactly once, right before `root` itself is torn down (a host
    // switching away from this panel, or closing the window
    // altogether) -- for a panel that needs to undo something it set
    // up in its own factory call beyond what destroying its element
    // tree already handles (e.g. unsubscribing a
    // dsbox::CommunicationManager event it registered with Anonymous
    // ownership rather than a module pointer -- see owner.hpp). Most
    // panels need nothing here and can leave it unset.
    struct BuiltPanel
    {
        std::unique_ptr<IUiElement> root;
        std::function<void()> onDestroy;
    };

    // Builds one panel's content on demand. Called by the host, never
    // by dsbox_ui itself -- see PanelRegistry's own class comment for
    // when a reasonable host calls this (typically: once, the first
    // time the panel becomes the active one; a host MAY call it again
    // on every switch instead, trading rebuild cost for not having to
    // keep a built-but-inactive panel's state around -- either is a
    // legitimate choice PanelRegistry has no opinion on).
    using PanelFactory = std::function<BuiltPanel(PanelBuildContext&)>;

    // One contribution offered to a PanelRegistry.
    struct PanelDescriptor
    {
        // Stable, unique key, e.g. "adb.devices" -- not shown to the
        // person using the app, only used for registerPanel()'s own
        // uniqueness check, unregisterPanel(), and a host remembering
        // which panel was last active. Namespacing it with the
        // contributing module's own name (dsbox::IModule::name(), or
        // a short abbreviation of it) is the easiest way to guarantee
        // two independently written panels never collide here, the
        // same convention dsbox::CommunicationManager's own bus topic
        // names already use throughout this codebase (e.g.
        // "adb.devices.changed").
        std::string id;

        // Shown to the person using the app -- a host typically prints
        // this verbatim as the panel's label in whatever switcher UI
        // (a sidebar, a row of tabs, ...) it builds from
        // PanelRegistry::list().
        std::string title;

        PanelFactory factory;
    };

    // Published into a dsbox::Context by whichever piece of an
    // application hosts a panel switcher -- a sidebar of buttons, a
    // row of tabs, anything that shows several independently built
    // pieces of UI and lets the person switch between them. Any OTHER
    // module can then contribute a panel from its own code via
    // context.get<PanelRegistry>()->registerPanel(...) -- the exact
    // same "small shared service published into Context" pattern
    // dsbox_cli::CommandRegistry already uses for CLI commands
    // (command_registry.hpp), extended to GUI panels. A module
    // contributing a panel needs no dependency on whatever concrete
    // host built the switcher, no dependency on dsbox_sdl/dsbox_vulkan/
    // dsbox_vulkan_gui, and -- critically for the "external module"
    // case -- does not even need to have existed when the host window
    // was first built: a module loaded later via
    // dsbox::ModuleManager::loadDynamic() can register a panel from
    // its own onStart() just as well as a module that was linked in
    // from the start, and a host that subscribes via onChanged() (see
    // below) picks it up without a restart.
    //
    // dsbox_ui itself has no opinion on what a panel's content looks
    // like, how many a host shows at once, or how it arranges them --
    // this class only solves discovery (where a panel can announce
    // itself, where a host can find out what's been announced) and a
    // lightweight "something changed" notification. Building an actual
    // application host around this (a window, a sidebar, a content
    // area that swaps in the active panel's root) is application code,
    // not part of dsbox_ui -- see dsbox_adb_tool's own main.cpp for a
    // worked example, including its own built-in panels registered
    // through this exact same API.
    //
    // Thread safety: every method is safe to call concurrently from
    // multiple threads (own mutex, entirely separate from
    // CommunicationManager's) -- same convention as CommandRegistry.
    // registerPanel()/unregisterPanel() callers may be running on any
    // thread (a dynamically loaded module's onStart() runs on whatever
    // thread called ModuleManager::loadDynamic()/startAll(), not
    // necessarily a window's own control thread) -- onChanged()
    // callbacks are invoked synchronously from inside whichever
    // register/unregister call triggered them, so a callback that
    // itself needs to touch a live WindowUiTree must not do so
    // directly (that tree is not this class's to synchronize) -- it
    // should instead just record "a rebuild is needed" (e.g. set an
    // std::atomic<bool>) for whatever thread actually owns that tree
    // to notice and act on, the same way dsbox_adb_tool's own host
    // code does.
    class DSBOX_UI_API PanelRegistry
    {
    public:
        // Throws std::runtime_error if `descriptor.id` is already
        // registered -- same rejection convention
        // dsbox::CommunicationManager::registerHandler uses for a
        // duplicate name.
        void registerPanel(PanelDescriptor descriptor)
        {
            std::vector<std::function<void()>> callbacksToRun;

            {
                std::lock_guard<std::mutex> lock(m_mutex);

                const bool duplicate = std::any_of(
                    m_panels.begin(), m_panels.end(),
                    [&](const PanelDescriptor& existing) { return existing.id == descriptor.id; });

                if (duplicate)
                    throw std::runtime_error(
                        "PanelRegistry: panel id '" + descriptor.id + "' is already registered");

                m_panels.push_back(std::move(descriptor));
                callbacksToRun = snapshotCallbacks();
            }

            runCallbacks(callbacksToRun);
        }

        // Best-effort removal. A module contributing a panel and
        // capable of being unloaded later (any dynamically loaded
        // module -- see dsbox::ModuleManager::unload()) should call
        // this from its own onStop() so a host never ends up holding a
        // PanelDescriptor whose factory closure points at code about
        // to be unmapped. No-op if `id` isn't currently registered.
        void unregisterPanel(std::string_view id)
        {
            std::vector<std::function<void()>> callbacksToRun;

            {
                std::lock_guard<std::mutex> lock(m_mutex);

                const auto it = std::find_if(
                    m_panels.begin(), m_panels.end(),
                    [&](const PanelDescriptor& existing) { return existing.id == id; });

                if (it == m_panels.end())
                    return;

                m_panels.erase(it);
                callbacksToRun = snapshotCallbacks();
            }

            runCallbacks(callbacksToRun);
        }

        // Snapshot, in registration order -- the order a host's own
        // panel switcher is expected to show them in, absent some
        // other ordering it wants to impose itself (e.g. alphabetical
        // by title). PanelDescriptor/PanelFactory are both copyable
        // (std::function underneath), so this is a genuine, independent
        // copy -- safe to keep around and iterate after releasing
        // whatever lock the caller might otherwise have worried about.
        std::vector<PanelDescriptor> list() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_panels;
        }

        // Registers `callback` to be invoked (synchronously, from
        // inside whichever thread called registerPanel()/
        // unregisterPanel() -- see this class's own threading note
        // above) every time the panel list changes after this call.
        // Returns an id accepted by removeOnChanged(). This is a
        // small dedicated callback list rather than something
        // threaded through dsbox::CommunicationManager, deliberately:
        // PanelRegistry has no CommunicationManager (or even
        // dsbox::Context) dependency of its own, so it stays usable
        // with nothing but a default-constructed instance -- useful
        // for a unit test, or an application with no dsbox::Context at
        // all.
        std::size_t onChanged(std::function<void()> callback)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const std::size_t id = m_nextCallbackId++;
            m_callbacks.push_back({ id, std::move(callback) });
            return id;
        }

        // No-op if `id` is unknown (already removed, or never valid).
        void removeOnChanged(std::size_t id)
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            m_callbacks.erase(
                std::remove_if(
                    m_callbacks.begin(), m_callbacks.end(),
                    [&](const auto& entry) { return entry.first == id; }),
                m_callbacks.end());
        }

    private:
        // Copies the current callbacks while m_mutex is held, so they
        // can be invoked AFTER it's released -- a callback is
        // arbitrary user code (see this class's own threading note)
        // and must never run while this registry's own lock is held,
        // the same reasoning dsbox::CommunicationManager applies to
        // its own debug-event notifications.
        std::vector<std::function<void()>> snapshotCallbacks() const
        {
            std::vector<std::function<void()>> result;
            result.reserve(m_callbacks.size());

            for (const auto& entry : m_callbacks)
                result.push_back(entry.second);

            return result;
        }

        static void runCallbacks(const std::vector<std::function<void()>>& callbacks) noexcept
        {
            for (const auto& callback : callbacks)
            {
                try
                {
                    if (callback)
                        callback();
                }
                catch (...)
                {
                    // A listener's own bug must never prevent
                    // registerPanel()/unregisterPanel() from
                    // completing for every OTHER listener, or from
                    // returning to its own caller -- same swallow-and-
                    // continue convention CommunicationManager's
                    // notifyDebug() uses.
                }
            }
        }

        mutable std::mutex m_mutex;
        std::vector<PanelDescriptor> m_panels;

        std::size_t m_nextCallbackId = 1;
        std::vector<std::pair<std::size_t, std::function<void()>>> m_callbacks;
    };
}
