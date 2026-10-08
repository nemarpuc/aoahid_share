// SPDX-License-Identifier: MIT
#include "store.hpp"

#include "paths.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

namespace aoas {
namespace {

// A profile that names no device has no serial to be
// filed under until a device takes it. Its file's name has a space in it,
// which no serial can have, so it is never also a device's.
std::string file_name(const DeviceConfig& device, const size_t unnamed) {
    if (!device.serial.empty())
        return device.serial + ".ini";
    return "unassigned " + std::to_string(unnamed + 1) + ".ini";
}

} // namespace

std::string devices_dir() { return (std::filesystem::path(settings_dir()) / "device").string(); }

std::string device_file_path(const std::string& serial) {
    return (std::filesystem::path(devices_dir()) / (serial + ".ini")).string();
}

std::string load_store(Config& out) {
    // The folder may have been removed since the program started.
    std::error_code made;
    std::filesystem::create_directories(settings_dir(), made);
    const std::string path = config_file_path();
    std::string text;
    Config config;
    if (read_file(path, text)) {
        const std::string error = parse_globals(text, config);
        if (!error.empty())
            return path + ": " + error;
    } else if (!write_file(path, format_globals(config))) {
        return path + " could not be written";
    }
    std::error_code ignored;
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(devices_dir(), ignored)) {
        if (entry.path().extension() == ".ini")
            files.push_back(entry.path());
    }
    // The folder's own order differs from run to run; the devices' does not.
    std::sort(files.begin(), files.end());
    if (config.devices.size() + files.size() > max_devices)
        return "there are more than " + std::to_string(max_devices) + " devices under " +
               devices_dir();
    for (const std::filesystem::path& file : files) {
        DeviceConfig device;
        if (!read_file(file.string(), text))
            return file.string() + " could not be read";
        const std::string error = parse_device(text, device);
        if (!error.empty())
            return file.string() + ": " + error;
        config.devices.push_back(std::move(device));
    }
    const std::string error = validate_config(config);
    if (!error.empty())
        return error;

    out = std::move(config);
    return {};
}

bool save_store(const Config& config) {
    std::error_code error;
    const std::filesystem::path folder = devices_dir();
    std::filesystem::create_directories(folder, error);
    std::vector<std::string> kept;
    size_t unnamed = 0;
    for (const DeviceConfig& device : config.devices) {
        const std::string name = file_name(device, device.serial.empty() ? unnamed++ : 0);
        if (!write_file((folder / name).string(), format_device(device)))
            return false;
        kept.push_back(name);
    }
    if (!write_file(config_file_path(), format_globals(config)))
        return false;
    // Files of devices that are gone are removed: only what reads as a
    // device's file, never anything else that was put in the folder.
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(folder, error)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".ini" ||
            std::find(kept.begin(), kept.end(), name) != kept.end())
            continue;
        std::string text;
        DeviceConfig device;
        if (read_file(entry.path().string(), text) && parse_device(text, device).empty())
            std::filesystem::remove(entry.path(), error);
    }
    return true;
}

} // namespace aoas
