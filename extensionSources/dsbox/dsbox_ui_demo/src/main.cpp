// End-to-end smoke test for the dsbox_ui / dsbox_vulkan_gui stack:
// opens TWO windows, each with its own small dsbox_ui::Container tree
// (a header bar with a Text label over three side-by-side cards, two
// of them Containers and one an Image, plus a controls row with a
// Button and a TextField), and renders both continuously on their OWN
// dedicated threads. Tab/Shift+Tab cycles focus across every
// focusable element (both cards, the button, the text field); typing
// while the field is focused goes through dsbox_sdl::WindowModule's
// real dsbox::ITextInputProvider implementation (see
// uiModule.createTree()'s third argument below).
//
// NOTE on dsbox_sdl::IWindowRenderHook (see <dsbox/window_render_hook.hpp>
// and WindowModule::setRenderHook()): an earlier version of this demo
// drove rendering from THAT mechanism instead -- calling onFrame() once
// per WindowModule control-thread iteration. That turned out to be the
// wrong choice for CONTINUOUS rendering specifically: on Windows, a live
// border-drag resize enters a modal loop inside the OS's own message
// pump, which blocks the control thread for the whole drag -- and since
// rendering was ALSO on that same thread by then, the picture froze for
// the same duration instead of updating (only the OS compositor's own
// stretch of the last-presented frame was visible). Continuous
// rendering belongs on its own thread, independent of that modal loop,
// which is what this file does. The render-hook mechanism itself is
// still useful for something that genuinely needs to happen exactly
// once per control-thread iteration -- just not this.
//
// dsbox_vulkan_gui::VulkanUiWindowDriver's onFrame(handle, width,
// height) doesn't care who calls it or how often -- it's called
// directly, in a loop, from a std::thread this file owns per window,
// using WindowModule::windowSize() (a plain thread-safe cached read,
// not a "window.resized" subscription) to learn the current size fresh
// every iteration.
//
// Bypasses dsbox_core's ModuleManager/module_loader (the mechanism
// DSBoxLauncher uses to load modules dynamically from an .ini file) --
// dsbox::Context is self-contained (see its own header) and every
// IModule's onInit()/onShutdown() can be called directly, which keeps
// this demo's control flow linear and easy to read top to bottom rather
// than routed through configuration.

#include <dsbox/bootstrap_paths.hpp>
#include <dsbox/context.hpp>

#include <dsbox_sdl/window_module.hpp>
#include <dsbox_ui/button.hpp>
#include <dsbox_ui/container.hpp>
#include <dsbox_ui/image.hpp>
#include <dsbox_ui/render_backend.hpp>
#include <dsbox_ui/stack_layout.hpp>
#include <dsbox_ui/text.hpp>
#include <dsbox_ui/text_field.hpp>
#include <dsbox_ui/window_ui_tree.hpp>
#include <dsbox_vulkan/vulkan_module.hpp>
#include <dsbox_vulkan_gui/vulkan_ui_window_driver.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
#else
    #include <climits>
    #include <unistd.h>
#endif

