#include "gui_mode.hpp"
#include "adb_format.hpp"
#include "launch_options.hpp"

#include <dsbox/context.hpp>
#include <dsbox/module_manager.hpp>

#include <dsbox_sdl/window_module.hpp>
#include <dsbox_vulkan/vulkan_module.hpp>
#include <dsbox_vulkan_gui/vulkan_ui_window_driver.hpp>

#include <dsbox_ui/button.hpp>
#include <dsbox_ui/container.hpp>
#include <dsbox_ui/list_view.hpp>
#include <dsbox_ui/panel.hpp>
#include <dsbox_ui/render_backend.hpp>
#include <dsbox_ui/scroll_view.hpp>
#include <dsbox_ui/stack_layout.hpp>
#include <dsbox_ui/text.hpp>
#include <dsbox_ui/text_area.hpp>
#include <dsbox_ui/text_field.hpp>
#include <dsbox_ui/window_ui_tree.hpp>

#include <dsbox_android_adb/adb_module.hpp>
#include <dsbox_android_adb/adb_packages.hpp>
#include <dsbox_android_adb/device.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dsbox_adb_tool
{
    using dsbox_android_adb::AdbModule;
    using dsbox_android_adb::AdbDeviceInfo;
    using dsbox_android_adb::DevicesChangedEvent;
    using dsbox_android_adb::LogcatLineEvent;
    using dsbox_android_adb::LogcatStoppedEvent;

    namespace
    {
        // Defers arbitrary callables onto whichever thread drains it.
        // Needed because neither dsbox_ui::Container's structural
        // mutators (addChild/clearChildren) nor a leaf element's own
        // setters are synchronized against a concurrent
        // WindowUiTree::tick() (see Container::clearChildren()'s own
        // doc) -- but a button's onClick fires from inside
        // dispatchEvent(), which this application's WindowModule calls
        // back on from ITS OWN control thread, while tick() here runs
        // on each window's OWN render thread (see openWindow()'s render
        // loop below). Every place in this file that wants to mutate a
        // live tree from a callback that might run on the wrong thread
        // pushes a task into that window's own queue instead of
        // mutating directly; the render loop drains it once per frame,
        // immediately before ticking.
        class UiTaskQueue
        {
        public:
            void push(std::function<void()> task)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_tasks.push_back(std::move(task));
            }

            void drainAndRun()
            {
                std::vector<std::function<void()>> tasks;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    tasks.swap(m_tasks);
                }

                for (auto& task : tasks)
                {
                    if (task)
                        task();
                }
            }

        private:
            std::mutex m_mutex;
            std::vector<std::function<void()>> m_tasks;
        };

        struct DeviceWindow;

        // Application-wide state: one GuiHost per process, shared by
        // every window this tool has open. Holds everything that isn't
        // specific to a single window -- the modules, the PanelRegistry
        // published into `context` (so a statically linked OR
        // dynamically loaded module can contribute a panel the exact
        // same way regardless of how many windows exist -- see
        // panel.hpp's own class comment), and the list of currently
        // open windows themselves.
        //
        // `windows` is mutated from exactly one thread (runGui()'s own
        // supervisor loop, see its own comment below, which is also the
        // only thread that ever calls
        // dsbox_sdl::WindowModule::createWindow()/closeWindow()), but
        // READ from others (a button's onClick handler calling
        // findWindow(), the global logcat/"window.closed" event
        // subscriptions below) -- `windowsMutex` guards it for that.
        struct GuiHost
        {
            dsbox::Context& context;
            dsbox::ModuleManager& modules;
            AdbModule& adb;

            dsbox_sdl::WindowModule* windowModule = nullptr;
            dsbox_vulkan::VulkanModule* vulkanModule = nullptr;
            dsbox_ui::UiModule* uiModule = nullptr;
            bool uiOwnedHere = false;

            std::shared_ptr<dsbox_ui::PanelRegistry> panelRegistry;
            std::size_t panelsChangedSubscription = 0;

            std::mutex windowsMutex;
            std::vector<std::unique_ptr<DeviceWindow>> windows;

            // Serials queued by the Devices panel's "Open in new
            // window" button (see buildDevicesPanel()), drained once
            // per supervisor-loop iteration in runGui() -- window
            // creation itself only ever happens on that loop's own
            // thread, never directly inside a button's onClick (which
            // fires on dsbox_sdl::WindowModule's own control thread --
            // see UiTaskQueue's own comment above for why mixing
            // threads here needs this kind of care).
            std::mutex pendingMutex;
            std::vector<std::string> pendingOpenRequests;
        };

        // All state for exactly ONE of this tool's windows: the
        // window/surface/driver/tree quadruple, this window's own panel
        // id allocator and task queue, which panel is currently active
        // and how to tear it down, and -- new in this revision -- the
        // single device serial this window is "pointed at" (see
        // selectedSerial below), shared by every panel this window
        // shows instead of each panel keeping its own typed-in serial.
        struct DeviceWindow
        {
            GuiHost& host;

            dsbox::WindowHandle window = dsbox::kInvalidWindowHandle;
            dsbox_vulkan::SurfaceHandle surface = dsbox_vulkan::kInvalidSurfaceHandle;
            std::unique_ptr<dsbox_vulkan_gui::VulkanUiWindowDriver> driver;
            dsbox_ui::WindowUiTree* tree = nullptr;

            // Loaded once per window, against THIS window's own
            // VulkanUiRenderer (driver->renderBackend()) -- a font
            // loaded for one window's backend is not valid against
            // another's, so this can no longer live on GuiHost the way
            // it did when there was only ever one window.
            dsbox_ui::FontHandle font = dsbox_ui::kInvalidFontHandle;

            dsbox_ui::AtomicPanelIdAllocator panelIds{ 1000 };
            UiTaskQueue uiTasks;
            std::atomic<bool> closed{ false };
            std::thread renderThread;

            dsbox_ui::Container* sidebar = nullptr;
            dsbox_ui::Container* contentArea = nullptr;

            std::string activePanelId;
            std::function<void()> activeOnDestroy;

            // The device this window currently acts on. Set only by
            // the Devices panel (now a device PICKER -- see
            // buildDevicesPanel()) when a row is clicked; read by every
            // other built-in panel below, live, right when one of
            // their own buttons fires (requireSelectedDevice()), rather
            // than each panel keeping its own "device serial" TextField
            // the way earlier revisions of this file did. Guarded by
            // its own small mutex since it's written from a window's
            // render thread (inside a pushed uiTask, see selectDevice())
            // but read from dsbox_sdl::WindowModule's own control
            // thread (inside a button's onClick, before that click's
            // own work gets handed off to runAdbCall()).
            mutable std::mutex selectedMutex;
            std::string selectedSerial;

            std::string selectedSerialSnapshot() const
            {
                std::lock_guard<std::mutex> lock(selectedMutex);
                return selectedSerial;
            }

            void setSelectedSerial(std::string serial)
            {
                std::lock_guard<std::mutex> lock(selectedMutex);
                selectedSerial = std::move(serial);
            }

            // Logcat panel state for THIS window. `logcatSerial` is the
            // serial Start was last clicked for in this window (not
            // necessarily still equal to `selectedSerial`, if the
            // person moved on to a different device in the Devices
            // panel without stopping logcat first) -- appendLogLine()
            // only appends a line here if it matches, so two windows
            // watching two different devices' logcat never bleed into
            // each other's buffer, and a window's own buffer doesn't
            // suddenly fill with a DIFFERENT device's output just
            // because the person re-picked a device in this window
            // without pressing Start again.
            //
            // `logcatOutput` is a TextArea (not a single-line Text) --
            // logcat output is inherently multi-line, and Text (see its
            // own "v1 layout limitation" doc, text.hpp) only ever shapes
            // and paints whatever string it's given as ONE line, so a
            // multi-line buffer handed to a Text just ran every line
            // together visually. TextArea understands '\n' as a hard
            // line break and wraps besides, and is wrapped in a
            // ScrollView (see addScrollableOutput()) so a buffer longer
            // than the panel can still be scrolled through.
            std::mutex logMutex;
            std::string logBuffer;
            std::string logcatSerial;
            dsbox_ui::TextArea* logcatOutput = nullptr;

            static constexpr dsbox_ui::UiElementId kRootId = 1;
            static constexpr dsbox_ui::UiElementId kSidebarId = 2;
            static constexpr dsbox_ui::UiElementId kContentAreaId = 3;

            explicit DeviceWindow(GuiHost& h) : host(h)
            {
            }
        };

        DeviceWindow* findWindow(GuiHost& host, dsbox::WindowHandle handle)
        {
            std::lock_guard<std::mutex> lock(host.windowsMutex);

            for (auto& w : host.windows)
                if (w->window == handle)
                    return w.get();

            return nullptr;
        }

        // Runs `fn(window)` for every currently open window, with
        // `windowsMutex` held for the duration -- used by the handful
        // of subscriptions in runGui() that are global (one per
        // process, not one per window) and need to reach whichever
        // window(s) care: the logcat events (a line might belong to
        // more than one window's logcatSerial, in principle, if two
        // windows somehow started logcat for the same device) and the
        // panel-list-changed callback (every window's sidebar needs
        // rebuilding, not just one).
        template<typename Fn>
        void forEachWindow(GuiHost& host, Fn fn)
        {
            std::lock_guard<std::mutex> lock(host.windowsMutex);

            for (auto& w : host.windows)
                fn(*w);
        }

        void requestOpenWindow(GuiHost& host, std::string initialSerial)
        {
            std::lock_guard<std::mutex> lock(host.pendingMutex);
            host.pendingOpenRequests.push_back(std::move(initialSerial));
        }

        void applyFont(DeviceWindow& win, dsbox_ui::Text& text)
        {
            if (win.font != dsbox_ui::kInvalidFontHandle)
                text.setFont(win.font);
        }

        void applyFont(DeviceWindow& win, dsbox_ui::TextField& field)
        {
            if (win.font != dsbox_ui::kInvalidFontHandle)
                field.setFont(win.font);
        }

        void applyFont(DeviceWindow& win, dsbox_ui::TextArea& area)
        {
            if (win.font != dsbox_ui::kInvalidFontHandle)
                area.setFont(win.font);
        }

        void applyFont(DeviceWindow& win, dsbox_ui::ListView& list)
        {
            if (win.font != dsbox_ui::kInvalidFontHandle)
                list.setFont(win.font);
        }

        // Every button built in this file goes through here: white
        // label text (the dark chrome below would otherwise swallow
        // whatever default color Text/makeButton assumes) plus this
        // window's own font.
        dsbox_ui::ButtonParts makeHostButton(DeviceWindow& win, std::string label)
        {
            dsbox_ui::ButtonParts button = dsbox_ui::makeButton(
                win.panelIds.nextId(), win.panelIds.nextId(), std::move(label), dsbox_ui::UiElementIdMode::Unique);
            button.label->setColor(dsbox_ui::UiColor{ 1.0f, 1.0f, 1.0f, 1.0f });
            applyFont(win, *button.label);
            return button;
        }

        void switchPanel(DeviceWindow& win, const std::string& id);

        void rebuildSidebar(DeviceWindow& win)
        {
            if (!win.sidebar)
                return;

            win.sidebar->clearChildren();

            for (const auto& descriptor : win.host.panelRegistry->list())
            {
                dsbox_ui::ButtonParts button = makeHostButton(win, descriptor.title);
                button.root->setPreferredWidth(dsbox_ui::UiLength::percent(100.0f));

                std::string panelId = descriptor.id;
                button.root->setOnClick(
                    [&win, panelId] { win.uiTasks.push([&win, panelId] { switchPanel(win, panelId); }); });

                win.sidebar->addChild(std::move(button.root));
            }

            win.tree->markDirty(DeviceWindow::kSidebarId);
        }

        // Tears down whichever panel is currently mounted in `win`
        // (calling its onDestroy, if any) and mounts `id` in its
        // place. Must only be called from the thread that owns `win`'s
        // tree -- see UiTaskQueue's own comment above; every caller in
        // this file reaches this through a pushed task.
        void switchPanel(DeviceWindow& win, const std::string& id)
        {
            if (!win.contentArea || !win.tree || !win.driver)
                return;

            const auto descriptors = win.host.panelRegistry->list();
            const auto it = std::find_if(descriptors.begin(), descriptors.end(),
                [&](const dsbox_ui::PanelDescriptor& d) { return d.id == id; });

            if (it == descriptors.end())
                return;

            if (win.activeOnDestroy)
            {
                win.activeOnDestroy();
                win.activeOnDestroy = nullptr;
            }

            win.logcatOutput = nullptr;
            win.contentArea->clearChildren();

            dsbox_ui::PanelBuildContext buildContext{
                win.panelIds, win.driver->renderBackend(), win.host.context, win.window };

            dsbox_ui::BuiltPanel built = it->factory(buildContext);
            win.activeOnDestroy = std::move(built.onDestroy);
            win.activePanelId = id;

            if (built.root)
                win.contentArea->addChild(std::move(built.root));

            win.tree->markDirty(DeviceWindow::kContentAreaId);
        }

        std::unique_ptr<dsbox_ui::IUiElement> buildChrome(DeviceWindow& win)
        {
            using namespace dsbox_ui;

            auto root = std::make_unique<Container>(DeviceWindow::kRootId, UiElementIdMode::Unique, "root");
            root->setBackgroundColor(UiColor{ 0.09f, 0.09f, 0.11f, 1.0f });
            root->setPreferredWidth(UiLength::percent(100.0f));
            root->setPreferredHeight(UiLength::percent(100.0f));
            root->setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Horizontal, 0.0f));

            auto& sidebar = static_cast<Container&>(root->addChild(
                std::make_unique<Container>(DeviceWindow::kSidebarId, UiElementIdMode::Unique, "sidebar")));
            sidebar.setBackgroundColor(UiColor{ 0.13f, 0.13f, 0.16f, 1.0f });
            sidebar.setPreferredWidth(UiLength::pixels(200.0f));
            sidebar.setPreferredHeight(UiLength::percent(100.0f));
            sidebar.setPadding(12.0f, 12.0f, 12.0f, 12.0f);
            sidebar.setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 8.0f));

            auto& contentArea = static_cast<Container&>(root->addChild(
                std::make_unique<Container>(DeviceWindow::kContentAreaId, UiElementIdMode::Unique, "contentArea")));
            contentArea.setBackgroundColor(UiColor{ 0.16f, 0.16f, 0.19f, 1.0f });
            contentArea.setPreferredWidth(UiLength::fraction(1.0f));
            contentArea.setPreferredHeight(UiLength::percent(100.0f));
            contentArea.setPadding(16.0f, 16.0f, 16.0f, 16.0f);
            contentArea.setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 12.0f));

            win.sidebar = &sidebar;
            win.contentArea = &contentArea;

            return root;
        }

        // A labeled text field, dropped into a horizontal row --
        // shared shape every panel below uses for its inputs.
        dsbox_ui::TextField& addField(
            DeviceWindow& win, dsbox_ui::Container& row, std::string placeholder, float width)
        {
            using namespace dsbox_ui;

            auto& field = static_cast<TextField&>(
                row.addChild(std::make_unique<TextField>(win.panelIds.nextId(), UiElementIdMode::Unique, "field")));
            field.setPreferredWidth(UiLength::pixels(width));
            field.setPlaceholder(std::move(placeholder));
            applyFont(win, field);
            return field;
        }

        // Wraps `field` in a flex-item Container occupying whatever's
        // left of the row's main axis -- see this function's own
        // comment in earlier revisions of this file for why the
        // fraction has to go on the WRAPPING Container rather than the
        // TextField itself (TextField does not implement IFlexItem).
        dsbox_ui::TextField& addFlexibleField(DeviceWindow& win, dsbox_ui::Container& row, std::string placeholder)
        {
            using namespace dsbox_ui;

            auto& wrapper = static_cast<Container&>(row.addChild(
                std::make_unique<Container>(win.panelIds.nextId(), UiElementIdMode::Unique, "fieldWrap")));
            wrapper.setPreferredWidth(UiLength::fraction(1.0f));
            wrapper.setPreferredHeight(UiLength::percent(100.0f));
            wrapper.setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Horizontal, 0.0f));

            auto& field = static_cast<TextField&>(wrapper.addChild(
                std::make_unique<TextField>(win.panelIds.nextId(), UiElementIdMode::Unique, "field")));
            field.setPreferredWidth(UiLength::percent(100.0f));
            field.setPlaceholder(std::move(placeholder));
            applyFont(win, field);
            return field;
        }

        dsbox_ui::Container& addRow(DeviceWindow& win, dsbox_ui::Container& root)
        {
            using namespace dsbox_ui;

            auto& row = static_cast<Container&>(
                root.addChild(std::make_unique<Container>(win.panelIds.nextId(), UiElementIdMode::Unique, "row")));
            row.setPreferredWidth(UiLength::percent(100.0f));
            row.setPreferredHeight(UiLength::pixels(40.0f));
            row.setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Horizontal, 8.0f, CrossAlign::Center));
            return row;
        }

        dsbox_ui::Text& addOutput(DeviceWindow& win, dsbox_ui::Container& root)
        {
            using namespace dsbox_ui;

            auto& output = static_cast<Text&>(
                root.addChild(std::make_unique<Text>(win.panelIds.nextId(), UiElementIdMode::Unique, "output")));
            output.setColor(UiColor{ 0.88f, 0.88f, 0.88f, 1.0f });
            applyFont(win, output);
            return output;
        }

        // The multi-line counterpart to addOutput() above: a
        // ScrollView wrapping a read-only TextArea, for any output that
        // may run to more than one line (package listings, logcat).
        // addOutput()'s Text is single-line only -- see Text's own "v1
        // layout limitation" (text.hpp) -- so handing it a string with
        // embedded '\n's just shapes and paints the WHOLE thing as one
        // run, which visually runs every line together. TextArea
        // understands '\n' as a hard break and additionally soft-wraps,
        // but (same as Container -- see its own "v1 sizing limitation")
        // does not size itself to its own content on a Content-sized
        // axis, so this gives it a generous fixed pixel height instead
        // and lets the enclosing ScrollView do the actual scrolling --
        // see ScrollView's own class comment for why that combination
        // works (the child is measured once against an unbounded
        // constraint on the scrollable axis).
        constexpr float kScrollableOutputHeight = 20000.0f;

        dsbox_ui::TextArea& addScrollableOutput(DeviceWindow& win, dsbox_ui::Container& root)
        {
            using namespace dsbox_ui;

            auto& scroll = static_cast<ScrollView&>(root.addChild(
                std::make_unique<ScrollView>(win.panelIds.nextId(), UiElementIdMode::Unique, "outputScroll")));
            scroll.setPreferredWidth(UiLength::percent(100.0f));
            scroll.setPreferredHeight(UiLength::fraction(1.0f));
            scroll.setVerticalScrollEnabled(true);
            scroll.setHorizontalScrollEnabled(false);
            scroll.setBackgroundColor(UiColor{ 0.0f, 0.0f, 0.0f, 0.0f });
            scroll.setFocusable(true);

            auto textOwned =
                std::make_unique<TextArea>(win.panelIds.nextId(), UiElementIdMode::Unique, "output");
            TextArea& text = *textOwned;
            text.setEnabled(false); // read-only: not a focus/edit target, just a scrollable display
            text.setTextColor(UiColor{ 0.88f, 0.88f, 0.88f, 1.0f });
            text.setBackgroundColor(UiColor{ 0.0f, 0.0f, 0.0f, 0.0f });
            text.setBorderColor(UiColor{ 0.0f, 0.0f, 0.0f, 0.0f });
            text.setFocusedBorderColor(UiColor{ 0.0f, 0.0f, 0.0f, 0.0f });
            text.setPreferredWidth(UiLength::percent(100.0f));
            text.setPreferredHeight(UiLength::pixels(kScrollableOutputHeight));
            applyFont(win, text);

            scroll.setChild(std::move(textOwned));

            return text;
        }

        // A single read-only line at the top of a panel reflecting
        // win.selectedSerial at the moment this panel was built --
        // Shell/Packages/Logcat all open with this instead of their own
        // "device serial" field now (see DeviceWindow::
        // selectedSerial's own comment). Since only one panel is ever
        // mounted per window and switching to the Devices panel
        // replaces the content area entirely, this can't go stale
        // without the panel itself being rebuilt (switchPanel() calls
        // the factory fresh every time) -- no live refresh needed here.
        dsbox_ui::Text& addSelectedDeviceHeader(DeviceWindow& win, dsbox_ui::Container& root)
        {
            dsbox_ui::Text& label = addOutput(win, root);
            const std::string serial = win.selectedSerialSnapshot();
            label.setText(serial.empty()
                ? std::string("No device selected -- pick one in the Devices panel.\n")
                : "Device: " + serial + "\n");
            return label;
        }

        // Reads win.selectedSerial and, if nothing is picked yet,
        // pushes a task pointing `output` back at the Devices panel and
        // returns an empty string -- every button below calls this
        // first instead of trusting a per-panel serial field that no
        // longer exists. Safe to call from dsbox_sdl::WindowModule's
        // own control thread (where onClick fires) since it only reads
        // the mutex-guarded serial and, in the failure case, pushes a
        // task rather than touching the tree directly.
        //
        // Templated on the output element type so it works for both
        // addOutput()'s single-line Text and addScrollableOutput()'s
        // multi-line TextArea -- both expose setText(std::string) and
        // id(), which is all this needs.
        template<typename OutputT>
        std::string requireSelectedDevice(DeviceWindow& win, OutputT* output)
        {
            std::string serial = win.selectedSerialSnapshot();
            if (!serial.empty())
                return serial;

            win.uiTasks.push(
                [&win, output]
                {
                    output->setText("No device selected -- pick one in the Devices panel.\n");
                    win.tree->markDirty(output->id());
                });

            return {};
        }

        // Runs `work` (expected to call into AdbModule, which blocks on
        // a subprocess) on a detached thread, then hands the resulting
        // string back to `output` through `win`'s own UiTaskQueue -- so
        // a slow adb call never stalls that window's render loop, and
        // its tree is only ever touched from the thread that owns it.
        // Templated for the same reason requireSelectedDevice() is.
        template<typename OutputT>
        void runAdbCall(DeviceWindow& win, OutputT* output, std::function<std::string()> work)
        {
            std::thread([&win, output, work = std::move(work)]
            {
                std::string result;
                try
                {
                    result = work();
                }
                catch (const std::exception& error)
                {
                    result = std::string("failed: ") + error.what() + "\n";
                }

                win.uiTasks.push([&win, output, result]
                {
                    output->setText(result);
                    win.tree->markDirty(output->id());
                });
            }).detach();
        }

        // Sets win.selectedSerial and, if the Devices panel is still
        // the one mounted, rebuilds it so its own row highlighting
        // reflects the new pick -- called from inside a pushed uiTask
        // (see buildDevicesPanel()'s row onClick below), so it's safe
        // to call switchPanel() directly here rather than pushing
        // another task.
        void selectDevice(DeviceWindow& win, const std::string& serial)
        {
            win.setSelectedSerial(serial);

            if (win.activePanelId == "adb.devices")
                switchPanel(win, "adb.devices");
        }

        // Rebuilds the clickable device list inside `listArea` -- one
        // row per device, the currently selected serial highlighted.
        // This is what makes the Devices panel a PICKER rather than a
        // plain read-only table: clicking a row is the only way
        // win.selectedSerial ever changes.
        void rebuildDeviceList(
            DeviceWindow& win, dsbox_ui::Container& listArea, const std::vector<AdbDeviceInfo>& devices)
        {
            using namespace dsbox_ui;

            listArea.clearChildren();

            const std::string selected = win.selectedSerialSnapshot();

            for (const auto& device : devices)
            {
                ButtonParts row = makeHostButton(win, formatDeviceLine(device));
                row.root->setPreferredWidth(UiLength::percent(100.0f));
                row.root->setPreferredHeight(UiLength::pixels(36.0f));

                const bool isSelected = (device.serial == selected);
                row.root->setBackgroundColor(isSelected
                    ? UiColor{ 0.20f, 0.45f, 0.85f, 1.0f }
                    : UiColor{ 0.20f, 0.20f, 0.24f, 1.0f });

                std::string serial = device.serial;
                row.root->setOnClick(
                    [&win, serial] { win.uiTasks.push([&win, serial] { selectDevice(win, serial); }); });

                listArea.addChild(std::move(row.root));
            }

            win.tree->markDirty(listArea.id());
        }

        // Runs `adb devices -l` on a detached thread (same reasoning as
        // runAdbCall(), but this one needs the parsed list back, not
        // just a formatted string, to rebuild the picker's rows) and
        // reflects the result into both `listArea` and `status`.
        void refreshDevices(DeviceWindow& win, dsbox_ui::Container* listArea, dsbox_ui::Text* status)
        {
            std::thread([&win, listArea, status]
            {
                std::vector<AdbDeviceInfo> devices;
                std::string error;

                try
                {
                    devices = win.host.adb.refreshDevices();
                }
                catch (const std::exception& e)
                {
                    error = std::string("failed: ") + e.what() + "\n";
                }

                win.uiTasks.push([&win, listArea, status, devices, error]
                {
                    if (listArea)
                        rebuildDeviceList(win, *listArea, devices);

                    if (status)
                    {
                        status->setText(!error.empty()
                            ? error
                            : devices.empty()
                                ? std::string("(no devices found)\n")
                                : std::to_string(devices.size()) + " device(s) -- click one to select it for this window.\n");
                        win.tree->markDirty(status->id());
                    }
                });
            }).detach();
        }

        dsbox_ui::BuiltPanel buildDevicesPanel(DeviceWindow& win, dsbox_ui::PanelBuildContext& ctx)
        {
            using namespace dsbox_ui;
            (void)ctx;

            auto root = std::make_unique<Container>(win.panelIds.nextId(), UiElementIdMode::Unique, "devicesPanel");
            root->setPreferredWidth(UiLength::percent(100.0f));
            root->setPreferredHeight(UiLength::percent(100.0f));
            root->setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 12.0f));

            Container& row = addRow(win, *root);

            ButtonParts refresh = makeHostButton(win, "Refresh");
            refresh.root->setPreferredWidth(UiLength::pixels(110.0f));

            ButtonParts openNew = makeHostButton(win, "Open in new window");
            openNew.root->setPreferredWidth(UiLength::pixels(190.0f));

            Text& status = addOutput(win, *root);
            Text* statusPtr = &status;

            auto& listArea = static_cast<Container&>(root->addChild(
                std::make_unique<Container>(win.panelIds.nextId(), UiElementIdMode::Unique, "deviceList")));
            listArea.setPreferredWidth(UiLength::percent(100.0f));
            listArea.setPreferredHeight(UiLength::fraction(1.0f));
            listArea.setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 4.0f));
            Container* listAreaPtr = &listArea;

            refresh.root->setOnClick([&win, listAreaPtr, statusPtr] { refreshDevices(win, listAreaPtr, statusPtr); });
            row.addChild(std::move(refresh.root));

            // Opens a new DeviceWindow for whatever is currently
            // selected in THIS window (possibly nothing -- the new
            // window just opens with no device picked, same as this
            // tool's very first window always has). The actual
            // createWindow() call happens later, on runGui()'s own
            // supervisor thread -- see requestOpenWindow()'s own
            // comment for why.
            openNew.root->setOnClick(
                [&win]
                {
                    std::string serial = win.selectedSerialSnapshot();
                    requestOpenWindow(win.host, std::move(serial));
                });
            row.addChild(std::move(openNew.root));

            // Populate once up front, same as opening the panel
            // already showing something instead of a blank area.
            refreshDevices(win, listAreaPtr, statusPtr);

            return BuiltPanel{ std::move(root), nullptr };
        }

        dsbox_ui::BuiltPanel buildShellPanel(DeviceWindow& win, dsbox_ui::PanelBuildContext& ctx)
        {
            using namespace dsbox_ui;
            (void)ctx;

            auto root = std::make_unique<Container>(win.panelIds.nextId(), UiElementIdMode::Unique, "shellPanel");
            root->setPreferredWidth(UiLength::percent(100.0f));
            root->setPreferredHeight(UiLength::percent(100.0f));
            root->setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 12.0f));

            addSelectedDeviceHeader(win, *root);

            Container& row = addRow(win, *root);
            TextField& cmd = addFlexibleField(win, row, "shell command");

            Text& output = addOutput(win, *root);
            Text* outputPtr = &output;
            TextField* cmdPtr = &cmd;

            ButtonParts run = makeHostButton(win, "Run");
            run.root->setPreferredWidth(UiLength::pixels(90.0f));
            run.root->setOnClick(
                [&win, outputPtr, cmdPtr]
                {
                    std::string serialValue = requireSelectedDevice(win, outputPtr);
                    if (serialValue.empty())
                        return;

                    std::string cmdValue = cmdPtr->text();
                    std::string warning = unknownSerialWarning(win.host.adb, serialValue);

                    runAdbCall(win, outputPtr, [&win, serialValue, cmdValue, warning]
                    {
                        std::string shellOutput = win.host.adb.device(serialValue).shell(cmdValue);
                        if (!shellOutput.empty() && shellOutput.back() != '\n')
                            shellOutput += '\n';
                        return warning + shellOutput;
                    });
                });
            row.addChild(std::move(run.root));

            return BuiltPanel{ std::move(root), nullptr };
        }

        // Shared, per-mount state for the Packages panel (see
        // buildPackagesPanel()). One instance is created per panel
        // mount and handed around as a shared_ptr to every async
        // callback and button closure the panel's two inner views
        // (list/detail, see showPackagesList()/showPackageDetail())
        // register, so it outlives whichever view happens to be
        // showing at a given moment.
        //
        // `body` is the Container both inner views render into --
        // switching between them means clearing and repopulating it,
        // NOT going through the host's own switchPanel() (that would
        // tear down the whole Packages panel, including the device
        // header above it, just to show package details). Set back to
        // nullptr by buildPackagesPanel()'s own BuiltPanel::onDestroy
        // the moment the WHOLE Packages panel is torn down (the person
        // switched to a different sidebar entry, or closed the
        // window) -- every function below checks this before touching
        // `body`, so a callback that was already queued at that moment
        // (an in-flight adb call, a click that was mid-dispatch) simply
        // becomes a no-op instead of touching a Container that no
        // longer exists.
        //
        // `generation` is bumped by showPackagesList()/showPackageDetail()
        // every time either view (re)builds `body`'s contents, and
        // captured by value into every async callback/button closure
        // those two functions register. A callback compares its own
        // captured value against generation.load() before touching any
        // UI element pointer it captured (listPtr/statusPtr/outputPtr)
        // -- if they no longer match, that pointer's own element has
        // since been destroyed by a LATER showPackagesList()/
        // showPackageDetail() call (the person clicked a different
        // package, or pressed Back, before this particular adb call
        // finished), and the callback bails without touching it.
        struct PackagesPanelState
        {
            dsbox_ui::Container* body = nullptr;
            std::vector<std::string> packageNames;
            std::string lastFilter;
            std::atomic<std::uint64_t> generation{ 0 };
        };

        // Forward declared: showPackagesList() registers a ListView
        // selection callback that calls showPackageDetail(), which in
        // turn (its own "Back" button) calls showPackagesList() --
        // only one direction of that mutual recursion can be textually
        // defined first.
        void showPackageDetail(
            DeviceWindow& win, std::shared_ptr<PackagesPanelState> state, std::string packageName);

        // Runs `pm list packages [filter]` on a detached thread and
        // reflects the result into both `listPtr` (one row per
        // package) and `statusPtr`, same pattern refreshDevices() uses
        // for the Devices panel -- except guarded by `viewGeneration`
        // (see PackagesPanelState's own doc) since the list view this
        // was launched for may no longer be the one mounted by the
        // time the adb call returns.
        void loadPackagesList(DeviceWindow& win, std::shared_ptr<PackagesPanelState> state,
            dsbox_ui::ListView* listPtr, dsbox_ui::Text* statusPtr, std::string filterValue,
            std::uint64_t viewGeneration)
        {
            std::thread([&win, state, listPtr, statusPtr, filterValue, viewGeneration]
            {
                std::string serialValue = win.selectedSerialSnapshot();
                std::vector<std::string> names;
                std::string message;

                if (serialValue.empty())
                {
                    message = "No device selected -- pick one in the Devices panel.\n";
                }
                else
                {
                    std::string warning = unknownSerialWarning(win.host.adb, serialValue);
                    try
                    {
                        names = win.host.adb.device(serialValue).getPackages().list(filterValue);
                        message = warning
                            + (names.empty()
                                   ? std::string("(no matching packages)\n")
                                   : std::to_string(names.size()) + " package(s) -- click one for details.\n");
                    }
                    catch (const std::exception& e)
                    {
                        message = warning + std::string("failed: ") + e.what() + "\n";
                    }
                }

                win.uiTasks.push([&win, state, listPtr, statusPtr, names, message, viewGeneration]
                {
                    if (state->generation.load() != viewGeneration)
                        return; // the Packages panel has since shown a different view

                    state->packageNames = names;

                    if (listPtr)
                    {
                        std::vector<std::vector<std::string>> rows;
                        rows.reserve(names.size());
                        for (const auto& name : names)
                            rows.push_back({ name });
                        listPtr->setRows(rows);
                        win.tree->markDirty(listPtr->id());
                    }

                    if (statusPtr)
                    {
                        statusPtr->setText(message);
                        win.tree->markDirty(statusPtr->id());
                    }
                });
            }).detach();
        }

        // Mounts the package LIST view into state->body: an Install
        // row, a filter/Refresh row, a status line, and the scrollable
        // ListView of every package matching the current filter.
        // Clicking a row opens that package's detail view (see
        // showPackageDetail()) in place of this one, inside the same
        // panel -- no new window, no leaving the Packages panel.
        void showPackagesList(DeviceWindow& win, std::shared_ptr<PackagesPanelState> state, std::string filterValue)
        {
            using namespace dsbox_ui;

            if (!state->body)
                return;

            const std::uint64_t viewGeneration = ++state->generation;
            state->lastFilter = filterValue;

            Container& body = *state->body;
            body.clearChildren();

            // Install -- this used to be its own "Install / Uninstall"
            // sidebar panel; it now lives at the top of the merged
            // Packages panel instead, since installing an APK is
            // naturally part of managing packages on this device.
            Container& installRow = addRow(win, body);
            TextField& apkPath = addFlexibleField(win, installRow, "path to .apk");
            ButtonParts installButton = makeHostButton(win, "Install");
            installButton.root->setPreferredWidth(UiLength::pixels(110.0f));

            Container& filterRow = addRow(win, body);
            TextField& filter = addFlexibleField(win, filterRow, "filter (optional)");
            filter.setText(filterValue);
            ButtonParts refreshButton = makeHostButton(win, "Refresh");
            refreshButton.root->setPreferredWidth(UiLength::pixels(100.0f));

            Text& status = addOutput(win, body);
            Text* statusPtr = &status;

            auto& scroll = static_cast<ScrollView&>(body.addChild(
                std::make_unique<ScrollView>(win.panelIds.nextId(), UiElementIdMode::Unique, "packagesScroll")));
            scroll.setPreferredWidth(UiLength::percent(100.0f));
            scroll.setPreferredHeight(UiLength::fraction(1.0f));
            scroll.setVerticalScrollEnabled(true);
            scroll.setHorizontalScrollEnabled(false);
            scroll.setBackgroundColor(UiColor{ 0.0f, 0.0f, 0.0f, 0.0f });
            scroll.setFocusable(true);

            // ListView sizes itself to fit its own rows on a
            // Content-sized axis (unlike Container/TextArea -- see
            // ListView's own doc, list_view.hpp), which is exactly what
            // lets it sit inside a ScrollView and have the ScrollView's
            // viewport (not the list itself) decide how much is
            // actually visible at once.
            auto listOwned = std::make_unique<ListView>(win.panelIds.nextId(), UiElementIdMode::Unique, "packagesList");
            ListView* listPtr = listOwned.get();
            listPtr->setColumns({ { "Package", 520.0f } });
            applyFont(win, *listPtr);
            listPtr->setPreferredWidth(UiLength::percent(100.0f));
            listPtr->setPreferredHeight(UiLength::content());
            listPtr->setHeaderBackgroundColor(UiColor{ 0.20f, 0.20f, 0.24f, 1.0f });
            listPtr->setHeaderTextColor(UiColor{ 1.0f, 1.0f, 1.0f, 1.0f });
            listPtr->setRowBackgroundColor(UiColor{ 0.16f, 0.16f, 0.19f, 1.0f });
            listPtr->setAlternateRowColor(UiColor{ 0.19f, 0.19f, 0.23f, 1.0f });
            listPtr->setSelectedRowColor(UiColor{ 0.20f, 0.45f, 0.85f, 0.45f });
            listPtr->setTextColor(UiColor{ 0.88f, 0.88f, 0.88f, 1.0f });
            listPtr->setBorderColor(UiColor{ 0.30f, 0.30f, 0.34f, 1.0f });
            listPtr->setGridLineColor(UiColor{ 0.24f, 0.24f, 0.28f, 1.0f });
            scroll.setChild(std::move(listOwned));

            TextField* apkPathPtr = &apkPath;
            TextField* filterPtr = &filter;

            installButton.root->setOnClick(
                [&win, statusPtr, apkPathPtr]
                {
                    std::string serialValue = requireSelectedDevice(win, statusPtr);
                    if (serialValue.empty())
                        return;

                    std::string pathValue = apkPathPtr->text();
                    std::string warning = unknownSerialWarning(win.host.adb, serialValue);

                    runAdbCall(win, statusPtr, [&win, serialValue, pathValue, warning]
                    {
                        const auto result = win.host.adb.device(serialValue).install(pathValue, {});
                        return warning + formatCommandResult(result);
                    });
                });
            installRow.addChild(std::move(installButton.root));

            refreshButton.root->setOnClick(
                [&win, state, listPtr, statusPtr, filterPtr, viewGeneration]
                {
                    std::string filterValue = filterPtr->text();
                    state->lastFilter = filterValue;
                    loadPackagesList(win, state, listPtr, statusPtr, filterValue, viewGeneration);
                });
            filterRow.addChild(std::move(refreshButton.root));

            // Clicking a row -- a selection change ListView attributes
            // to the user (click or arrow keys, never setSelectedIndex()
            // itself, see ListView's own doc) -- opens that package's
            // detail view. Pushed through uiTasks like every other
            // tree-mutating callback in this file (see UiTaskQueue's
            // own comment up top); the generation check guards against
            // a selection event that was already queued when the user
            // switched away from this list view some other way.
            listPtr->setOnSelectionChanged(
                [&win, state, viewGeneration](std::size_t index)
                {
                    win.uiTasks.push([&win, state, index, viewGeneration]
                    {
                        if (state->generation.load() != viewGeneration)
                            return;
                        if (index >= state->packageNames.size())
                            return;

                        std::string packageName = state->packageNames[index];
                        showPackageDetail(win, state, packageName);
                    });
                });

            loadPackagesList(win, state, listPtr, statusPtr, filterValue, viewGeneration);
        }

        // Runs `dumpsys package <packageName>` on a detached thread and
        // drops the raw text into `outputPtr` -- the package detail
        // view's own default "information about this program" (see
        // showPackageDetail()), refreshed again by its "Refresh Info"
        // button. Guarded by `viewGeneration` the same way
        // loadPackagesList() is.
        void loadPackageInfo(DeviceWindow& win, std::shared_ptr<PackagesPanelState> state,
            dsbox_ui::TextArea* outputPtr, std::string packageName, std::uint64_t viewGeneration)
        {
            std::string serialValue = win.selectedSerialSnapshot();

            if (serialValue.empty())
            {
                outputPtr->setText("No device selected -- pick one in the Devices panel.\n");
                win.tree->markDirty(outputPtr->id());
                return;
            }

            outputPtr->setText("Loading...\n");
            win.tree->markDirty(outputPtr->id());

            std::thread([&win, state, outputPtr, serialValue, packageName, viewGeneration]
            {
                std::string text;
                try
                {
                    text = win.host.adb.device(serialValue).getPackages().dumpPackage(packageName);
                    if (text.empty())
                        text = "(no dumpsys output for this package)\n";
                }
                catch (const std::exception& e)
                {
                    text = std::string("failed: ") + e.what() + "\n";
                }

                win.uiTasks.push([&win, state, outputPtr, text, viewGeneration]
                {
                    if (state->generation.load() != viewGeneration)
                        return;

                    outputPtr->setText(text);
                    win.tree->markDirty(outputPtr->id());
                });
            }).detach();
        }

        // Mounts the package DETAIL view into state->body in place of
        // the list: a "<- Back" button and the package's own name, a
        // row of actions (Uninstall / Clear Data / Force Stop /
        // Refresh Info) that run the matching AdbPackages method
        // against this one package, a status line for whichever action
        // last ran, and a scrollable `dumpsys package` dump -- loaded
        // automatically as soon as this view opens (loadPackageInfo()
        // below), so the "window" the person clicked into already has
        // something to show them instead of starting blank.
        void showPackageDetail(DeviceWindow& win, std::shared_ptr<PackagesPanelState> state, std::string packageName)
        {
            using namespace dsbox_ui;

            if (!state->body)
                return;

            const std::uint64_t viewGeneration = ++state->generation;

            Container& body = *state->body;
            body.clearChildren();

            Container& headerRow = addRow(win, body);

            ButtonParts back = makeHostButton(win, "<- Back");
            back.root->setPreferredWidth(UiLength::pixels(90.0f));
            back.root->setOnClick(
                [&win, state]
                { win.uiTasks.push([&win, state] { showPackagesList(win, state, state->lastFilter); }); });
            headerRow.addChild(std::move(back.root));

            Text& title = addOutput(win, headerRow);
            title.setText(packageName);

            Container& actionsRow = addRow(win, body);

            Text& status = addOutput(win, body);
            Text* statusPtr = &status;

            TextArea& output = addScrollableOutput(win, body);
            TextArea* outputPtr = &output;

            // Shared by every action button below: resolves the
            // selected device, runs `action` against this package's
            // own AdbPackages wrapper on a detached thread, and
            // reflects the formatted AdbCommandResult into statusPtr --
            // the same runAdbCall() shape every other panel uses,
            // specialized here since the work needs an AdbPackages (not
            // just an AdbDevice).
            auto runPackageAction = [&win, state, statusPtr, viewGeneration](
                std::function<dsbox_android_adb::AdbCommandResult(dsbox_android_adb::AdbPackages&)> action)
            {
                std::string serialValue = requireSelectedDevice(win, statusPtr);
                if (serialValue.empty())
                    return;

                std::string warning = unknownSerialWarning(win.host.adb, serialValue);

                std::thread([&win, state, statusPtr, serialValue, warning, action, viewGeneration]
                {
                    std::string result;
                    try
                    {
                        auto packages = win.host.adb.device(serialValue).getPackages();
                        const auto cmdResult = action(packages);
                        result = warning + formatCommandResult(cmdResult);
                    }
                    catch (const std::exception& e)
                    {
                        result = warning + std::string("failed: ") + e.what() + "\n";
                    }

                    win.uiTasks.push([&win, state, statusPtr, result, viewGeneration]
                    {
                        if (state->generation.load() != viewGeneration)
                            return;
                        statusPtr->setText(result);
                        win.tree->markDirty(statusPtr->id());
                    });
                }).detach();
            };

            ButtonParts uninstallButton = makeHostButton(win, "Uninstall");
            uninstallButton.root->setPreferredWidth(UiLength::pixels(110.0f));
            uninstallButton.root->setOnClick(
                [runPackageAction, packageName]
                {
                    runPackageAction(
                        [packageName](dsbox_android_adb::AdbPackages& packages) { return packages.uninstall(packageName); });
                });
            actionsRow.addChild(std::move(uninstallButton.root));

            ButtonParts clearDataButton = makeHostButton(win, "Clear Data");
            clearDataButton.root->setPreferredWidth(UiLength::pixels(110.0f));
            clearDataButton.root->setOnClick(
                [runPackageAction, packageName]
                {
                    runPackageAction(
                        [packageName](dsbox_android_adb::AdbPackages& packages) { return packages.clearData(packageName); });
                });
            actionsRow.addChild(std::move(clearDataButton.root));

            ButtonParts forceStopButton = makeHostButton(win, "Force Stop");
            forceStopButton.root->setPreferredWidth(UiLength::pixels(110.0f));
            forceStopButton.root->setOnClick(
                [runPackageAction, packageName]
                {
                    runPackageAction(
                        [packageName](dsbox_android_adb::AdbPackages& packages) { return packages.forceStop(packageName); });
                });
            actionsRow.addChild(std::move(forceStopButton.root));

            ButtonParts refreshInfoButton = makeHostButton(win, "Refresh Info");
            refreshInfoButton.root->setPreferredWidth(UiLength::pixels(130.0f));
            refreshInfoButton.root->setOnClick(
                [&win, state, outputPtr, packageName, viewGeneration]
                { loadPackageInfo(win, state, outputPtr, packageName, viewGeneration); });
            actionsRow.addChild(std::move(refreshInfoButton.root));

            loadPackageInfo(win, state, outputPtr, packageName, viewGeneration);
        }

        // Merged "Packages" panel: everything the old separate
        // "Packages" (list-only) and "Install / Uninstall" panels did,
        // plus per-package details. state->body is the one Container
        // the two inner views (showPackagesList()/showPackageDetail())
        // take turns rendering into; this factory itself only ever
        // mounts the list view, the very first time the panel opens.
        dsbox_ui::BuiltPanel buildPackagesPanel(DeviceWindow& win, dsbox_ui::PanelBuildContext& ctx)
        {
            using namespace dsbox_ui;
            (void)ctx;

            auto root = std::make_unique<Container>(win.panelIds.nextId(), UiElementIdMode::Unique, "packagesPanel");
            root->setPreferredWidth(UiLength::percent(100.0f));
            root->setPreferredHeight(UiLength::percent(100.0f));
            root->setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 12.0f));

            addSelectedDeviceHeader(win, *root);

            auto& body = static_cast<Container&>(root->addChild(
                std::make_unique<Container>(win.panelIds.nextId(), UiElementIdMode::Unique, "packagesBody")));
            body.setPreferredWidth(UiLength::percent(100.0f));
            body.setPreferredHeight(UiLength::fraction(1.0f));
            body.setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 12.0f));

            auto state = std::make_shared<PackagesPanelState>();
            state->body = &body;

            showPackagesList(win, state, std::string());

            // Nulls state->body the moment this whole panel is torn
            // down (switched away from, or the window closed) -- see
            // PackagesPanelState's own doc for why every function above
            // checks this before touching `body`.
            return BuiltPanel{ std::move(root), [state] { state->body = nullptr; } };
        }

        dsbox_ui::BuiltPanel buildLogcatPanel(DeviceWindow& win, dsbox_ui::PanelBuildContext& ctx)
        {
            using namespace dsbox_ui;
            (void)ctx;

            auto root = std::make_unique<Container>(win.panelIds.nextId(), UiElementIdMode::Unique, "logcatPanel");
            root->setPreferredWidth(UiLength::percent(100.0f));
            root->setPreferredHeight(UiLength::percent(100.0f));
            root->setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 12.0f));

            addSelectedDeviceHeader(win, *root);

            Container& row = addRow(win, *root);

            TextArea& output = addScrollableOutput(win, *root);
            TextArea* outputPtr = &output;

            {
                std::lock_guard<std::mutex> lock(win.logMutex);
                output.setText(win.logBuffer);
            }
            win.logcatOutput = outputPtr;

            ButtonParts start = makeHostButton(win, "Start");
            start.root->setPreferredWidth(UiLength::pixels(90.0f));
            start.root->setOnClick(
                [&win]
                {
                    std::string serialValue = win.selectedSerialSnapshot();
                    if (serialValue.empty())
                        return; // Devices panel is where this gets fixed

                    {
                        std::lock_guard<std::mutex> lock(win.logMutex);
                        win.logcatSerial = serialValue;
                    }

                    std::thread(
                        [&win, serialValue]
                        {
                            try
                            {
                                win.host.adb.startLogcat(serialValue, {});
                            }
                            catch (const std::exception&)
                            {
                                // Surfaces on "adb.logcat.stderr_line" a
                                // moment later -- nothing more to do here.
                            }
                        }).detach();
                });
            row.addChild(std::move(start.root));

            ButtonParts stop = makeHostButton(win, "Stop");
            stop.root->setPreferredWidth(UiLength::pixels(90.0f));
            stop.root->setOnClick([&win] { win.host.adb.stopLogcat(win.selectedSerialSnapshot()); });
            row.addChild(std::move(stop.root));

            return BuiltPanel{ std::move(root), [&win] { win.logcatOutput = nullptr; } };
        }

        void registerBuiltinPanels(GuiHost& host)
        {
            auto& registry = *host.panelRegistry;

            // Every panel factory published into PanelRegistry has the
            // signature PanelBuildContext& -> BuiltPanel -- there's no
            // slot for "which DeviceWindow" in that signature, by
            // design (see panel.hpp's own PanelBuildContext doc: "a
            // future host that manages several windows doesn't need a
            // breaking change to support it"). PanelBuildContext DOES
            // carry the window handle it's being built for, so `dispatch`
            // below is how this host plugs into that: look up which
            // DeviceWindow owns ctx.window and hand it to the real
            // build*Panel function. A single global PanelDescriptor per
            // built-in panel still works for any number of open
            // windows, same as a dynamically loaded module's own panel
            // would.
            auto dispatch = [&host](dsbox_ui::BuiltPanel (*buildFn)(DeviceWindow&, dsbox_ui::PanelBuildContext&))
            {
                return [&host, buildFn](dsbox_ui::PanelBuildContext& ctx) -> dsbox_ui::BuiltPanel
                {
                    DeviceWindow* win = findWindow(host, ctx.window);
                    if (!win)
                        return dsbox_ui::BuiltPanel{};
                    return buildFn(*win, ctx);
                };
            };

            registry.registerPanel({ "adb.devices", "Devices", dispatch(buildDevicesPanel) });
            registry.registerPanel({ "adb.shell", "Shell", dispatch(buildShellPanel) });

            // "adb.install" no longer exists as its own sidebar entry
            // -- installing an APK is now the top row of "adb.packages"
            // itself (see showPackagesList()), and uninstalling/
            // clearing data/force-stopping move with a package into its
            // own detail view (showPackageDetail()) rather than needing
            // its package name typed into a separate field.
            registry.registerPanel({ "adb.packages", "Packages", dispatch(buildPackagesPanel) });
            registry.registerPanel({ "adb.logcat", "Logcat", dispatch(buildLogcatPanel) });
        }

        // Appends to `win`'s own rolling log buffer -- but only if
        // `serial` matches whatever this window's logcat Start button
        // was last clicked for (win.logcatSerial) -- and, if the logcat
        // panel is currently mounted in this window, pushes a task to
        // reflect it. The three "adb.logcat.*" subscriptions in
        // runGui() call this once per open window for every event (see
        // forEachWindow()), so a line only ever lands in the window(s)
        // actually watching that serial.
        void appendLogLine(DeviceWindow& win, const std::string& serial, const std::string& line)
        {
            bool matches = false;

            {
                std::lock_guard<std::mutex> lock(win.logMutex);
                matches = !win.logcatSerial.empty() && win.logcatSerial == serial;

                if (matches)
                {
                    win.logBuffer += line;

                    constexpr std::size_t kMaxBuffer = 20000;
                    if (win.logBuffer.size() > kMaxBuffer)
                        win.logBuffer.erase(0, win.logBuffer.size() - kMaxBuffer);
                }
            }

            if (!matches)
                return;

            win.uiTasks.push(
                [&win]
                {
                    if (!win.logcatOutput)
                        return;

                    std::string snapshot;
                    {
                        std::lock_guard<std::mutex> lock(win.logMutex);
                        snapshot = win.logBuffer;
                    }

                    win.logcatOutput->setText(snapshot);
                    win.tree->markDirty(win.logcatOutput->id());
                });
        }

        // Creates one new window -- the window/surface/tree/driver
        // quadruple, its own font, its initial sidebar/panel -- and
        // starts its render loop on its own dedicated std::thread, the
        // same pattern dsbox_ui_demo's own main.cpp uses for its two
        // startup windows, just invoked on demand instead of only at
        // startup. Only ever called from runGui()'s own thread (the
        // first time, directly; afterwards, from its supervisor loop --
        // see that loop's own comment for why window creation is
        // funneled through there). Returns nullptr (after logging why)
        // if window/surface creation itself failed; the window is still
        // registered into host.windows on success, for the supervisor
        // loop to reap once it closes.
        DeviceWindow* openWindow(GuiHost& host, const std::string& initialSerial)
        {
            auto win = std::make_unique<DeviceWindow>(host);

            dsbox_sdl::WindowOptions windowOptions;
            windowOptions.title = "DSBox ADB Tool";
            windowOptions.width = 1100;
            windowOptions.height = 650;
            windowOptions.graphicsHint = dsbox_sdl::WindowGraphicsHint::Vulkan;

            win->window = host.windowModule->createWindow(windowOptions);
            if (win->window == dsbox::kInvalidWindowHandle)
            {
                std::fprintf(
                    stderr, "dsbox_adb_tool: createWindow failed: %s\n", host.windowModule->lastError().c_str());
                return nullptr;
            }

            win->surface = host.vulkanModule->attachToWindow(*host.windowModule, win->window);
            if (win->surface == dsbox_vulkan::kInvalidSurfaceHandle)
            {
                std::fprintf(
                    stderr, "dsbox_adb_tool: attachToWindow failed: %s\n", host.vulkanModule->lastError().c_str());
                host.windowModule->closeWindow(win->window);
                return nullptr;
            }

            dsbox_ui::WindowUiTree& tree =
                host.uiModule->createTree(win->window, buildChrome(*win), host.windowModule);
            win->tree = &tree;

            // DSBoxVulkanGui.dll/.so lives in bin/framework/render/, a
            // sibling of this executable's own bin/executables/ -- but
            // the compiled shaders now live in their own top-level
            // fin-out/shaders/dsbox_vulkan_gui/ folder, a sibling of
            // fin-out/bin/ rather than nested inside it (see
            // docs/BUILD_LAYOUT.md). This executable's own directory is
            // fin-out/bin/executables/, so reaching fin-out/shaders/
            // takes two ".." steps.
            const std::string shaderDirectory =
                (executableDirectory() / ".." / ".." / "shaders" / "dsbox_vulkan_gui").string();
            win->driver = std::make_unique<dsbox_vulkan_gui::VulkanUiWindowDriver>(
                *host.vulkanModule, win->surface, tree, shaderDirectory);

            if (!win->driver->lastError().empty())
                std::fprintf(
                    stderr, "dsbox_adb_tool: VulkanUiWindowDriver: %s\n", win->driver->lastError().c_str());

            dsbox_ui::IUiRenderBackend& backend = win->driver->renderBackend();
            win->font = dsbox_ui::loadFontOrFallback(
                backend, "C:\\Windows\\Fonts\\segoeui.ttf", 16.0f, "C:\\Windows\\Fonts\\consola.ttf");

            win->setSelectedSerial(initialSerial);

            rebuildSidebar(*win);

            const auto initialPanels = host.panelRegistry->list();
            if (!initialPanels.empty())
                switchPanel(*win, initialPanels.front().id);

            DeviceWindow* winPtr = win.get();
            dsbox_sdl::WindowModule* windowModule = host.windowModule;

            // Continuous rendering, independent of
            // dsbox_sdl::WindowModule's own control thread -- same
            // reasoning dsbox_ui_demo's own main.cpp gives: a Windows
            // modal resize loop would otherwise freeze THIS window's
            // picture for as long as it's being dragged. Each window
            // gets its own thread now (there can be more than one),
            // where earlier revisions of this file used runGui()'s own
            // calling thread directly, since there was only ever one.
            winPtr->renderThread = std::thread(
                [winPtr, windowModule]
                {
                    while (!winPtr->closed.load(std::memory_order_relaxed))
                    {
                        int width = 0, height = 0;
                        if (!windowModule->windowSize(winPtr->window, width, height))
                            break;

                        winPtr->uiTasks.drainAndRun();

                        if (width <= 0 || height <= 0)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(16));
                            continue;
                        }

                        winPtr->driver->onFrame(winPtr->window, width, height);
                    }

                    winPtr->closed.store(true, std::memory_order_relaxed);
                });

            {
                std::lock_guard<std::mutex> lock(host.windowsMutex);
                host.windows.push_back(std::move(win));
            }

            return winPtr;
        }

        // Reverses everything openWindow() set up for `win`. Only ever
        // called from runGui()'s own supervisor loop, and only once
        // win.closed is already true (join() below still waits for the
        // render thread to actually finish its current iteration if it
        // hasn't quite yet).
        void closeAndDestroyWindow(GuiHost& host, DeviceWindow& win)
        {
            if (win.renderThread.joinable())
                win.renderThread.join();

            if (win.activeOnDestroy)
                win.activeOnDestroy();

            win.driver.reset();
            host.uiModule->destroyTree(win.window);
            host.vulkanModule->detach(win.surface);
            host.windowModule->closeWindow(win.window);
        }
    }

    void runGui(dsbox::Context& context, dsbox::ModuleManager& modules, AdbModule& adb)
    {
        GuiHost host{ context, modules, adb };
        host.panelRegistry = std::make_shared<dsbox_ui::PanelRegistry>();
        context.provide(host.panelRegistry);

        // dsbox_sdl::WindowModule and dsbox_vulkan::VulkanModule self-
        // register via DSBOX_STATIC_MODULE once dsbox_adb_tool links
        // DSBox::SDL/DSBox::Vulkan, so main()'s earlier
        // registerStaticModules() + startAll() pass already started
        // both of them -- only find() is needed for them here.
        //
        // Calling modules.startAll() a SECOND time here would be wrong
        // -- see this function's own earlier revisions for the full
        // reasoning (calling modules.startAll() again would rerun the
        // already-started modules). dsbox_ui::UiModule is the only
        // genuinely new module here (it deliberately does not self-
        // register -- see its own CMakeLists.txt comment), so it alone
        // gets onInit() called directly, bypassing ModuleManager the
        // same way dsbox_ui_demo's own main.cpp does for all three of
        // these modules.
        host.windowModule = dynamic_cast<dsbox_sdl::WindowModule*>(modules.find("dsbox.sdl.window"));
        host.vulkanModule = dynamic_cast<dsbox_vulkan::VulkanModule*>(modules.find("dsbox.vulkan"));

        if (!host.windowModule || !host.vulkanModule)
        {
            std::fprintf(stderr,
                "dsbox_adb_tool: dsbox_sdl::WindowModule/dsbox_vulkan::VulkanModule not found -- "
                "did registerStaticModules()/startAll() run in main() with DSBox::SDL/DSBox::Vulkan linked?\n");
            return;
        }

        dsbox_ui::UiModule* uiModulePtr = dynamic_cast<dsbox_ui::UiModule*>(modules.find("dsbox.ui"));

        if (!uiModulePtr)
        {
            auto uiModuleOwned = std::make_unique<dsbox_ui::UiModule>();
            uiModulePtr = uiModuleOwned.get();
            modules.registerModule(std::move(uiModuleOwned));
            host.uiOwnedHere = true;

            try
            {
                uiModulePtr->onInit(context);
            }
            catch (const std::exception& error)
            {
                std::fprintf(stderr, "dsbox_adb_tool: failed to init dsbox_ui::UiModule: %s\n", error.what());
                return;
            }
        }

        host.uiModule = uiModulePtr;

        registerBuiltinPanels(host);

        // Permanent for this host's whole lifetime -- this function
        // runs once per process, and every subscription below reaches
        // every window open at the time it fires via forEachWindow(),
        // so there's no per-window (re)subscription to manage as
        // windows come and go.
        host.panelsChangedSubscription = host.panelRegistry->onChanged(
            [&host] { forEachWindow(host, [](DeviceWindow& win) { win.uiTasks.push([&win] { rebuildSidebar(win); }); }); });

        context.comm().registerEvent<LogcatLineEvent>(
            "adb.logcat.line",
            [&host](LogcatLineEvent event)
            {
                forEachWindow(host, [&](DeviceWindow& win)
                    { appendLogLine(win, event.serial, "[" + event.serial + "] " + event.line + "\n"); });
            });

        context.comm().registerEvent<LogcatLineEvent>(
            "adb.logcat.stderr_line",
            [&host](LogcatLineEvent event)
            {
                forEachWindow(host, [&](DeviceWindow& win)
                    { appendLogLine(win, event.serial, "[" + event.serial + "] (adb error) " + event.line + "\n"); });
            });

        context.comm().registerEvent<LogcatStoppedEvent>(
            "adb.logcat.stopped",
            [&host](LogcatStoppedEvent event)
            {
                forEachWindow(host, [&](DeviceWindow& win)
                {
                    appendLogLine(win, event.serial,
                        "[adb] logcat for " + event.serial +
                            " stopped (exit code " + std::to_string(event.exitCode) + ")\n");
                });
            });

        context.comm().registerEvent<dsbox::WindowHandle>(
            "window.closed",
            [&host](dsbox::WindowHandle closedHandle)
            {
                if (DeviceWindow* win = findWindow(host, closedHandle))
                    win->closed.store(true, std::memory_order_relaxed);
            });

        if (!openWindow(host, {}))
        {
            if (host.uiOwnedHere)
                host.uiModule->onShutdown(context);
            return;
        }

        // Supervisor loop -- this is runGui()'s own calling thread,
        // the same one that opened the very first window just above,
        // and stays the ONLY thread that ever calls
        // createWindow()/attachToWindow()/closeWindow() for the
        // lifetime of this function, so two such calls (one for a
        // queued "Open in new window" request, one for another) never
        // race each other. Each window's own rendering runs
        // independently on its own thread (see openWindow()); this
        // loop just creates newly requested ones, reaps closed ones
        // (joining their render thread and tearing down their
        // window/surface/tree), and returns once none are left open
        // or pending -- main() blocks here the same way it used to
        // block on the single render loop this replaced.
        for (;;)
        {
            std::vector<std::string> pending;
            {
                std::lock_guard<std::mutex> lock(host.pendingMutex);
                pending.swap(host.pendingOpenRequests);
            }

            for (auto& serial : pending)
                openWindow(host, serial);

            bool anyOpen = false;
            {
                std::lock_guard<std::mutex> lock(host.windowsMutex);
                for (auto it = host.windows.begin(); it != host.windows.end();)
                {
                    if ((*it)->closed.load(std::memory_order_relaxed))
                    {
                        closeAndDestroyWindow(host, **it);
                        it = host.windows.erase(it);
                    }
                    else
                    {
                        anyOpen = true;
                        ++it;
                    }
                }
            }

            bool pendingLeft = false;
            {
                std::lock_guard<std::mutex> lock(host.pendingMutex);
                pendingLeft = !host.pendingOpenRequests.empty();
            }

            if (!anyOpen && !pendingLeft)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        host.panelRegistry->removeOnChanged(host.panelsChangedSubscription);

        // Mirrors the manual onInit() call above -- see this function's
        // own earlier revisions for why only an instance actually
        // initialized here gets shut down from here.
        if (host.uiOwnedHere)
            host.uiModule->onShutdown(context);
    }
}
