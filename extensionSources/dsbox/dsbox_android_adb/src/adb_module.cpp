#include <dsbox_android_adb/adb_module.hpp>

#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

#include <dsbox_config/persistent_config_store.hpp>
#include <dsbox_process/subprocess.hpp>
#include <dsbox_scheduler/scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace dsbox_android_adb
{
    namespace
    {
        AdbDeviceState deviceStateFromFirstToken(std::string_view firstToken)
        {
            if (firstToken == "device") return AdbDeviceState::Device;
            if (firstToken == "offline") return AdbDeviceState::Offline;
            if (firstToken == "unauthorized") return AdbDeviceState::Unauthorized;
            if (firstToken == "no") return AdbDeviceState::NoPermissions;
            if (firstToken == "bootloader") return AdbDeviceState::Bootloader;
            if (firstToken == "recovery") return AdbDeviceState::Recovery;
            return AdbDeviceState::Unknown;
        }

        std::vector<std::string_view> splitWhitespace(std::string_view line)
        {
            std::vector<std::string_view> tokens;

            std::size_t i = 0;
            while (i < line.size())
            {
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                    ++i;

                if (i >= line.size())
                    break;

                const std::size_t tokenStart = i;

                while (i < line.size() && line[i] != ' ' && line[i] != '\t')
                    ++i;

                tokens.push_back(line.substr(tokenStart, i - tokenStart));
            }

            return tokens;
        }

        std::string_view trim(std::string_view text)
        {
            while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1);

            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                text.remove_prefix(1);

            return text;
        }

        std::chrono::milliseconds parseMillis(const std::string& text, std::chrono::milliseconds fallback)
        {
            long long value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);

            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 0)
                return fallback;

            return std::chrono::milliseconds(value);
        }

        std::size_t parseSizeT(const std::string& text, std::size_t fallback)
        {
            unsigned long long value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);

            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                return fallback;

            return static_cast<std::size_t>(value);
        }

        // Runs `executable arguments...` to completion and returns
        // whatever it wrote to stdout. Throws std::runtime_error if the
        // process could not be spawned at all (propagated from
        // dsbox_process::Subprocess's constructor); a nonzero exit code
        // is not itself an error here -- the caller decides whether the
        // (possibly empty) output means anything.
        std::string runToCompletion(const std::string& executable, std::vector<std::string> arguments)
        {
            dsbox_process::SubprocessOptions options;
            options.executable = executable;
            options.arguments = std::move(arguments);

            dsbox_process::Subprocess process(options);

            std::string collected;
            std::mutex collectedMutex;

            process.onStdoutData([&](std::span<const std::byte> data)
            {
                std::lock_guard<std::mutex> lock(collectedMutex);
                collected.append(reinterpret_cast<const char*>(data.data()), data.size());
            });

            process.wait();

            std::lock_guard<std::mutex> lock(collectedMutex);
            return collected;
        }
    }

    std::vector<AdbDeviceInfo> parseAdbDevicesOutput(std::string_view output)
    {
        std::vector<AdbDeviceInfo> result;

        std::size_t pos = 0;
        while (pos <= output.size())
        {
            const std::size_t newline = output.find('\n', pos);
            const std::string_view rawLine = (newline == std::string_view::npos)
                ? output.substr(pos)
                : output.substr(pos, newline - pos);

            const std::string_view line = trim(rawLine);

            if (!line.empty() && line != "List of devices attached")
            {
                const auto tokens = splitWhitespace(line);

                if (tokens.size() >= 2)
                {
                    AdbDeviceInfo device;
                    device.serial = std::string(tokens[0]);

                    // The state is normally one token ("device",
                    // "offline", ...). "no permissions" is the one
                    // built-in exception that's two words on its own --
                    // swallow tokens after "no" up to (but not
                    // including) the first "key:value"-shaped token, so
                    // any parenthetical explanation adb appends
                    // ("no permissions (missing udev rules?)") ends up in
                    // rawState rather than misparsed as a property.
                    std::size_t stateEnd = 2;
                    if (tokens[1] == "no" && tokens.size() > 2)
                    {
                        stateEnd = 3;
                        while (stateEnd < tokens.size() && tokens[stateEnd].find(':') == std::string_view::npos)
                            ++stateEnd;
                    }

                    std::string rawState;
                    for (std::size_t t = 1; t < stateEnd; ++t)
                    {
                        if (!rawState.empty())
                            rawState += ' ';
                        rawState += tokens[t];
                    }

                    device.rawState = rawState;
                    device.state = deviceStateFromFirstToken(tokens[1]);

                    for (std::size_t t = stateEnd; t < tokens.size(); ++t)
                    {
                        const std::string_view token = tokens[t];
                        const auto colon = token.find(':');

                        if (colon == std::string_view::npos || colon == 0)
                            continue; // not a "key:value" token -- ignore it rather than guess

                        device.properties.emplace(
                            std::string(token.substr(0, colon)),
                            std::string(token.substr(colon + 1)));
                    }

                    result.push_back(std::move(device));
                }
            }

            if (newline == std::string_view::npos)
                break;

            pos = newline + 1;
        }

        return result;
    }

    struct AdbModule::Impl
    {
        explicit Impl(AdbOptions opts) : options(std::move(opts)) {}

        AdbOptions options;
        dsbox::Context* context = nullptr;

        mutable std::mutex devicesMutex;
        std::vector<AdbDeviceInfo> devicesSnapshot;

        // The shared backend behind device()/forEachDevice() and every
        // AdbDevice/AdbShell/AdbPackages handle this module hands out.
        // Built lazily, from whatever `options` holds at the moment of
        // the FIRST call that needs it -- by the time any caller is
        // likely to reach for device(), onInit() (which may have
        // overridden options.adbPath/commandThreads from config) has
        // already run, so this naturally picks up the configured values
        // without needing its own onInit() hook.
        mutable std::mutex commandExecutorMutex;
        mutable std::shared_ptr<AdbCommandExecutor> commandExecutorPtr;

        const std::shared_ptr<AdbCommandExecutor>& ensureCommandExecutor() const
        {
            std::lock_guard<std::mutex> lock(commandExecutorMutex);

            if (!commandExecutorPtr)
                commandExecutorPtr = std::make_shared<AdbCommandExecutor>(options.adbPath, options.commandThreads);

            return commandExecutorPtr;
        }

        // Device watch: exactly one of the two mechanisms below is
        // active at a time, chosen in startDeviceWatchLocked() depending
        // on whether the Context has a published Scheduler -- same
        // optional-scheduler convention as dsbox_io::IOModule's request
        // timeouts.
        std::atomic<bool> watchActive{ false };
        std::atomic<bool> usingSchedulerTimer{ false };
        dsbox_scheduler::ScopedTimer schedulerTimer;

        std::mutex fallbackWatchMutex;
        std::condition_variable fallbackWatchCv;
        bool fallbackWatchRunning = false;
        std::chrono::milliseconds fallbackWatchInterval{ 0 };
        std::thread fallbackWatchThread;

        struct LogcatEntry
        {
            std::unique_ptr<dsbox_process::Subprocess> process;
            std::string lineBuffer; // bytes received since the last complete line
        };

        mutable std::mutex logcatMutex;
        std::unordered_map<std::string, LogcatEntry> logcatEntries;

        std::vector<AdbDeviceInfo> refreshDevices();
        void emitDevicesDiff(std::vector<AdbDeviceInfo> newSnapshot);

        void startFallbackWatch(std::chrono::milliseconds interval);
        void stopFallbackWatch();

        void onLogcatData(const std::string& serial, std::span<const std::byte> data);
        void onLogcatStderrLine(const std::string& serial, std::string line);
        void onLogcatExit(const std::string& serial, int exitCode);
    };

    std::vector<AdbDeviceInfo> AdbModule::Impl::refreshDevices()
    {
        const std::string output = runToCompletion(options.adbPath, { "devices", "-l" });
        std::vector<AdbDeviceInfo> parsed = parseAdbDevicesOutput(output);
        emitDevicesDiff(parsed);
        return parsed;
    }

    void AdbModule::Impl::emitDevicesDiff(std::vector<AdbDeviceInfo> newSnapshot)
    {
        std::vector<AdbDeviceInfo> previous;
        {
            std::lock_guard<std::mutex> lock(devicesMutex);
            previous = devicesSnapshot;
            devicesSnapshot = newSnapshot;
        }

        std::unordered_set<std::string> previousSerials;
        previousSerials.reserve(previous.size());
        for (const auto& device : previous)
            previousSerials.insert(device.serial);

        std::unordered_set<std::string> newSerials;
        newSerials.reserve(newSnapshot.size());
        for (const auto& device : newSnapshot)
            newSerials.insert(device.serial);

        DevicesChangedEvent event;
        event.devices = newSnapshot;

        for (const auto& device : newSnapshot)
            if (!previousSerials.contains(device.serial))
                event.added.push_back(device);

        for (const auto& device : previous)
            if (!newSerials.contains(device.serial))
                event.removedSerials.push_back(device.serial);

        if (!context)
            return;

        context->comm().emit<DevicesChangedEvent>("adb.devices.changed", event);

        for (const auto& device : event.added)
            context->comm().emit<AdbDeviceInfo>("adb.device.connected", device);

        for (const auto& serial : event.removedSerials)
        {
            // Report the full record the device last had (model,
            // transport_id, ...) rather than just its serial, if we still
            // have it.
            const auto it = std::find_if(previous.begin(), previous.end(),
                [&](const AdbDeviceInfo& device) { return device.serial == serial; });

            context->comm().emit<AdbDeviceInfo>("adb.device.disconnected", it != previous.end() ? *it : AdbDeviceInfo{ serial });
        }
    }

    void AdbModule::Impl::startFallbackWatch(std::chrono::milliseconds interval)
    {
        {
            std::lock_guard<std::mutex> lock(fallbackWatchMutex);
            fallbackWatchRunning = true;
            fallbackWatchInterval = interval;
        }

        fallbackWatchThread = std::thread([this]
        {
            std::unique_lock<std::mutex> lock(fallbackWatchMutex);

            while (fallbackWatchRunning)
            {
                const auto interval = fallbackWatchInterval;

                fallbackWatchCv.wait_for(lock, interval, [this] { return !fallbackWatchRunning; });

                if (!fallbackWatchRunning)
                    break;

                lock.unlock();

                try
                {
                    refreshDevices();
                }
                catch (...)
                {
                    // A transient failure (adb momentarily unavailable,
                    // etc.) must not stop the watch -- same contract as
                    // the scheduler-timer path below.
                }

                lock.lock();
            }
        });
    }

    void AdbModule::Impl::stopFallbackWatch()
    {
        {
            std::lock_guard<std::mutex> lock(fallbackWatchMutex);
            if (!fallbackWatchRunning)
                return;

            fallbackWatchRunning = false;
        }

        fallbackWatchCv.notify_all();

        if (fallbackWatchThread.joinable())
            fallbackWatchThread.join();
    }

    void AdbModule::Impl::onLogcatData(const std::string& serial, std::span<const std::byte> data)
    {
        std::vector<std::string> lines;

        {
            std::lock_guard<std::mutex> lock(logcatMutex);

            const auto it = logcatEntries.find(serial);
            if (it == logcatEntries.end())
                return; // stopLogcat() already removed this entry -- drop the trailing chunk

            std::string& buffer = it->second.lineBuffer;
            buffer.append(reinterpret_cast<const char*>(data.data()), data.size());

            std::size_t start = 0;
            while (true)
            {
                const std::size_t newline = buffer.find('\n', start);
                if (newline == std::string::npos)
                    break;

                std::string line = buffer.substr(start, newline - start);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                lines.push_back(std::move(line));
                start = newline + 1;
            }

            buffer.erase(0, start);
        }

        if (!context)
            return;

        for (auto& line : lines)
            context->comm().emit<LogcatLineEvent>("adb.logcat.line", LogcatLineEvent{ serial, std::move(line) });
    }

    void AdbModule::Impl::onLogcatStderrLine(const std::string& serial, std::string line)
    {
        // Unlike onLogcatData() (stdout), dsbox_process::Subprocess
        // already splits stderr into complete lines for us -- see
        // Subprocess::onStderrLine()'s own docs -- so there is no buffer
        // to maintain here. This is the only place adb's own diagnostics
        // ("error: device 'X' not found", "error: more than one device/emulator",
        // a logcat crash message, ...) become visible at all: they never
        // appear on stdout, and LogcatStoppedEvent carries only a bare
        // exit code.
        if (!context)
            return;

        context->comm().emit<LogcatLineEvent>("adb.logcat.stderr_line", LogcatLineEvent{ serial, std::move(line) });
    }

    void AdbModule::Impl::onLogcatExit(const std::string& serial, int exitCode)
    {
        // Deliberately does not touch logcatEntries: this runs on
        // dsbox_process::Subprocess's own internal waiter thread, and
        // erasing the map entry here would destroy that same Subprocess
        // from within its own callback, which joins that very thread --
        // see stopLogcat() and the two shutdown paths, which are the
        // only places an entry is ever removed, always from a different
        // thread. A process that exited on its own therefore lingers in
        // the map (fully exited, harmless) until one of those reaps it;
        // isLogcatRunning()/activeLogcatSerials() check isRunning() so
        // they report it correctly in the meantime.
        if (context)
            context->comm().emit<LogcatStoppedEvent>("adb.logcat.stopped", LogcatStoppedEvent{ serial, exitCode });
    }

    AdbModule::AdbModule() : AdbModule(AdbOptions{})
    {
    }

    AdbModule::AdbModule(AdbOptions options)
        : m_impl(std::make_unique<Impl>(std::move(options)))
    {
    }

    AdbModule::~AdbModule()
    {
        stopDeviceWatch();

        std::unordered_map<std::string, Impl::LogcatEntry> entries;
        {
            std::lock_guard<std::mutex> lock(m_impl->logcatMutex);
            entries.swap(m_impl->logcatEntries);
        }
        // `entries` goes out of scope here, on the caller's thread:
        // every still-running logcat subprocess is forcibly terminated
        // by its own destructor (see dsbox_process::Subprocess's docs),
        // and an already-exited one is simply freed.
    }

    void AdbModule::onInit(dsbox::Context& context)
    {
        m_impl->context = &context;

        if (auto config = context.get<dsbox_config::PersistentConfigStore>())
        {
            m_impl->options.adbPath = config->getOr("android.adb", "adb_path", m_impl->options.adbPath);

            m_impl->options.deviceWatchInterval = parseMillis(
                config->getOr(
                    "android.adb",
                    "device_watch_interval_ms",
                    std::to_string(m_impl->options.deviceWatchInterval.count())),
                m_impl->options.deviceWatchInterval);

            m_impl->options.autoStartDeviceWatch = config->getBool(
                "android.adb", "auto_start_device_watch", m_impl->options.autoStartDeviceWatch);

            m_impl->options.commandThreads = parseSizeT(
                config->getOr(
                    "android.adb",
                    "command_threads",
                    std::to_string(m_impl->options.commandThreads)),
                m_impl->options.commandThreads);
        }
    }

    void AdbModule::onStart(dsbox::Context& context)
    {
        (void)context;

        if (m_impl->options.autoStartDeviceWatch && m_impl->options.deviceWatchInterval.count() > 0)
            startDeviceWatch(m_impl->options.deviceWatchInterval);
    }

    void AdbModule::onStop(dsbox::Context& context)
    {
        (void)context;

        stopDeviceWatch();

        std::unordered_map<std::string, Impl::LogcatEntry> entries;
        {
            std::lock_guard<std::mutex> lock(m_impl->logcatMutex);
            entries.swap(m_impl->logcatEntries);
        }
        // Same reasoning as the destructor: destroyed here, on whatever
        // thread called onStop(), never on a Subprocess's own callback
        // thread.
    }

    void AdbModule::onShutdown(dsbox::Context&)
    {
        // onStop() already stopped the watch and reaped every logcat
        // subprocess; this module holds nothing else of its own --
        // commandExecutorPtr (if ever created) is left alone, since
        // AdbDevice handles already handed out are documented to keep
        // working past this module's own shutdown. Clear the Context
        // pointer defensively so a callback that somehow outlived
        // teardown finds no-one to emit through instead of touching a
        // Context that may itself be going away.
        m_impl->context = nullptr;
    }

    std::vector<AdbDeviceInfo> AdbModule::refreshDevices()
    {
        return m_impl->refreshDevices();
    }

    std::vector<AdbDeviceInfo> AdbModule::devices() const
    {
        std::lock_guard<std::mutex> lock(m_impl->devicesMutex);
        return m_impl->devicesSnapshot;
    }

    void AdbModule::startDeviceWatch(std::chrono::milliseconds interval)
    {
        if (interval.count() <= 0)
            throw std::invalid_argument("dsbox_android_adb: device watch interval must be > 0");

        if (m_impl->watchActive.exchange(true))
            return; // already watching

        std::shared_ptr<dsbox_scheduler::Scheduler> scheduler =
            m_impl->context ? m_impl->context->get<dsbox_scheduler::Scheduler>() : nullptr;

        if (scheduler)
        {
            Impl* impl = m_impl.get();

            auto handle = scheduler->every(
                interval,
                [impl]
                {
                    try
                    {
                        impl->refreshDevices();
                    }
                    catch (...)
                    {
                        // See startFallbackWatch(): a failed tick must
                        // not stop the watch.
                    }
                },
                dsbox_scheduler::PeriodicPolicy::FixedDelay);

            m_impl->schedulerTimer.reset(handle);
            m_impl->usingSchedulerTimer.store(true);
        }
        else
        {
            m_impl->usingSchedulerTimer.store(false);
            m_impl->startFallbackWatch(interval);
        }
    }

    void AdbModule::startDeviceWatch()
    {
        startDeviceWatch(m_impl->options.deviceWatchInterval);
    }

    void AdbModule::stopDeviceWatch()
    {
        if (m_impl->usingSchedulerTimer.exchange(false))
        {
            dsbox_scheduler::TimerHandle handle = m_impl->schedulerTimer.release();

            // Waits for a tick that is already in flight, so once this
            // returns refreshDevices() is guaranteed not to be touching
            // this Impl any more -- important since the destructor may
            // be about to free it.
            if (handle)
                handle.cancelAndWait();
        }
        else
        {
            m_impl->stopFallbackWatch();
        }

        m_impl->watchActive.store(false);
    }

    bool AdbModule::isWatchingDevices() const
    {
        return m_impl->watchActive.load();
    }

    void AdbModule::startLogcat(std::string serial, std::vector<std::string> extraArgs)
    {
        {
            std::lock_guard<std::mutex> lock(m_impl->logcatMutex);

            const auto it = m_impl->logcatEntries.find(serial);
            if (it != m_impl->logcatEntries.end())
            {
                if (it->second.process->isRunning())
                    throw std::runtime_error("dsbox_android_adb: logcat is already running for serial '" + serial + "'");

                // A previous logcat for this serial exited on its own
                // and was never reaped with stopLogcat() -- reap it now
                // so this call can proceed instead of spuriously
                // throwing "already running".
                m_impl->logcatEntries.erase(it);
            }
        }

        dsbox_process::SubprocessOptions options;
        options.executable = m_impl->options.adbPath;
        options.arguments = { "-s", serial, "logcat" };
        for (auto& arg : extraArgs)
            options.arguments.push_back(std::move(arg));

        auto process = std::make_unique<dsbox_process::Subprocess>(options);

        Impl* impl = m_impl.get();
        const std::string serialCopy = serial;

        process->onStdoutData([impl, serialCopy](std::span<const std::byte> data)
        {
            impl->onLogcatData(serialCopy, data);
        });

        process->onStderrLine([impl, serialCopy](std::string line)
        {
            impl->onLogcatStderrLine(serialCopy, std::move(line));
        });

        process->onExit([impl, serialCopy](int exitCode)
        {
            impl->onLogcatExit(serialCopy, exitCode);
        });

        std::lock_guard<std::mutex> lock(m_impl->logcatMutex);

        // Re-check under lock: two concurrent startLogcat() calls for the
        // same serial could both pass the check above. Whichever loses
        // this race throws here; its already-spawned `process` is then
        // destroyed (forcibly terminated) as this function returns.
        if (m_impl->logcatEntries.contains(serial))
            throw std::runtime_error("dsbox_android_adb: logcat is already running for serial '" + serial + "'");

        m_impl->logcatEntries.emplace(std::move(serial), Impl::LogcatEntry{ std::move(process), {} });
    }

    void AdbModule::stopLogcat(std::string_view serial)
    {
        Impl::LogcatEntry entry;
        bool found = false;

        {
            std::lock_guard<std::mutex> lock(m_impl->logcatMutex);

            const auto it = m_impl->logcatEntries.find(std::string(serial));
            if (it != m_impl->logcatEntries.end())
            {
                entry = std::move(it->second);
                m_impl->logcatEntries.erase(it);
                found = true;
            }
        }

        // `entry` is destroyed here (on the caller's thread, never on
        // the Subprocess's own callback thread -- see Impl::onLogcatExit).
        // Its destructor forcibly terminates the process if it was still
        // running, or is a cheap no-op if it had already exited.
        (void)found;
    }

    bool AdbModule::isLogcatRunning(std::string_view serial) const
    {
        std::lock_guard<std::mutex> lock(m_impl->logcatMutex);

        const auto it = m_impl->logcatEntries.find(std::string(serial));
        return it != m_impl->logcatEntries.end() && it->second.process->isRunning();
    }

    std::vector<std::string> AdbModule::activeLogcatSerials() const
    {
        std::vector<std::string> result;

        std::lock_guard<std::mutex> lock(m_impl->logcatMutex);

        for (const auto& [serial, entry] : m_impl->logcatEntries)
            if (entry.process->isRunning())
                result.push_back(serial);

        return result;
    }

    AdbDevice AdbModule::device(std::string serial) const
    {
        return AdbDevice(m_impl->ensureCommandExecutor(), std::move(serial));
    }

    const std::shared_ptr<AdbCommandExecutor>& AdbModule::commandExecutor() const
    {
        return m_impl->ensureCommandExecutor();
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_android_adb::AdbModule)
