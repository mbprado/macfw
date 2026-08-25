#include "device_registry.h"
#include "../../../version.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

struct DetectedUnit {
    std::string productName;
    std::uint64_t guid = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t unitSpecifierId = 0;
    std::uint32_t unitSwVersion = 0;
    bool hasGuid = false;
    bool hasVendorId = false;
    bool hasUnitSpecifierId = false;
    bool hasUnitSwVersion = false;
    const macfw::deviceprobe::SupportedDevice* supported = nullptr;
};

std::string stringProperty(io_registry_entry_t service, const char* key) {
    CFStringRef keyString = CFStringCreateWithCString(
        kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!keyString) return {};
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, keyString, kCFAllocatorDefault, 0);
    CFRelease(keyString);
    if (!value) return {};

    std::string result;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        char buffer[1024] = {};
        if (CFStringGetCString(static_cast<CFStringRef>(value), buffer,
                               sizeof(buffer), kCFStringEncodingUTF8))
            result = buffer;
    }
    CFRelease(value);
    return result;
}

std::optional<std::uint64_t> numberProperty(io_registry_entry_t service, const char* key) {
    CFStringRef keyString = CFStringCreateWithCString(
        kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!keyString) return std::nullopt;
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, keyString, kCFAllocatorDefault, 0);
    CFRelease(keyString);
    if (!value) return std::nullopt;

    std::optional<std::uint64_t> result;
    if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        std::uint64_t number = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(value),
                             kCFNumberSInt64Type, &number))
            result = number;
    }
    CFRelease(value);
    return result;
}

const macfw::deviceprobe::SupportedDevice* identify(const DetectedUnit& unit) {
    for (const auto& candidate : macfw::deviceprobe::kSupportedDevices) {
        if (unit.productName != candidate.productName) continue;
        if (unit.hasUnitSpecifierId && unit.unitSpecifierId != candidate.unitSpecifierId) continue;
        if (candidate.requireSwVersion &&
            (!unit.hasUnitSwVersion || unit.unitSwVersion != candidate.unitSwVersion)) continue;
        return &candidate;
    }
    return nullptr;
}

std::vector<DetectedUnit> scan() {
    std::vector<DetectedUnit> units;
    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return units;

    io_iterator_t iterator = IO_OBJECT_NULL;
    const kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
    if (kr != KERN_SUCCESS) return units;

    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        DetectedUnit unit;
        unit.productName = stringProperty(service, "FireWire Product Name");

        if (const auto v = numberProperty(service, "GUID")) {
            unit.guid = *v; unit.hasGuid = true;
        }
        if (const auto v = numberProperty(service, "Vendor_ID")) {
            unit.vendorId = static_cast<std::uint32_t>(*v); unit.hasVendorId = true;
        }
        if (const auto v = numberProperty(service, "Unit_Spec_ID")) {
            unit.unitSpecifierId = static_cast<std::uint32_t>(*v); unit.hasUnitSpecifierId = true;
        }
        if (const auto v = numberProperty(service, "Unit_SW_Version")) {
            unit.unitSwVersion = static_cast<std::uint32_t>(*v); unit.hasUnitSwVersion = true;
        }

        unit.supported = identify(unit);
        units.push_back(unit);
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return units;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

void printHex(std::uint64_t value, unsigned width = 0) {
    std::cout << "0x" << std::hex;
    if (width) std::cout << std::setw(width) << std::setfill('0');
    std::cout << value << std::dec << std::setfill(' ');
}

void printHuman(const std::vector<DetectedUnit>& units) {
    std::cout << "macfw deviceprobe " << macfw::build::kVersion
              << " build " << macfw::build::kGitSha << "\n";

    unsigned supportedCount = 0;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto& unit = units[i];
        std::cout << "\nFireWire unit #" << (i + 1) << "\n"
                  << "    product:      " << (unit.productName.empty() ? "<unknown>" : unit.productName) << '\n';
        if (unit.hasGuid) { std::cout << "    GUID:         "; printHex(unit.guid, 16); std::cout << '\n'; }
        if (unit.hasVendorId) { std::cout << "    vendor ID:    "; printHex(unit.vendorId); std::cout << '\n'; }
        if (unit.hasUnitSpecifierId) { std::cout << "    unit spec ID: "; printHex(unit.unitSpecifierId); std::cout << '\n'; }
        if (unit.hasUnitSwVersion) { std::cout << "    unit SW ver:  "; printHex(unit.unitSwVersion); std::cout << '\n'; }

        if (unit.supported) {
            ++supportedCount;
            std::cout << "    supported:    yes\n"
                      << "    macfw id:     " << unit.supported->macfwId << '\n'
                      << "    family:       " << unit.supported->family << '\n'
                      << "    model:        " << unit.supported->model << '\n'
                      << "    personality:  "
                      << macfw::deviceprobe::personalityName(unit.supported->personality) << '\n';
        } else {
            std::cout << "    supported:    no\n";
        }
    }

    std::cout << "\nsummary: " << supportedCount << " supported macfw device"
              << (supportedCount == 1 ? "" : "s") << " detected\n";
}

void printJson(const std::vector<DetectedUnit>& units) {
    std::cout << "{\"version\":\"" << macfw::build::kVersion
              << "\",\"build\":\"" << macfw::build::kGitSha << "\",\"devices\":[";
    bool first = true;
    unsigned supportedCount = 0;
    for (const auto& unit : units) {
        if (!first) std::cout << ',';
        first = false;
        if (unit.supported) ++supportedCount;
        std::cout << "{\"product\":\"" << jsonEscape(unit.productName) << "\""
                  << ",\"supported\":" << (unit.supported ? "true" : "false");
        if (unit.hasGuid) std::cout << ",\"guid\":\"0x" << std::hex << unit.guid << std::dec << "\"";
        if (unit.hasVendorId) std::cout << ",\"vendor_id\":" << unit.vendorId;
        if (unit.hasUnitSpecifierId) std::cout << ",\"unit_spec_id\":" << unit.unitSpecifierId;
        if (unit.hasUnitSwVersion) std::cout << ",\"unit_sw_version\":" << unit.unitSwVersion;
        if (unit.supported) {
            std::cout << ",\"macfw_id\":\"" << unit.supported->macfwId << "\""
                      << ",\"family\":\"" << unit.supported->family << "\""
                      << ",\"model\":\"" << unit.supported->model << "\""
                      << ",\"personality\":\""
                      << macfw::deviceprobe::personalityName(unit.supported->personality) << "\"";
        }
        std::cout << '}';
    }
    std::cout << "],\"supported_count\":" << supportedCount << "}\n";
}

void usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [--json] [--require-supported]\n"
              << "  --json               machine-readable output for installers/services\n"
              << "  --require-supported  exit 3 when no supported macfw device is connected\n";
}

} // namespace

int main(int argc, char** argv) {
    bool json = false;
    bool requireSupported = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") json = true;
        else if (arg == "--require-supported") requireSupported = true;
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
        else { std::cerr << "unknown argument: " << arg << '\n'; usage(argv[0]); return 2; }
    }

    const auto units = scan();
    unsigned supportedCount = 0;
    for (const auto& unit : units) if (unit.supported) ++supportedCount;

    if (json) printJson(units); else printHuman(units);
    if (requireSupported && supportedCount == 0) return 3;
    return 0;
}