namespace
{
    // The directory this executable's own binary lives in -- used both
    // to widen the DLL search path (see dsbox::bootstrapFrameworkSearchPaths()
    // in main(), below) and as the base for locating dsbox_vulkan_gui's
    // compiled shaders (see shaderDirectory in main(): DSBoxVulkanGui now
    // lives in bin/framework/render/, a sibling of this executable's own
    // bin/executables/, not the same folder it used to share with it).
    std::filesystem::path executableDirectory()
    {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length == 0)
            return std::filesystem::current_path();
        return std::filesystem::path(buffer, buffer + length).parent_path();
#else
        char buffer[PATH_MAX];
        ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (length <= 0)
            return std::filesystem::current_path();
        buffer[length] = '\0';
        return std::filesystem::path(buffer).parent_path();
#endif
    }

    // A header bar (with a Text label over it) over three side-by-side
    // cards -- just enough nesting to exercise Container/StackLayout in
    // both directions, Text/Image as leaf elements, and rounded-corner
    // rects at a few different sizes. `windowIndex` only changes the
    // header's color, so the two demo windows are visibly
    // distinguishable at a glance while running independently.
    //
    // Both Container cards are also clickable/focusable (see
    // Container's own "-- Interactivity --" section) -- a real end-to-
    // end smoke test of the IUiElementActions plumbing added alongside
    // this: clicking either one prints to stdout AND visibly swaps in
    // its own setPressedColor() for as long as the mouse button is
    // held, proving WindowUiTree::dispatchEvent() -> Container::
    // handleEvent() -> IUiElementActions::markDirty()/requestFocus() ->
    // a genuinely new tick() -> a genuinely new DisplayList all
    // actually happen, on a real window, not just in a unit test.
    //
    // The header's Text and the third card's Image are built here with
    // FontHandle/ImageHandle left at their invalid defaults -- neither
    // can be obtained until AFTER this tree is handed to a real
    // IUiRenderBackend (see main()'s own comment on why), so
    // buildDemoUi() hands back raw pointers to all of them alongside
    // the tree root, for the caller to finish configuring once that
    // backend exists. buttonLabel/textField exercise makeButton()
    // (button.hpp) and TextField (text_field.hpp) respectively -- both
    // also demonstrate WindowUiTree's Tab/Shift+Tab traversal (see its
    // own dispatchEvent()) since they're focusable alongside cardA/
    // cardB, and textField exercises the real IME start/stop wiring
    // through main()'s dsbox_sdl::WindowModule (see uiModule.createTree()
    // there).
    struct DemoUi
    {
        std::unique_ptr<dsbox_ui::IUiElement> root;
        dsbox_ui::Text* headerLabel = nullptr;
        dsbox_ui::Image* cardImage = nullptr;
        dsbox_ui::Text* buttonLabel = nullptr;
        dsbox_ui::TextField* textField = nullptr;
    };

    // 2-color checkerboard, `tileSize`-pixel squares, RGBA8 -- exactly
    // the pixel format IUiRenderBackend::registerImage() expects
    // (render_backend.hpp). Doesn't depend on any image file existing
    // on disk, so this demo has no extra asset to ship just to
    // exercise Image -- see buildDemoUi()'s own comment on what this
    // feeds into.
    std::vector<std::uint8_t> buildCheckerboardRgba8(std::uint32_t width, std::uint32_t height, std::uint32_t tileSize)
    {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);

        for (std::uint32_t y = 0; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const bool light = ((x / tileSize) + (y / tileSize)) % 2 == 0;
                const std::uint8_t value = light ? static_cast<std::uint8_t>(230) : static_cast<std::uint8_t>(60);

                const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
                pixels[offset + 0] = value;
                pixels[offset + 1] = value;
                pixels[offset + 2] = light ? static_cast<std::uint8_t>(210) : static_cast<std::uint8_t>(90);
                pixels[offset + 3] = 255;
            }
        }

        return pixels;
    }

    DemoUi buildDemoUi(int windowIndex)
    {
        using namespace dsbox_ui;

        const UiColor headerColor = (windowIndex % 2 == 0)
            ? UiColor{ 0.20f, 0.45f, 0.85f, 1.0f }
            : UiColor{ 0.65f, 0.35f, 0.80f, 1.0f };

        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");
        root->setBackgroundColor(UiColor{ 0.10f, 0.10f, 0.13f, 1.0f });
        root->setPreferredWidth(UiLength::percent(100.0f));
        root->setPreferredHeight(UiLength::percent(100.0f));
        root->setPadding(24.0f, 24.0f, 24.0f, 24.0f);
        root->setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 16.0f));

        auto& header = static_cast<Container&>(
            root->addChild(std::make_unique<Container>(2, UiElementIdMode::Unique, "header")));
        header.setBackgroundColor(headerColor);
        header.setCornerRadius(10.0f);
        header.setPreferredWidth(UiLength::percent(100.0f));
        header.setPreferredHeight(UiLength::pixels(80.0f));
        header.setPadding(0.0f, 24.0f, 0.0f, 24.0f);
        header.setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Horizontal, 0.0f));

        // FontHandle stays kInvalidFontHandle until main() loads one
        // against the real IUiRenderBackend this tree ends up ticked
        // against -- see DemoUi's own comment above.
        auto& headerLabel = static_cast<Text&>(
            header.addChild(std::make_unique<Text>(3, UiElementIdMode::Unique, "headerLabel")));
        headerLabel.setText("DSBox UI Demo - Window " + std::to_string(windowIndex + 1));
        headerLabel.setColor(UiColor{ 1.0f, 1.0f, 1.0f, 1.0f });

        auto& body = static_cast<Container&>(
            root->addChild(std::make_unique<Container>(4, UiElementIdMode::Unique, "body")));
        body.setPreferredWidth(UiLength::percent(100.0f));
        // Fraction (not Auto): root now has THREE non-flexible siblings
        // at the vertical stack's top level (header, body, controlsRow)
        // once controlsRow -- itself a fixed 56px -- is added below, and
        // StackLayout's own v1 note warns that an Auto child claims
        // ALL remaining space the moment its own turn in pass 1 comes
        // up, leaving nothing for a later fixed-size sibling in the
        // SAME pass (controlsRow would end up squeezed to ~0 height).
        // Fraction is measured in pass 2, strictly after every
        // non-flexible sibling's own space is already subtracted --
        // order-independent, which is exactly what's needed here.
        body.setPreferredHeight(UiLength::fraction(1.0f));
        body.setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Horizontal, 16.0f));

        auto& cardA = static_cast<Container&>(
            body.addChild(std::make_unique<Container>(5, UiElementIdMode::Unique, "cardA")));
        cardA.setBackgroundColor(UiColor{ 0.85f, 0.35f, 0.35f, 1.0f });
        cardA.setPressedColor(UiColor{ 0.55f, 0.15f, 0.15f, 1.0f });
        cardA.setCornerRadius(14.0f);
        cardA.setPreferredWidth(UiLength::pixels(200.0f));
        cardA.setPreferredHeight(UiLength::percent(100.0f));
        cardA.setFocusable(true);
        cardA.setOnClick([windowIndex] { std::printf("Window %d: cardA clicked\n", windowIndex + 1); });

        auto& cardB = static_cast<Container&>(
            body.addChild(std::make_unique<Container>(6, UiElementIdMode::Unique, "cardB")));
        cardB.setBackgroundColor(UiColor{ 0.35f, 0.75f, 0.45f, 1.0f });
        cardB.setPressedColor(UiColor{ 0.15f, 0.45f, 0.25f, 1.0f });
        cardB.setCornerRadius(14.0f);
        cardB.setPreferredWidth(UiLength::pixels(200.0f));
        cardB.setPreferredHeight(UiLength::percent(100.0f));
        cardB.setFocusable(true);
        cardB.setOnClick([windowIndex] { std::printf("Window %d: cardB clicked\n", windowIndex + 1); });

        // Third card: an Image instead of a Container -- ImageHandle
        // stays kInvalidImageHandle until main() registers the
        // checkerboard pixels above against the real backend, same
        // deferred-configuration reasoning as headerLabel's FontHandle.
        auto& cardImage = static_cast<Image&>(
            body.addChild(std::make_unique<Image>(7, UiElementIdMode::Unique, "cardImage")));
        cardImage.setPreferredWidth(UiLength::pixels(200.0f));
        cardImage.setPreferredHeight(UiLength::percent(100.0f));

        // Controls row: a Button (see button.hpp) and a TextField (see
        // text_field.hpp), both focusable -- Tab from cardB lands on
        // the button, then the field, cycling back to cardA (or
        // header/cardA first, depending on where focus started; see
        // WindowUiTree::dispatchEvent()'s own Tab handling for the
        // exact traversal order, which is simply tree order here).
        auto& controlsRow = static_cast<Container&>(
            root->addChild(std::make_unique<Container>(8, UiElementIdMode::Unique, "controlsRow")));
        controlsRow.setPreferredWidth(UiLength::percent(100.0f));
        controlsRow.setPreferredHeight(UiLength::pixels(56.0f));
        controlsRow.setLayoutStrategy(
            std::make_unique<StackLayout>(StackDirection::Horizontal, 16.0f, CrossAlign::Center));

        ButtonParts button = makeButton(9, 10, "Say hello", UiElementIdMode::Unique);
        button.root->setOnClick(
            [windowIndex] { std::printf("Window %d: button clicked\n", windowIndex + 1); });
        Text* buttonLabel = button.label;
        controlsRow.addChild(std::move(button.root));

        auto& textField = static_cast<TextField&>(
            controlsRow.addChild(std::make_unique<TextField>(11, UiElementIdMode::Unique, "textField")));
        textField.setPreferredWidth(UiLength::pixels(220.0f));
        textField.setPreferredHeight(UiLength::pixels(36.0f));
        textField.setPlaceholder("Type here...");
        textField.setOnChange([windowIndex](const std::string& text)
        {
            std::printf("Window %d: text field changed to \"%s\"\n", windowIndex + 1, text.c_str());
        });
        textField.setOnSubmit([windowIndex, &textField]
        {
            std::printf("Window %d: text field submitted \"%s\"\n", windowIndex + 1, textField.text().c_str());
        });

        return DemoUi{ std::move(root), &headerLabel, &cardImage, buttonLabel, &textField };
    }

    struct DemoWindow
    {
        dsbox::WindowHandle handle = dsbox::kInvalidWindowHandle;
        dsbox_vulkan::SurfaceHandle surface = dsbox_vulkan::kInvalidSurfaceHandle;
        std::unique_ptr<dsbox_vulkan_gui::VulkanUiWindowDriver> driver;
        std::atomic<bool> closed{ false };
        std::thread renderThread;
    };
}

