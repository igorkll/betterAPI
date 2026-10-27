#pragma once

#include <dsbox_net_file/api.hpp>
#include <dsbox_net_file/file_module.hpp>

#include <dsbox_net/body.hpp>
#include <dsbox_net/protocol_handler.hpp>

#include <dsbox/communication_manager.hpp>
#include <dsbox/overridable.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dsbox_net_file
{
    // What the overridable filesystem-open operation reports back.
    // Movable-only (owns a dsbox_net::IByteStream), like
    // dsbox_net::FetchResult -- never copy one, always move.
    struct FileOpenResult
    {
        // What actually went wrong, if anything. NotFound/Denied are
        // NOT delivery failures as far as dsbox_net::NetworkManager is
        // concerned -- they become a normally-delivered Response with
        // the matching StatusClass, the same way an HTTP handler turns
        // a 404 into a Response rather than a NetError (see
        // dsbox_net::Response's own docs on that distinction). Other is
        // for anything that doesn't fit those two (a directory where a
        // file was expected, an I/O error, ...) and DOES become a
        // dsbox_net::NetError.
        enum class Failure
        {
            None,
            NotFound,
            Denied,
            Other,
        };

        // Non-null on success (Failure::None); null otherwise.
        std::unique_ptr<dsbox_net::IByteStream> stream;

        // Known ahead of time on success where the override can cheaply
        // determine it (e.g. via std::filesystem::file_size);
        // std::nullopt if not.
        std::optional<std::uint64_t> size;

        Failure failure = Failure::None;

        // Human-readable detail, used as Response::statusText for
        // NotFound/Denied and as part of the NetError message for
        // Other. Empty for Failure::None.
        std::string message;
    };

    // Signature the "net.file.open" (default name -- see
    // FileModuleOptions::overrideName) dsbox::Overridable slot is
    // registered under: takes the NATIVE filesystem path already
    // resolved from the file:// URL (see nativePathFromUrlPath below --
    // FileProtocolHandler does that resolution itself before calling
    // this, so an override never has to deal with URL syntax, percent-
    // encoding, or FilePathStyle), and reports a FileOpenResult.
    //
    // An override is free to serve entirely fake content (a canned
    // in-memory filesystem for tests) or to wrap the real one with
    // added policy (e.g. refusing paths outside some sandbox root) by
    // delegating to the default implementation for paths it allows --
    // see FileProtocolHandler's constructor for how to get a
    // std::function that does exactly that.
    using FileOpenFn = std::function<FileOpenResult(std::string path)>;
    using FileOpenSignature = FileOpenResult(std::string);

    // Converts a file:// URL's path component (percent-encoded, as
    // Url::path stores it) to a native filesystem path per `style`.
    // Returns std::nullopt if `urlPath` doesn't parse as a path at all
    // (doesn't start with '/' once decoded -- shouldn't happen for a
    // URL that came from Url::parse, which always gives file:// an
    // absolute path), doesn't match what `style` requires (e.g. a
    // non-drive path under WindowsOnly), or `style` is
    // FilePathStyle::Default (not a real style -- see its own docs;
    // this function has no configuration to resolve it against, so
    // passing it here is the same kind of misuse as passing it to
    // FileProtocolHandler's constructor, just reported as "no path"
    // rather than by throwing). Exposed standalone for testing, and
    // because a caller assembling a file:// URL from a native path may
    // find the inverse direction useful too (not provided here -- out
    // of scope for this module for now).
    DSBOX_NET_FILE_API std::optional<std::string> nativePathFromUrlPath(
        std::string_view urlPath,
        FilePathStyle style);

    // Answers file:// requests, using dsbox::Overridable<FileOpenSignature>
    // for the actual filesystem access -- see FileModule's own docs for
    // the override slot's default name and how to replace it.
    //
    // `pathStyle` must already be a concrete value (PosixOnly/Both/
    // WindowsOnly) by the time it reaches this class -- FileProtocolHandler
    // has no access to configuration and does not know how to resolve
    // FilePathStyle::Default itself. Passing Default here is a
    // programming error: the constructor throws std::invalid_argument.
    // FileModule::onInit() is the one place that resolution happens.
    //
    // Only GET-shaped requests make sense for a local file: `start()`
    // ignores Request::method and Request::body entirely (there is
    // nothing to "post" to a file:// URL in this module -- writing to
    // local files is a different, not-yet-designed part of this
    // project, not something this read-only handler grows into).
    // Request::query and Request::fragment are likewise ignored (a
    // fragment never reaches a server for any scheme; a query string on
    // a file:// URL has no defined meaning here).
    class DSBOX_NET_FILE_API FileProtocolHandler final : public dsbox_net::IProtocolHandler
    {
    public:
        FileProtocolHandler(
            dsbox::CommunicationManager& comm,
            std::string overrideName,
            FilePathStyle pathStyle);

        void start(dsbox_net::Request request, dsbox_net::FetchCallback done) override;

    private:
        dsbox::Overridable<FileOpenSignature> m_open;
        FilePathStyle m_pathStyle;
    };
}
