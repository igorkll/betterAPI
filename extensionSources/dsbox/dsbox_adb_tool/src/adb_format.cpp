#include "adb_format.hpp"

#include <dsbox_android_adb/adb_module.hpp>
#include <dsbox_android_adb/device.hpp>

#include <algorithm>

namespace dsbox_adb_tool
{
    using dsbox_android_adb::AdbCommandResult;
    using dsbox_android_adb::AdbDeviceInfo;
    using dsbox_android_adb::AdbModule;

    std::string formatDeviceLine(const AdbDeviceInfo& device)
    {
        std::string line = device.serial;
        line.append(line.size() < 24 ? 24 - line.size() : 1, ' ');

        std::string state = dsbox_android_adb::toString(device.state);
        line += state;
        line.append(state.size() < 14 ? 14 - state.size() : 1, ' ');

        const std::string model = device.property("model");
        const std::string product = device.property("product");
        const std::string transportId = device.property("transport_id");

        if (!model.empty())
            line += "model:" + model + " ";
        if (!product.empty())
            line += "product:" + product + " ";
        if (!transportId.empty())
            line += "transport_id:" + transportId;

        return line;
    }

    std::string formatDevicesTable(const std::vector<AdbDeviceInfo>& devices)
    {
        if (devices.empty())
            return "(no devices)\n";

        std::string output = "SERIAL                  STATE         DETAILS\n";
        for (const auto& device : devices)
            output += formatDeviceLine(device) + "\n";

        return output;
    }

    std::string formatCommandResult(const AdbCommandResult& result)
    {
        std::string output = "exit code: " + std::to_string(result.exitCode) + "\n";

        if (!result.stdoutText.empty())
        {
            output += "stdout:\n" + result.stdoutText;
            if (result.stdoutText.back() != '\n')
                output += "\n";
        }

        if (!result.stderrText.empty())
        {
            output += "stderr:\n" + result.stderrText;
            if (result.stderrText.back() != '\n')
                output += "\n";
        }

        return output;
    }

    std::string unknownSerialWarning(const AdbModule& adb, const std::string& serial)
    {
        const auto known = adb.devices();
        const bool knownSerial = std::any_of(known.begin(), known.end(),
            [&](const AdbDeviceInfo& device) { return device.serial == serial; });

        if (knownSerial)
            return {};

        return "warning: '" + serial + "' is not a currently known device serial"
            " (run 'devices' to see available serials).\n";
    }
}