int main()
{
    // Must happen before ANY call into a delay-loaded DSBox:: DLL (see
    // cmake/DSBoxDelayLoad.cmake) -- widens the process's DLL search path
    // to cover every bin/framework/<subsystem>/ subfolder. A no-op on
    // non-Windows, where RPATH already handles this at link time.
    // bootstrapFrameworkSearchPaths() takes the FRAMEWORK ROOT
    // (fin-out/bin/framework/), not this executable's own directory --
    // this executable lives at fin-out/bin/executables/, so the
    // framework root is one ".." step up from there, then into
    // "framework" (see bootstrap_paths.hpp's own top comment).
    dsbox::bootstrapFrameworkSearchPaths((executableDirectory() / ".." / "framework").string());

    dsbox::Context context;

    dsbox_sdl::WindowModule windowModule;
    dsbox_vulkan::VulkanModule vulkanModule;
    dsbox_ui::UiModule uiModule;

    windowModule.onInit(context);
    vulkanModule.onInit(context);
    uiModule.onInit(context);

    // DSBoxVulkanGui.dll/.so lives in bin/framework/render/, a sibling
    // of this executable's own bin/executables/ -- but the compiled
    // shaders now live in fin-out/shaders/dsbox_vulkan_gui/, a
    // top-level fin-out/ category of its own, a sibling of fin-out/bin/
    // rather than nested inside it (see docs/BUILD_LAYOUT.md). This
    // executable's own directory is fin-out/bin/executables/, so
    // reaching fin-out/shaders/ takes two ".." steps.
    const std::string shaderDirectory =
        (executableDirectory() / ".." / ".." / "shaders" / "dsbox_vulkan_gui").string();

    std::vector<std::unique_ptr<DemoWindow>> demoWindows;

    // Two windows -- exercises that WindowUiTree/VulkanUiRenderer/
    // VulkanUiWindowDriver instances are genuinely independent per
    // window, that WindowModule's single control thread serves several
    // windows' event pumping without interference, and that several
    // independent render threads share the same VkDevice/VkQueue
    // without stepping on each other.
    for (int i = 0; i < 2; ++i)
    {
        auto win = std::make_unique<DemoWindow>();

        dsbox_sdl::WindowOptions windowOptions;
        windowOptions.title = "DSBox UI Demo - Window " + std::to_string(i + 1);
        windowOptions.width = 800;
        windowOptions.height = 500;
        windowOptions.graphicsHint = dsbox_sdl::WindowGraphicsHint::Vulkan;

        win->handle = windowModule.createWindow(windowOptions);
        if (win->handle == dsbox::kInvalidWindowHandle)
        {
            std::fprintf(stderr, "Window %d: createWindow failed: %s\n", i + 1, windowModule.lastError().c_str());
            continue;
        }

        win->surface = vulkanModule.attachToWindow(windowModule, win->handle);
        if (win->surface == dsbox_vulkan::kInvalidSurfaceHandle)
        {
            std::fprintf(stderr, "Window %d: attachToWindow failed: %s\n", i + 1, vulkanModule.lastError().c_str());
            windowModule.closeWindow(win->handle);
            continue;
        }

        DemoUi demoUi = buildDemoUi(i);
        dsbox_ui::WindowUiTree& tree = uiModule.createTree(win->handle, std::move(demoUi.root), &windowModule);

        win->driver = std::make_unique<dsbox_vulkan_gui::VulkanUiWindowDriver>(
            vulkanModule, win->surface, tree, shaderDirectory);

        if (!win->driver->lastError().empty())
            std::fprintf(stderr, "Window %d: VulkanUiWindowDriver: %s\n", i + 1, win->driver->lastError().c_str());

        // Only now does a real dsbox_ui::IUiRenderBackend exist for
        // this window (the driver's own VulkanUiRenderer) -- see
        // buildDemoUi()'s own comment on why headerLabel/cardImage
        // couldn't be finished any earlier. Segoe UI is Windows' own
        // default UI font; Consolas is the fallback loadFontOrFallback()
        // reaches for if that file can't be loaded -- reliably present
        // on any Windows install, which is what this demo (windows.h et
        // al.) already targets. VulkanUiRenderer bakes an ASCII glyph
        // atlas from the chosen .ttf (see its class comment's "Text
        // scope"), and uploads the checkerboard below as a real GPU
        // texture.
        dsbox_ui::IUiRenderBackend& backend = win->driver->renderBackend();

        const dsbox_ui::FontHandle headerFont = dsbox_ui::loadFontOrFallback(
            backend, "C:\\Windows\\Fonts\\segoeui.ttf", 22.0f, "C:\\Windows\\Fonts\\consola.ttf");

        if (headerFont == dsbox_ui::kInvalidFontHandle)
            std::fprintf(stderr, "Window %d: could not load Segoe UI or Consolas -- header text will not be drawn (%s)\n",
                         i + 1, win->driver->lastError().c_str());

        if (demoUi.headerLabel)
            demoUi.headerLabel->setFont(headerFont);

        if (demoUi.buttonLabel)
            demoUi.buttonLabel->setFont(headerFont);

        if (demoUi.textField)
            demoUi.textField->setFont(headerFont);

        if (demoUi.cardImage)
        {
            const std::vector<std::uint8_t> checkerboard = buildCheckerboardRgba8(64, 64, 8);
            const dsbox_ui::ImageHandle image = backend.registerImage(64, 64, checkerboard.data());
            demoUi.cardImage->setImage(image);
        }

        demoWindows.push_back(std::move(win));
    }

    if (demoWindows.empty())
    {
        std::fprintf(stderr, "No windows could be created -- exiting.\n");
        uiModule.onShutdown(context);
        vulkanModule.onShutdown(context);
        windowModule.onShutdown(context);
        return 1;
    }

    // One subscription serving every window, by design -- mirrors how
    // dsbox_ui::UiModule itself subscribes once per event type and
    // dispatches by handle, rather than one lambda per window.
    context.comm().registerEvent<dsbox::WindowHandle>(
        "window.closed",
        [&demoWindows](dsbox::WindowHandle closedHandle)
        {
            for (auto& win : demoWindows)
                if (win->handle == closedHandle)
                    win->closed.store(true, std::memory_order_relaxed);
        });

    // Each window's render loop runs on its own thread, independent of
    // WindowModule's control thread and of every other window's render
    // thread. windowSize() is a plain cached read (see its own doc) --
    // no per-window event subscription needed here to stay current.
    for (auto& win : demoWindows)
    {
        DemoWindow* w = win.get();
        w->renderThread = std::thread(
            [w, &windowModule]()
            {
                while (!w->closed.load(std::memory_order_relaxed))
                {
                    int width = 0, height = 0;
                    if (!windowModule.windowSize(w->handle, width, height))
                        break; // window gone

                    if (width <= 0 || height <= 0)
                    {
                        // Minimized (or a transient zero-size) -- onFrame()
                        // would just return immediately for this, which
                        // would otherwise spin this thread at 100% CPU
                        // doing nothing useful until the window is restored.
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        continue;
                    }

                    w->driver->onFrame(w->handle, width, height);
                }
            });
    }

    // This thread now just waits for every window to close.
    while (!std::all_of(demoWindows.begin(), demoWindows.end(),
                         [](const auto& win) { return win->closed.load(std::memory_order_relaxed); }))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    for (auto& win : demoWindows)
    {
        if (win->renderThread.joinable())
            win->renderThread.join(); // the loop above exits on its own once win->closed is set

        win->driver.reset();

        uiModule.destroyTree(win->handle);
        vulkanModule.detach(win->surface);
        windowModule.closeWindow(win->handle); // no-op if already closed
    }

    uiModule.onShutdown(context);
    vulkanModule.onShutdown(context);
    windowModule.onShutdown(context);

    return 0;
}
