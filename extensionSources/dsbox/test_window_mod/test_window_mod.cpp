#include <dsbox/module.hpp>
#include <dsbox/context.hpp>
#include <dsbox/plugin_export.hpp>

#include <dsbox_sdl/window_module.hpp>
#include <dsbox_vulkan/vulkan_module.hpp>
#include <dsbox_ui/container.hpp>
#include <dsbox_ui/stack_layout.hpp>
#include <dsbox_ui/text_field.hpp>
#include <dsbox_ui/button.hpp>
#include <dsbox_ui/text.hpp>
#include <dsbox_ui/window_ui_tree.hpp>
#include <dsbox_vulkan_gui/vulkan_ui_window_driver.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>     // Для std::cout
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>    // Для AllocConsole
#endif

class TestWindowMod final : public dsbox::IModule
{
public:
    std::string_view name() const override { return "test.window.mod"; }

    void onStart(dsbox::Context& context) override
    {
        // 1. Инициализируем модули
        m_windowModule.onInit(context);
        m_vulkanModule.onInit(context);
        m_uiModule.onInit(context);

        // 2. Создаем окно
        dsbox_sdl::WindowOptions opts;
        opts.title = "DSBox Mod - TextField & Button";
        opts.width = 400;
        opts.height = 250;
        opts.graphicsHint = dsbox_sdl::WindowGraphicsHint::Vulkan;
        m_windowHandle = m_windowModule.createWindow(opts);

        if (m_windowHandle == dsbox::kInvalidWindowHandle) return;

        // 3. Создаем Vulkan Surface
        m_surface = m_vulkanModule.attachToWindow(m_windowModule, m_windowHandle);
        if (m_surface == dsbox_vulkan::kInvalidSurfaceHandle) return;

        // 4. Строим UI дерево
        auto root = std::make_unique<dsbox_ui::Container>(1, dsbox_ui::UiElementIdMode::Unique, "root");
        root->setBackgroundColor(dsbox_ui::UiColor{ 0.15f, 0.15f, 0.18f, 1.0f });
        root->setPreferredWidth(dsbox_ui::UiLength::percent(100.0f));
        root->setPreferredHeight(dsbox_ui::UiLength::percent(100.0f));
        root->setPadding(30.0f, 30.0f, 30.0f, 30.0f);
        root->setLayoutStrategy(std::make_unique<dsbox_ui::StackLayout>(dsbox_ui::StackDirection::Vertical, 20.0f));

        // --- ПОЛЕ ВВОДА ТЕКСТА ---
        auto& textField = static_cast<dsbox_ui::TextField&>(
            root->addChild(std::make_unique<dsbox_ui::TextField>(2, dsbox_ui::UiElementIdMode::Unique, "input"))
            );
        textField.setPreferredWidth(dsbox_ui::UiLength::pixels(300.0f));
        textField.setPreferredHeight(dsbox_ui::UiLength::pixels(36.0f));
        textField.setPlaceholder("Enter the text...");

        // --- КНОПКА С ТЕКСТОМ "print" ---
        auto buttonParts = dsbox_ui::makeButton(3, 4, "print", dsbox_ui::UiElementIdMode::Unique);
        buttonParts.root->setPreferredWidth(dsbox_ui::UiLength::pixels(300.0f));

        dsbox_ui::TextField* tfPtr = &textField;

        // ОБРАБОТЧИК КЛИКА
        buttonParts.root->setOnClick([tfPtr]() {
            // 1. Создаем консольное окно при первом клике (один раз за сессию)
            static bool consoleAllocated = false;
            if (!consoleAllocated) {
#if defined(_WIN32)
                AllocConsole();
                FILE* fp;
                freopen_s(&fp, "CONOUT$", "w", stdout);
                freopen_s(&fp, "CONOUT$", "w", stderr);
                // Перенаправляем std::cout в новую консоль
                std::ios::sync_with_stdio(true);
#endif
                consoleAllocated = true;
            }

            // 2. Выводим через std::cout (как ты и просил)
            std::string textFromField = tfPtr->text();
            std::cout << "[DSBox Mod] " << textFromField << std::endl;

            // 3. Для надежности дублируем в файл, если вдруг консоль не перехватится игрой
            std::ofstream logFile("dsbox/test_mod_click_log.txt", std::ios::app);
            if (logFile.is_open()) {
                logFile << "[Click] Text: '" << textFromField << "'\n";
            }
            });

        root->addChild(std::move(buttonParts.root));

        // 5. Создаем Driver
        m_tree = &m_uiModule.createTree(m_windowHandle, std::move(root), &m_windowModule);

        std::string shaderDir = findShaderDirectory();
        m_driver = std::make_unique<dsbox_vulkan_gui::VulkanUiWindowDriver>(
            m_vulkanModule, m_surface, *m_tree, shaderDir
        );

        // 6. Загружаем шрифт
        dsbox_ui::IUiRenderBackend& backend = m_driver->renderBackend();
        dsbox_ui::FontHandle font = dsbox_ui::kInvalidFontHandle;

        std::vector<std::string> fontPaths = {
            "C:\\Windows\\Fonts\\consolas.ttf",
            "C:\\Windows\\Fonts\\consola.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf"
        };

        for (const auto& fp : fontPaths) {
            font = backend.loadFont(fp, 16.0f);
            if (font != dsbox_ui::kInvalidFontHandle) break;
        }

        textField.setFont(font);
        if (buttonParts.label) {
            buttonParts.label->setFont(font);
            buttonParts.label->setColor(dsbox_ui::UiColor{ 1.0f, 1.0f, 1.0f, 1.0f });
        }

        // 7. Запускаем поток рендеринга
        m_running.store(true);
        m_renderThread = std::thread([this]() {
            try {
                while (m_running.load()) {
                    int w = 0, h = 0;
                    if (!m_windowModule.windowSize(m_windowHandle, w, h)) break;

                    if (w > 0 && h > 0) {
                        m_driver->onFrame(m_windowHandle, w, h);
                    }
                    else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            catch (...) {}
            });
    }

    void onStop(dsbox::Context& context) override
    {
        m_running.store(false);
        if (m_renderThread.joinable()) m_renderThread.join();

        if (m_tree) m_uiModule.destroyTree(m_windowHandle);
        if (m_surface != dsbox_vulkan::kInvalidSurfaceHandle) m_vulkanModule.detach(m_surface);
        if (m_windowHandle != dsbox::kInvalidWindowHandle) m_windowModule.closeWindow(m_windowHandle);

        m_driver.reset();

        m_uiModule.onShutdown(context);
        m_vulkanModule.onShutdown(context);
        m_windowModule.onShutdown(context);
    }

private:
    std::string findShaderDirectory()
    {
#if defined(_WIN32)
        static const char kModuleAnchor = 0;
        HMODULE hm = NULL;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&kModuleAnchor),
            &hm
        );
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(hm, path, MAX_PATH);
        std::filesystem::path dllDir = std::filesystem::path(path).parent_path();

        auto shaderPath = dllDir / ".." / "shaders" / "dsbox_vulkan_gui";
        if (std::filesystem::exists(shaderPath)) return shaderPath.string();

        auto cwdPath = std::filesystem::current_path() / "dsbox" / "shaders" / "dsbox_vulkan_gui";
        if (std::filesystem::exists(cwdPath)) return cwdPath.string();

        return dllDir.string();
#else
        return (std::filesystem::current_path() / "shaders" / "dsbox_vulkan_gui").string();
#endif
    }

    dsbox_sdl::WindowModule m_windowModule;
    dsbox_vulkan::VulkanModule m_vulkanModule;
    dsbox_ui::UiModule m_uiModule;

    dsbox::WindowHandle m_windowHandle = dsbox::kInvalidWindowHandle;
    dsbox_vulkan::SurfaceHandle m_surface = dsbox_vulkan::kInvalidSurfaceHandle;

    dsbox_ui::WindowUiTree* m_tree = nullptr;
    std::unique_ptr<dsbox_vulkan_gui::VulkanUiWindowDriver> m_driver;

    std::thread m_renderThread;
    std::atomic<bool> m_running{ false };
};

// --- Экспорты ---
DSBOX_PLUGIN_EXPORT dsbox::IModule* DSBox_CreateModule() { return new TestWindowMod(); }
DSBOX_PLUGIN_EXPORT void DSBox_DestroyModule(dsbox::IModule* module) { delete module; }