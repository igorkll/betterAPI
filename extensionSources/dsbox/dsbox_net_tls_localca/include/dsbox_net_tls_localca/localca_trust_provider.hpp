#pragma once

#include <dsbox_net_tls_localca/api.hpp>

#include <dsbox_net_tls/certificate.hpp>
#include <dsbox_net_tls/trust_provider.hpp>

#include <dsbox/communication_manager.hpp>
#include <dsbox/overridable.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace dsbox_net_tls_localca
{
    // Which of the two conventional directories a resolveDir call is
    // asking about -- mirrors config/default/config.ini vs config/config.ini
    // (see dsbox_config::PersistentConfigStore::load's own two-argument
    // shape): Default is what ships with the application (read-only as
    // far as this module is concerned), Override is where a deployment
    // or a user adds their own CAs. Both are always read and their
    // certificates simply concatenated -- see LocalCaTrustProvider's own
    // docs; there is no per-file "this overrides that" relationship the
    // way an INI key can be shadowed.
    enum class TrustedCaDirKind
    {
        Default,
        Override,
    };

    // Signature for the "net.tls.localca.resolve_dir" (default name --
    // see LocalCaModuleOptions) dsbox::Overridable slot: given which
    // directory is being asked for, returns its absolute path. The
    // default fallback resolves it as "<executable's own directory>/config/default/trusted_ca"
    // or ".../config/trusted_ca" -- the same base-directory convention
    // launcher/main.cpp already uses for config/default/config.ini vs
    // config/config.ini, just reimplemented here rather than shared,
    // per the project's current decision to defer a shared filesystem-
    // base-path abstraction and use Overridable for this in the
    // meantime. Override this to point somewhere else entirely (a
    // different --pathmode-style convention, a fixed deployment path,
    // ...) without touching how the directories are actually read.
    using ResolveTrustedCaDirFn = std::function<std::string(TrustedCaDirKind)>;
    using ResolveTrustedCaDirSignature = std::string(TrustedCaDirKind);

    // One file found in a trusted-CA directory, as raw bytes -- this
    // module classifies PEM vs DER and splits multi-certificate PEM
    // bundles itself (see parseCertificatesFromFileContent below); an
    // override only ever has to hand back file contents, never parse
    // anything.
    struct CaDirectoryFile
    {
        std::string fileName; // for diagnostics only
        std::vector<std::byte> data;
    };

    // Signature for the "net.tls.localca.read_dir" (default name -- see
    // LocalCaModuleOptions) dsbox::Overridable slot: given a directory
    // path (as resolveDir above produced), returns every *.pem/*.der/*.crt
    // file in it (non-recursive). A missing directory is NOT an error --
    // both conventional directories are optional (a deployment may add
    // custom CAs to only one of them, or neither) -- the default
    // fallback returns an empty list for it, same as for an empty
    // directory.
    using ReadCaDirectoryFn = std::function<std::vector<CaDirectoryFile>(std::string)>;
    using ReadCaDirectorySignature = std::vector<CaDirectoryFile>(std::string);

    // Classifies `fileContent` as PEM or DER and returns one
    // dsbox_net_tls::Certificate per certificate found in it:
    // - If it contains at least one "-----BEGIN CERTIFICATE-----" marker,
    //   it is treated as PEM and split into one Certificate per
    //   BEGIN/END CERTIFICATE block (other PEM block types -- private
    //   keys, CSRs -- are silently skipped; a bundle file with more than
    //   just certificates in it is unusual but not invalid).
    // - Otherwise the whole content is treated as one DER-encoded
    //   certificate.
    // Exposed standalone for testing, and because it is the one piece of
    // this module with no filesystem or dsbox::Overridable involved at
    // all.
    DSBOX_NET_TLS_LOCALCA_API std::vector<dsbox_net_tls::Certificate> parseCertificatesFromFileContent(
        std::span<const std::byte> fileContent);

    // An dsbox_net_tls::ITrustProvider backed by the two conventional
    // trusted-CA directories (config/default/trusted_ca and
    // config/trusted_ca -- see TrustedCaDirKind's own docs), using
    // dsbox::Overridable for both the directory-resolution and the
    // directory-reading step, exactly like
    // dsbox_net_file::FileProtocolHandler uses one for disk access. Both
    // directories are always read and their certificates concatenated;
    // this class has no notion of a UseSystemCA-style toggle -- that
    // decision belongs to whichever OTHER provider reads the OS trust
    // store (see dsbox_net_tls_systemca), not to this one.
    class DSBOX_NET_TLS_LOCALCA_API LocalCaTrustProvider final : public dsbox_net_tls::ITrustProvider
    {
    public:
        LocalCaTrustProvider(
            dsbox::CommunicationManager& comm,
            std::string resolveDirOverrideName,
            std::string readDirOverrideName);

        std::vector<dsbox_net_tls::Certificate> trustedCertificates() const override;

    private:
        dsbox::Overridable<ResolveTrustedCaDirSignature> m_resolveDir;
        dsbox::Overridable<ReadCaDirectorySignature> m_readDir;
    };
}
