#include <dsbox_net_file/file_module.hpp>
#include <dsbox_net_file/file_protocol_handler.hpp>

#include <dsbox_net/network_manager.hpp>

// PRIVATE dependency, exactly like dsbox_net::NetModule's own use of
// this: only this .cpp knows about dsbox_config. See FileModule's own
// docs for the recognized [net.file] key.
#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

#include <stdexcept>

namespace dsbox_net_file
{
    FileModule::FileModule()
        : FileModule(FileModuleOptions{})
    {
    }

    FileModule::FileModule(FileModuleOptions options)
        : m_initialOptions(std::move(options))
    {
    }

    FileModule::~FileModule() = default;

    void FileModule::onInit(dsbox::Context& context)
    {
        FileModuleOptions options = m_initialOptions;

        FilePathStyle resolvedStyle = options.pathStyle;

        if (resolvedStyle == FilePathStyle::Default)
        {
            // Hard fallback when nothing is configured at all -- see
            // FilePathStyle::Default's own docs.
            resolvedStyle = FilePathStyle::PosixOnly;

            if (auto config = context.get<dsbox_config::PersistentConfigStore>())
            {
                const auto styleText = config->getOr("net.file", "path_style", "posix");

                if (styleText == "posix")
                    resolvedStyle = FilePathStyle::PosixOnly;
                else if (styleText == "windows")
                    resolvedStyle = FilePathStyle::WindowsOnly;
                else if (styleText == "both")
                    resolvedStyle = FilePathStyle::Both;
                // else: unrecognized value -- keep the PosixOnly fallback
                // above, silently (a blank/garbled config value should
                // never crash module startup).
            }
        }

        auto net = context.get<dsbox_net::NetworkManager>();

        if (!net)
        {
            throw std::runtime_error(
                "dsbox_net_file: no dsbox_net::NetworkManager in Context -- "
                "is dsbox_net::NetModule registered as a dependency?");
        }

        auto handler = std::make_shared<FileProtocolHandler>(
            context.comm(),
            options.overrideName,
            resolvedStyle);

        m_handler = handler;
        net->registerHandler("file", handler, this);
    }

    void FileModule::onStop(dsbox::Context& context)
    {
        if (auto net = context.get<dsbox_net::NetworkManager>())
            net->unregisterOwner(this);

        m_handler.reset();
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_net_file::FileModule)
