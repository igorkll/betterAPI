#pragma once

#include <dsbox_net_file/api.hpp>

#include <dsbox_net/protocol_handler.hpp>

#include <dsbox/module.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dsbox_net_file
{
    // How a file:// URL's path component is interpreted as a native
    // filesystem path. Deliberately configuration, not autodetection:
    // an autodetected "does this look like a Windows path" is exactly
    // the kind of ambiguity that turns into a path-traversal or
    // wrong-file surprise on whichever platform didn't expect it, so
    // the project decides which forms are even considered, per host.
    enum class FilePathStyle
    {
        // Not a real style on its own -- means "resolve one of the
        // other three values from [net.file].path_style in a
        // dsbox_config::PersistentConfigStore, if one is available in
        // the Context by FileModule::onInit() time; PosixOnly if not"
        // (see FileModule's own docs for the exact key). This is
        // FileModuleOptions::pathStyle's own default -- pass anything
        // else to bypass configuration entirely and always use exactly
        // that style. Only meaningful there: passing Default straight
        // to FileProtocolHandler's constructor is a programming error
        // (see its own docs) since that class never looks at
        // configuration at all -- resolution must already have
        // happened, in FileModule::onInit().
        Default,

        // "/home/user/file.txt" style paths only: the URL path,
        // percent-decoded, is used exactly as-is. A path that happens
        // to look like a Windows drive ("/C:/...") is NOT
        // special-cased in this mode -- it is passed straight to the
        // filesystem as a literal (and, on a POSIX filesystem, almost
        // certainly nonexistent) path.
        PosixOnly,

        // Recognizes a URL path of the form "/<letter>:" or
        // "/<letter>:/..." and converts it to a Windows drive path
        // ("/C:/Users/x" -> "C:\Users\x"). Every other path is still
        // treated as POSIX -- use WindowsOnly instead if POSIX paths
        // should be refused entirely rather than silently accepted
        // alongside Windows ones.
        Both,

        // Only Windows drive paths (see Both's own docs for the exact
        // pattern) are accepted; anything else fails to resolve to a
        // native path at all (FileProtocolHandler reports it as
        // StatusClass::NotFound, without even reaching the overridable
        // filesystem access -- see FileProtocolHandler's own docs).
        WindowsOnly,
    };

    struct FileModuleOptions
    {
        // Default (the default): resolved from [net.file].path_style
        // in onInit(), falling back to PosixOnly if there's no
        // PersistentConfigStore or the key is unrecognized -- see
        // FilePathStyle::Default's own docs. Anything else here skips
        // that lookup entirely: the module always uses exactly the
        // value given, regardless of what (if anything) is configured.
        FilePathStyle pathStyle = FilePathStyle::Default;

        // Name of the dsbox::Overridable slot (registered on the
        // shared dsbox::Context::comm() bus, exactly like
        // dsbox_io::IOModule's own stdout/stdin overrides) that
        // actually touches the filesystem. Override it -- e.g.
        // context.comm().registerHandler<dsbox_net_file::FileOpenSignature>(
        // overrideName, ...) -- to sandbox or fake disk access, most
        // usefully in tests. See FileProtocolHandler's own docs for the
        // exact signature this name is registered under.
        std::string overrideName = "net.file.open";
    };

    // Registers an dsbox_net::IProtocolHandler for the "file" scheme on
    // the dsbox_net::NetworkManager provided by dsbox_net::NetModule --
    // depends on "dsbox.net" (see dependencies()) for that.
    //
    // Configuration: FileModuleOptions passed to the constructor are
    // used as-is when `pathStyle` is anything other than Default --
    // configuration is never consulted in that case. When it IS Default
    // (FileModuleOptions::pathStyle's own default), onInit() resolves it
    // from the [net.file] section of a dsbox_config::PersistentConfigStore,
    // IF one is available in the Context by then (see
    // dsbox_net::NetModule's own docs for why that's already guaranteed
    // regardless of module dependency order), falling back to PosixOnly
    // if there's no store or the value is unrecognized. Recognized key,
    // case-sensitive:
    //
    //   [net.file]
    //   path_style = posix | windows | both   (only consulted for Default -- see above)
    //
    // "data:" URLs are explicitly out of scope for this module -- they
    // will be handled once a ResourceManager exists to own that
    // decision (see the project's own notes on this); until then a
    // data: fetch simply gets NetErrorCode::UnsupportedScheme, same as
    // any other scheme nothing has registered a handler for.
    class DSBOX_NET_FILE_API FileModule final : public dsbox::IModule
    {
    public:
        FileModule();
        explicit FileModule(FileModuleOptions options);
        ~FileModule() override;

        std::string_view name() const override
        {
            return "dsbox.net.file";
        }

        std::vector<std::string> dependencies() const override
        {
            return { "dsbox.net" };
        }

        void onInit(dsbox::Context& context) override;
        void onStop(dsbox::Context& context) override;

    private:
        FileModuleOptions m_initialOptions;
        std::shared_ptr<dsbox_net::IProtocolHandler> m_handler;
    };
}
