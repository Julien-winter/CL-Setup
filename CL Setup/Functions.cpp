#include "Functions.h"

namespace Helper {
    bool restartRequired = false;
    std::atomic<bool> vcComplete{false};
    int vcCheckSleepTimes = 0;
    std::mutex consoleMutex;
    std::mutex logMutex;
    std::ofstream logFile;
    bool logEnabled = false;
    std::string g_logPath;
    CLIConfig cliConfig;
    std::vector<CheckResult> g_results;
    std::mutex g_resultsMutex;
    std::vector<std::string> g_pendingChanges;
    std::mutex g_pendingChangesMutex;
    bool applyChanges = false;
    std::string g_repoUrl = "Julien-winter/CL-Setup";
    std::string g_appName = "CL-Setup";
    std::string g_appVersion = "1.2.2";
    int g_exitCode = 0;
}

namespace {
    std::string toLowerCopy(const std::string& value)
    {
        std::string result = value;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    std::string trimCopy(const std::string& value)
    {
        const char* whitespace = " \t\r\n";
        size_t start = value.find_first_not_of(whitespace);
        if (start == std::string::npos) return "";
        size_t end = value.find_last_not_of(whitespace);
        return value.substr(start, end - start + 1);
    }

    // "5,6,7,8" style CSV without matching 3 inside 13/23/30
    bool csvContains(const std::string& csv, int value)
    {
        std::string token;
        for (size_t i = 0; i <= csv.size(); i++) {
            if (i == csv.size() || csv[i] == ',') {
                std::string trimmed = trimCopy(token);
                if (!trimmed.empty() && atoi(trimmed.c_str()) == value) return true;
                token.clear();
            }
            else {
                token += csv[i];
            }
        }
        return false;
    }

    std::string readRegistryStringValue(HKEY root, const char* subkey, const char* valueName)
    {
        HKEY hKey;
        if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return "";
        char buffer[512] = { 0 };
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        std::string result;
        if (RegQueryValueExA(hKey, valueName, NULL, &type, reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            result.assign(buffer);
        }
        RegCloseKey(hKey);
        return result;
    }

    bool startsWithLower(const std::string& lowerValue, const char* lowerPrefix)
    {
        std::string prefix = lowerPrefix;
        return lowerValue.size() >= prefix.size() &&
            lowerValue.compare(0, prefix.size(), prefix) == 0;
    }

    // v1.2.2: a device instance on one of these buses is real hardware and is
    // NEVER a removal candidate (ROOT\, SWD\, VMBUS\, TAP\, BTH\... are not listed)
    bool isHardwareBusInstance(const std::string& instanceId)
    {
        if (instanceId.empty()) return false;
        static const char* hardwarePrefixes[] = {
            "pci\\", "usb\\", "pcmcia\\", "1394\\", "scsi\\", "sas\\", "nvme\\",
            "ide\\", "fibre\\", "mmc\\", "ufs\\", "gpe\\", "acpi\\"
        };
        std::string lower = toLowerCopy(instanceId);
        for (const char* prefix : hardwarePrefixes) {
            if (startsWithLower(lower, prefix)) return true;
        }
        return false;
    }

    // Only loopback / real virtual devices are ever candidates for removal - never a physical NIC.
    // v1.2.2 rework: classify by (a) friendly-name markers, (b) the PnP device-instance bus.
    // Unknown instance -> treated as hardware (conservative: never remove what we cannot prove).
    bool isVirtualAdapter(const std::string& friendlyName, const IP_ADAPTER_ADDRESSES* adapter,
                          const std::string& instanceId, std::string* reason)
    {
        if (adapter == NULL) return false;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) return false;

        std::string name = toLowerCopy(friendlyName);

        // (a) name markers win over the bus check - VMware/VBox sit on PCI\ but are virtual
        static const char* virtualMarkers[] = {
            "vethernet", "hyper-v", "vmware", "virtualbox", "vbox", "bluetooth", "wan miniport",
            "km-test", "microsoft wi-fi direct", "microsoft loopback", "isatap", "teredo",
            "6to4", "vpn", "tap", "tun", "wireguard", "nordlynx", "proton", "windscribe", "mullvad",
            "expressvpn", "openvpn", "softether", "anyconnect", "hamachi", "zerotier", "npcap",
            "wsl", "docker", "network bridge", "layered", "pan ", "virtual",
            // v1.2.2: leftovers that stayed behind in v1.2.1
            "lan-verbindung*", "tailscale", "wintun"
        };
        for (const char* marker : virtualMarkers) {
            if (name.find(marker) != std::string::npos) {
                if (reason != NULL) *reason = std::string("name marker '") + marker + "'";
                return true;
            }
        }

        if (adapter->IfType == IF_TYPE_PPP || adapter->IfType == IF_TYPE_SLIP ||
            adapter->IfType == IF_TYPE_TUNNEL) {
            if (reason != NULL) *reason = "software link (PPP/SLIP/tunnel)";
            return true;
        }

        // (b) bus classification via the PnP device instance
        if (instanceId.empty()) {
            return false; // unknown device -> conservative, keep it
        }
        if (isHardwareBusInstance(instanceId)) {
            return false;
        }
        if (reason != NULL) {
            *reason = "virtual bus device (" + instanceId + ")";
        }
        return true;
    }

    // v1.2.2: mainboard vendor from the firmware registry block (SMBIOS mirror)
    std::string mainboardVendor()
    {
        std::string vendor = readRegistryStringValue(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\BIOS", "BaseBoardManufacturer");
        if (vendor.empty()) {
            vendor = readRegistryStringValue(HKEY_LOCAL_MACHINE,
                "HARDWARE\\DESCRIPTION\\System\\BIOS", "SystemManufacturer");
        }
        std::string v = toLowerCopy(trimCopy(vendor));
        if (v.empty()) return "";
        if (v.find("asus") != std::string::npos) return "ASUS";
        if (v.find("micro-star") != std::string::npos || v == "msi") return "MSI";
        if (v.find("gigabyte") != std::string::npos) return "Gigabyte";
        if (v.find("asrock") != std::string::npos) return "ASRock";
        if (v.find("alienware") != std::string::npos || v.find("dell") != std::string::npos) return "Dell";
        if (v.find("hewlett") != std::string::npos || v == "hp" || v.find("compaq") != std::string::npos) return "HP";
        if (v.find("lenovo") != std::string::npos || v.find("ibm") != std::string::npos) return "Lenovo";
        if (v.find("acer") != std::string::npos) return "Acer";
        if (v.find("medion") != std::string::npos) return "Medion";
        if (v.find("toshiba") != std::string::npos) return "Toshiba";
        if (v.find("supermicro") != std::string::npos) return "Supermicro";
        if (v.find("evga") != std::string::npos) return "EVGA";
        return "";
    }

    // v1.2.2: SHORT BIOS path for the detected board, appended in the SAME line
    // topic: "tpm" | "secureboot" | "cpuv"
    std::string biosHint(const char* topic)
    {
        const std::string t = topic;
        const std::string vendor = mainboardVendor();
        std::string path;

        if (t == "tpm") {
            if (vendor == "ASUS") path = "Advanced > fTPM Config > Off";
            else if (vendor == "MSI") path = "Security > fTPM/PTT > Off";
            else if (vendor == "Gigabyte") path = "Settings > fTPM > Off";
            else if (vendor == "ASRock") path = "Advanced > CPU Config > fTPM";
            else if (vendor == "Dell" || vendor == "HP" || vendor == "Lenovo")
                path = "Security > TPM > Off";
            else path = "BIOS > Security > TPM > Off";
        }
        else if (t == "secureboot") {
            if (vendor == "ASUS" || vendor == "Gigabyte") path = "Boot > Secure Boot > Disabled";
            else if (vendor == "MSI") path = "Security > Secure Boot > Disabled";
            else if (vendor == "ASRock") path = "Security > Secure Boot > Disabled";
            else if (vendor == "Dell" || vendor == "HP" || vendor == "Lenovo")
                path = "Security > Secure Boot > Disabled";
            else path = "BIOS > Secure Boot > Disabled";
        }
        else { // "cpuv"
            if (vendor == "ASUS") path = "Advanced > CPU Config > VT-x";
            else if (vendor == "MSI") path = "OC > CPU Features > VT-x";
            else if (vendor == "Gigabyte") path = "Tweaker > VT-x / SVM Mode";
            else if (vendor == "ASRock") path = "Advanced > CPU Config > VT-x/SVM";
            else if (vendor == "Dell") path = "CPU > VT-x";
            else if (vendor == "HP") path = "Security > VT-x";
            else if (vendor == "Lenovo") path = "Config > CPU > VT-x";
            else path = "BIOS > CPU Config > VT-x";
        }

        return "[" + (vendor.empty() ? std::string("BIOS") : vendor) + ": " + path + "]";
    }

    // Maps adapter GUID (from GetAdaptersAddresses) to the PnP device instance id used by pnputil
    void collectNetAdapterInstances(std::map<std::string, std::string>& out)
    {
        static const GUID kGuidDevClassNet =
            { 0x4d36e972, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

        HDEVINFO deviceInfo = SetupDiGetClassDevsA(&kGuidDevClassNet, NULL, NULL, DIGCF_PRESENT);
        if (deviceInfo == INVALID_HANDLE_VALUE) return;

        SP_DEVINFO_DATA data;
        ZeroMemory(&data, sizeof(data));
        data.cbSize = sizeof(data);

        for (DWORD index = 0; SetupDiEnumDeviceInfo(deviceInfo, index, &data); index++) {
            char instanceBuffer[512] = { 0 };
            DWORD needed = 0;
            std::string instanceId;
            if (SetupDiGetDeviceInstanceIdA(deviceInfo, &data, instanceBuffer, sizeof(instanceBuffer), &needed)) {
                instanceId = instanceBuffer;
            }

            std::string adapterGuid;
            // Scope must be DICS_FLAG_GLOBAL/DICS_FLAG_CONFIGSPECIFIC and KeyType
            // DIREG_DEV/DIREG_DRV - "DIREG_READ" does not exist in the Windows SDK.
            // DIREG_DRV opens the driver (software) key that holds NetCfgInstanceId.
            HKEY driverKey = SetupDiOpenDevRegKey(deviceInfo, &data, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
            if (driverKey != NULL && driverKey != INVALID_HANDLE_VALUE) {
                char guidBuffer[128] = { 0 };
                DWORD size = sizeof(guidBuffer);
                DWORD type = 0;
                if (RegQueryValueExA(driverKey, "NetCfgInstanceId", NULL, &type,
                    reinterpret_cast<LPBYTE>(guidBuffer), &size) == ERROR_SUCCESS &&
                    (type == REG_SZ || type == REG_EXPAND_SZ)) {
                    adapterGuid = guidBuffer;
                }
                RegCloseKey(driverKey);
            }

            if (!adapterGuid.empty() && !instanceId.empty()) {
                out[toLowerCopy(adapterGuid)] = instanceId;
            }
        }
        SetupDiDestroyDeviceInfoList(deviceInfo);
    }

    // The undo script (Undo-CL-Setup.ps1) uses this list to re-detect removed adapters
    void appendRemovedAdapterLog(const std::string& instanceId, const std::string& name)
    {
        char basePath[MAX_PATH] = { 0 };
        if (GetEnvironmentVariableA("LOCALAPPDATA", basePath, MAX_PATH) == 0) return;
        std::string directory = std::string(basePath) + "\\CL-Setup";
        CreateDirectoryA(directory.c_str(), NULL);
        std::ofstream log(directory + "\\removed-adapters.log", std::ios::app);
        if (log.is_open()) {
            log << instanceId << "|" << name << "\n";
        }
    }

    struct SmbiosIdentity {
        bool valid = false;
        int smbiosMajor = 0;
        int smbiosMinor = 0;
        std::string systemManufacturer;
        std::string systemProduct;
        std::string systemVersion;
        std::string systemSerial;
        std::string boardManufacturer;
        std::string boardProduct;
        std::string boardVersion;
        std::string boardSerial;
        std::string uuidText;
        bool uuidNil = false;
    };

    std::string formatSmbiosUuid(const BYTE* bytes)
    {
        char text[64] = { 0 };
        snprintf(text, sizeof(text),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
        return text;
    }

    // Raw SMBIOS (GetSystemFirmwareTable) - the layer perm HWID spoofers patch
    SmbiosIdentity readSmbiosIdentity()
    {
        SmbiosIdentity identity;

        UINT bufferSize = GetSystemFirmwareTable(0x52534D42 /* 'RSMB' */, 0, NULL, 0);
        if (bufferSize == 0 || bufferSize > 8 * 1024 * 1024) return identity;

        std::vector<BYTE> buffer(bufferSize);
        UINT bytesRead = GetSystemFirmwareTable(0x52534D42, 0, buffer.data(), bufferSize);
        if (bytesRead == 0) return identity;

#pragma pack(push, 1)
        struct RawSmbiosData {
            BYTE Used20CallingMethod;
            BYTE SMBIOSMajorVersion;
            BYTE SMBIOSMinorVersion;
            BYTE DmiRevision;
            DWORD Length;
            BYTE SMBIOSTableData[1];
        };
#pragma pack(pop)

        const size_t headerSize = offsetof(RawSmbiosData, SMBIOSTableData);
        if (bytesRead < headerSize) return identity;

        const RawSmbiosData* raw = reinterpret_cast<const RawSmbiosData*>(buffer.data());
        identity.smbiosMajor = raw->SMBIOSMajorVersion;
        identity.smbiosMinor = raw->SMBIOSMinorVersion;
        DWORD tableLength = raw->Length;
        DWORD available = bytesRead - static_cast<DWORD>(headerSize);
        if (tableLength > available) tableLength = available;

        const BYTE* table = raw->SMBIOSTableData;
        DWORD offset = 0;
        while (offset + 4 <= tableLength) {
            const BYTE* header = table + offset;
            BYTE type = header[0];
            BYTE formattedLength = header[1];
            if (formattedLength < 4 || offset + formattedLength > tableLength) break;

            std::vector<std::string> strings;
            DWORD position = offset + formattedLength;
            while (position < tableLength && table[position] != 0) {
                DWORD start = position;
                while (position < tableLength && table[position] != 0) position++;
                strings.emplace_back(reinterpret_cast<const char*>(table + start), position - start);
                position++;
            }
            position++; // string-set terminator

            auto getString = [&](int index) -> std::string {
                if (index < 0 || index >= static_cast<int>(strings.size())) return "";
                return strings[index];
            };

            if (type == 1 && formattedLength >= 0x18) {
                identity.valid = true;
                identity.systemManufacturer = getString(0);
                identity.systemProduct = getString(1);
                identity.systemVersion = getString(2);
                identity.systemSerial = getString(3);

                const BYTE* uuid = header + 8;
                bool allZero = true;
                bool allFF = true;
                for (int i = 0; i < 16; i++) {
                    if (uuid[i] != 0x00) allZero = false;
                    if (uuid[i] != 0xFF) allFF = false;
                }
                identity.uuidNil = allZero || allFF;
                identity.uuidText = formatSmbiosUuid(uuid);
            }
            else if (type == 2 && formattedLength >= 8) {
                identity.boardManufacturer = getString(0);
                identity.boardProduct = getString(1);
                identity.boardVersion = getString(2);
                identity.boardSerial = getString(3);
            }
            else if (type == 127) {
                break;
            }

            offset = position;
        }
        return identity;
    }

    bool isPlaceholderIdentity(const std::string& value)
    {
        std::string lowered = toLowerCopy(trimCopy(value));
        if (lowered.empty()) return true;
        static const char* placeholders[] = {
            "to be filled by o.e.m.", "to be filled by o.e.m", "default string",
            "system product name", "system manufacturer", "system version",
            "not specified", "not applicable", "none", "n/a", "oem", "unknown",
            "base board version", "chassis version", "invalid", "sernum", "xxxx"
        };
        for (const char* placeholder : placeholders) {
            if (lowered == placeholder) return true;
        }
        return false;
    }

    bool isAllHexValue(const std::string& value)
    {
        if (value.size() < 8) return false;
        for (char c : value) {
            bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex) return false;
        }
        return true;
    }

    // "BCF1D5C8102B6C3", "AT5LMDB1M": no spaces, letters + digits, high entropy
    bool isRandomToken(const std::string& value)
    {
        if (value.size() < 8 || value.find(' ') != std::string::npos) return false;
        bool hasAlpha = false;
        bool hasDigit = false;
        bool seen[256] = { false };
        int unique = 0;
        for (unsigned char c : value) {
            if (!seen[c]) { seen[c] = true; unique++; }
            if (c >= '0' && c <= '9') hasDigit = true;
            else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) hasAlpha = true;
        }
        if (!hasAlpha || !hasDigit) return false;
        return static_cast<double>(unique) / static_cast<double>(value.size()) >= 0.6;
    }

    // "6429-BM-11135", "10116-BV-11344": digits + dashes + short code
    bool isSyntheticPattern(const std::string& value)
    {
        if (value.size() < 10) return false;
        int dashes = 0;
        int digitRun = 0;
        bool longDigitRun = false;
        for (char c : value) {
            if (c == '-') { dashes++; digitRun = 0; }
            else if (c >= '0' && c <= '9') { digitRun++; if (digitRun >= 4) longDigitRun = true; }
            else if ((c >= 'A' && c <= 'Z') || c == ' ') { digitRun = 0; }
            else { return false; }
        }
        return dashes >= 2 && longDigitRun;
    }

    // v1.2.1: 0 = clean, 1 = weak anomaly, 2 = strong (hex/synthetic) pattern;
    // strong is counted separately because two independent strong hits confirm spoofing
    int identityScore(const std::string& value, bool* wasStrong)
    {
        if (wasStrong) *wasStrong = false;
        std::string cleaned = trimCopy(value);
        if (cleaned.empty()) return 0;
        if (isAllHexValue(cleaned) || isSyntheticPattern(cleaned)) {
            if (wasStrong) *wasStrong = true;
            return 2;
        }
        if (isRandomToken(cleaned)) return 1;
        if (isPlaceholderIdentity(cleaned)) return 1;
        return 0;
    }

    bool identityDiffers(const std::string& smbios, const std::string& registryValue)
    {
        if (smbios.empty() || registryValue.empty()) return false;
        return toLowerCopy(trimCopy(smbios)) != toLowerCopy(trimCopy(registryValue));
    }

    // v1.2.1: "v1.2.3" / "1.2.3-beta" -> {1,2,3}; false if not version-like
    bool parseVersionParts(const std::string& input, std::vector<int>& out)
    {
        std::string s = trimCopy(input);
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
        size_t cut = s.find_first_of("-+");
        if (cut != std::string::npos) s = s.substr(0, cut);
        if (s.empty()) return false;

        out.clear();
        std::stringstream ss(s);
        std::string part;
        while (std::getline(ss, part, '.')) {
            if (part.empty()) return false;
            for (char c : part) {
                if (c < '0' || c > '9') return false;
            }
            out.push_back(atoi(part.c_str()));
        }
        return out.size() >= 2;
    }

    bool looksLikeVersion(const std::string& input)
    {
        std::vector<int> parts;
        return parseVersionParts(input, parts);
    }

    // true only when the remote tag is actually NEWER than the local version
    bool isNewerVersion(const std::string& remote, const std::string& local)
    {
        std::vector<int> r;
        std::vector<int> l;
        if (!parseVersionParts(remote, r) || !parseVersionParts(local, l)) return false;
        size_t n = (std::max)(r.size(), l.size());
        r.resize(n, 0);
        l.resize(n, 0);
        return r > l;
    }
}

void Helper::recordResult(const std::string& check, const std::string& status, const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    // v1.2.2: upsert instead of append. A check re-run in the apply phase replaces its
    // initial entry (fresh status), so the final summary counts every check exactly once
    // instead of only the partial apply re-run ("17 passed" bug).
    for (auto& result : g_results) {
        if (result.check == check) {
            result.status = status;
            result.message = message;
            logWrite("  [" + status + "] " + check + " - " + message);
            return;
        }
    }
    g_results.push_back({check, status, message});
    logWrite("  [" + status + "] " + check + " - " + message);
}

void Helper::clearResults()
{
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    g_results.clear();
}

bool Helper::requestChange(const std::string& change)
{
    if (applyChanges) return true;

    {
        std::lock_guard<std::mutex> lock(g_pendingChangesMutex);
        if (std::find(g_pendingChanges.begin(), g_pendingChanges.end(), change) == g_pendingChanges.end()) {
            g_pendingChanges.push_back(change);
        }
    }

    printConcern("- Change needed: " + change);
    return false;
}

void Helper::clearPendingChanges()
{
    std::lock_guard<std::mutex> lock(g_pendingChangesMutex);
    g_pendingChanges.clear();
}

bool Helper::hasPendingChanges()
{
    std::lock_guard<std::mutex> lock(g_pendingChangesMutex);
    return !g_pendingChanges.empty();
}

void Helper::printPendingChanges()
{
    std::lock_guard<std::mutex> lock(g_pendingChangesMutex);
    if (g_pendingChanges.empty()) return;

    Color::setForegroundColor(Color::Cyan);
    std::cout << "\nPending changes:\n";
    Color::setForegroundColor(Color::LightGray);
    for (const auto& change : g_pendingChanges) {
        std::cout << " - " << change << "\n";
    }
}

// v1.2.2: %TEMP%\CL-Setup\settings.ini - edit showShopInfo=true -> false to
// silence the promo box silently (no console output either way)
namespace {
    std::string shopSettingsPath()
    {
        char tempPath[MAX_PATH] = { 0 };
        if (GetTempPathA(MAX_PATH, tempPath) == 0 || tempPath[0] == '\0') return "";
        std::string dir = std::string(tempPath) + "CL-Setup";
        CreateDirectoryA(dir.c_str(), NULL);
        return dir + "\\settings.ini";
    }

    bool shopInfoEnabled()
    {
        std::string path = shopSettingsPath();
        if (path.empty()) return true;

        std::ifstream in(path);
        if (!in) {
            // first run: create the file with the default so it can be found and edited
            std::ofstream out(path);
            if (out) out << "showShopInfo=true\n";
            return true;
        }

        std::string line;
        while (std::getline(in, line)) {
            line = trimCopy(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = toLowerCopy(trimCopy(line.substr(0, eq)));
            std::string value = toLowerCopy(trimCopy(line.substr(eq + 1)));
            if (key == "showshopinfo") {
                return !(value == "false" || value == "0" || value == "no" || value == "off");
            }
        }
        return true;
    }
}

// v1.2.1: shop info box shown right after the apply-changes yes/no dialog
void Helper::showShopInfo()
{
    if (cliConfig.headless) return;
    // v1.2.2: opt-out via %TEMP%\CL-Setup\settings.ini (showShopInfo=false)
    if (!shopInfoEnabled()) return;
    MessageBoxA(NULL,
        "You can buy alt accounts for cheap from shop.uchiha.us\n"
        "You can buy CL from Main shop at: cheatloverz.store",
        "CL Setup",
        MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

bool Helper::promptApplyChanges()
{
    if (!hasPendingChanges()) return false;
    printPendingChanges();

    if (cliConfig.headless) {
        printConcern("- Headless mode: pending changes were not applied");
        return false;
    }

    std::string msg = "The checker found changes that may be needed:\n\n";
    {
        std::lock_guard<std::mutex> lock(g_pendingChangesMutex);
        for (const auto& change : g_pendingChanges) {
            msg += "  \x95 " + change + "\n";
        }
    }
    msg += "\nApply these changes now?";

    int choice = MessageBoxA(NULL, msg.c_str(),
        "Apply setup changes?",
        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);

    // v1.2.1: promo info box appears right after the yes/no answer
    showShopInfo();

    if (choice == IDYES) return true;
    if (choice == IDNO) return false;

    std::string answer;
    std::cout << "\nApply pending changes? Type yes or no: ";
    std::getline(std::cin, answer);
    std::transform(answer.begin(), answer.end(), answer.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return answer == "yes" || answer == "y";
}

std::string Helper::escapeJSON(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    return out;
}

std::string Helper::getTimestampISO()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    gmtime_s(&tm, &time);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void Helper::exportResultsJSON()
{
    if (cliConfig.exportPath.empty()) return;

    std::ofstream f(cliConfig.exportPath);
    if (!f.is_open()) {
        printError("- Failed to export results to " + cliConfig.exportPath);
        return;
    }

    int passed = 0, failed = 0, warnings = 0, skipped = 0;
    for (auto& r : g_results) {
        if (r.status == "OK") passed++;
        else if (r.status == "FAIL") failed++;
        else if (r.status == "WARN") warnings++;
        else if (r.status == "SKIPPED") skipped++;
    }

    f << "{\n";
    f << "  \"timestamp\": \"" << escapeJSON(getTimestampISO()) << "\",\n";
    f << "  \"program\": \"" << escapeJSON(g_appName) << "\",\n";
    f << "  \"version\": \"" << escapeJSON(g_appVersion) << "\",\n";
    f << "  \"results\": [\n";
    for (size_t i = 0; i < g_results.size(); i++) {
        f << "    {\"check\": \"" << escapeJSON(g_results[i].check) << "\", "
          << "\"status\": \"" << escapeJSON(g_results[i].status) << "\", "
          << "\"message\": \"" << escapeJSON(g_results[i].message) << "\"}";
        if (i < g_results.size() - 1) f << ",";
        f << "\n";
    }
    f << "  ],\n";
    f << "  \"summary\": {\n";
    f << "    \"passed\": " << passed << ",\n";
    f << "    \"failed\": " << failed << ",\n";
    f << "    \"warnings\": " << warnings << ",\n";
    f << "    \"skipped\": " << skipped << ",\n";
    f << "    \"restart_required\": " << (restartRequired ? "true" : "false") << "\n";
    f << "  }\n";
    f << "}\n";
    f.close();

    printSuccess("- Results exported to " + cliConfig.exportPath, false);
}

std::string Helper::fetchURL(const std::wstring& url)
{
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"CL-Setup/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    URL_COMPONENTS urlComp;
    ZeroMemory(&urlComp, sizeof(urlComp));
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {0}, path[1024] = {0};
    urlComp.lpszHostName = host;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = path;
    urlComp.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &urlComp)) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    WinHttpReceiveResponse(hRequest, NULL);

    DWORD bytesRead = 0;
    char buffer[4096];
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead)) {
        if (bytesRead == 0) break;
        buffer[bytesRead] = '\0';
        result += buffer;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

void Helper::showHelp()
{
    std::cout << g_appName << " - System Pre-Flight Check Tool v" << g_appVersion << "\n\n";
    std::cout << "Usage: " << g_appName << ".exe [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --help              Show this help message\n";
    std::cout << "  --headless          Run without user interaction\n";
    std::cout << "  --quiet             Only show errors and warnings\n";
    std::cout << "  --apply             Apply needed changes without prompting\n";
    std::cout << "  --export FILE       Export results as JSON to FILE\n";
    std::cout << "  --skip N[,M,...]    Skip specific checks by number (1-28)\n";
    std::cout << "  --only N[,M,...]    Run only specific checks by number (1-28)\n\n";
    std::cout << "Check Numbers:\n";
    std::cout << "  1=WindowsDefender 2=3rdPartyAV 3=SecureBoot 4=RiotVanguard\n";
    std::cout << "  5=VCRedist 6=Chrome 7=ChromeProtection 8=TimeSync 9=Winver\n";
    std::cout << "  10=Symbols 11=FastBoot 12=ExploitProtection 13=SmartScreen\n";
    std::cout << "  14=GameBar 15=TPM 16=CoreIsolation 17=DMAProtection 18=ModifiedOS\n";
    std::cout << "  19=Internet 20=NetworkAdapters 21=VM-Detect 22=Auto-Update\n";
    std::cout << "  23=SecurityMitigations 24=UAC 25=WindowsUpdate\n";
    std::cout << "  26=CPU-V 27=HWID-Spoofer 28=Activation\n";
}

CLIConfig Helper::parseCLI(int argc, char* argv[])
{
    CLIConfig config;
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "--help") {
            config.showHelp = true;
            return config;
        }
        else if (arg == "--headless") {
            config.headless = true;
        }
        else if (arg == "--quiet") {
            config.quiet = true;
        }
        else if (arg == "--apply") {
            config.autoApply = true;
        }
        else if (arg == "--skip" && i + 1 < argc) {
            std::string val(argv[++i]);
            std::stringstream ss(val);
            std::string token;
            while (std::getline(ss, token, ',')) {
                try { config.skipChecks.push_back(std::stoi(token)); }
                catch (...) {}
            }
        }
        else if (arg == "--only" && i + 1 < argc) {
            std::string val(argv[++i]);
            std::stringstream ss(val);
            std::string token;
            while (std::getline(ss, token, ',')) {
                try { config.onlyChecks.push_back(std::stoi(token)); }
                catch (...) {}
            }
        }
        else if (arg == "--export" && i + 1 < argc) {
            config.exportPath = argv[++i];
        }
    }

    return config;
}

bool Helper::isCheckSkipped(int checkId)
{
    if (!cliConfig.onlyChecks.empty()) {
        return std::find(cliConfig.onlyChecks.begin(), cliConfig.onlyChecks.end(), checkId) == cliConfig.onlyChecks.end();
    }
    if (!cliConfig.skipChecks.empty()) {
        return std::find(cliConfig.skipChecks.begin(), cliConfig.skipChecks.end(), checkId) != cliConfig.skipChecks.end();
    }
    return false;
}

bool Helper::isAdmin()
{
    BOOL isElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, size, &size)) {
            isElevated = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return isElevated != FALSE;
}

void Helper::initLogging()
{
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring dir = std::wstring(tempPath) + L"CL-Setup";
    CreateDirectoryW(dir.c_str(), NULL);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    char filename[64];
    snprintf(filename, sizeof(filename), "setup-%04d-%02d-%02d-%02d%02d.log",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);

    std::wstring logPath = dir + L"\\" + std::wstring(filename, filename + strlen(filename));
    int len = WideCharToMultiByte(CP_UTF8, 0, logPath.c_str(), -1, NULL, 0, NULL, NULL);
    g_logPath.resize(len - 1);
    WideCharToMultiByte(CP_UTF8, 0, logPath.c_str(), -1, &g_logPath[0], len, NULL, NULL);

    logFile.open(logPath, std::ios::out | std::ios::app);
    if (logFile.is_open()) {
        logEnabled = true;
        logWrite("=== " + g_appName + " v" + g_appVersion + " started at " + getTimestampISO() + " ===");
    }
}

void Helper::closeLogging()
{
    if (logEnabled && logFile.is_open()) {
        logWrite("=== " + g_appName + " finished ===");
        logFile.close();
        logEnabled = false;
    }
}

void Helper::logWrite(const std::string& message)
{
    if (!logEnabled || !logFile.is_open()) return;
    std::lock_guard<std::mutex> lock(logMutex);
    logFile << message << std::endl;
    logFile.flush();
}

std::string Helper::runSystemCommandWithOutput(const char* command, DWORD timeoutMs)
{
    std::string commandText = command ? command : "";
    auto future = std::async(std::launch::async, [commandText]() -> std::string {
        std::FILE* pipe = _popen(commandText.c_str(), "r");
        if (!pipe) return "";
        char buffer[256];
        std::string result;
        while (std::fgets(buffer, sizeof(buffer), pipe) != NULL) {
            result += buffer;
        }
        _pclose(pipe);
        return result;
    });

    if (future.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::timeout) {
        return "TIMEOUT";
    }
    return future.get();
}

void Helper::backupRegistryKey(HKEY hKeyParent, const std::string& subkey)
{
    // Export every key only once per run: the first call always happens before the
    // key is modified, later calls would already capture changed data.
    // Undo-CL-Setup.ps1 imports the recorded backup file (or, as a fallback, the
    // OLDEST matching .reg file in C:\Windows\Temp - the one closest to the
    // pre-change state) to revert the change.
    static std::mutex backupMutex;
    static std::vector<std::string> backedUpKeys;
    {
        std::lock_guard<std::mutex> lock(backupMutex);
        if (std::find(backedUpKeys.begin(), backedUpKeys.end(), subkey) != backedUpKeys.end()) return;
    }

    HKEY hKey;
    if (RegOpenKeyEx(hKeyParent, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) return;

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring dir = std::wstring(tempPath) + L"CL-Setup\\Backup";
    CreateDirectoryW(dir.c_str(), NULL);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%04d%02d%02d-%02d%02d%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::string keyName = subkey;
    std::replace(keyName.begin(), keyName.end(), '\\', '_');
    std::string exportPath = "C:\\Windows\\Temp\\" + keyName + "_" + timestamp + ".reg";
    // reg export needs the full hive path (HKLM\...), RegOpenKeyEx above does not
    std::string hivePrefix = (hKeyParent == HKEY_LOCAL_MACHINE) ? "HKLM\\" :
        (hKeyParent == HKEY_CURRENT_USER) ? "HKCU\\" :
        (hKeyParent == HKEY_CLASSES_ROOT) ? "HKCR\\" :
        (hKeyParent == HKEY_USERS) ? "HKU\\" : "";
    std::string cmd = "reg export \"" + hivePrefix + subkey + "\" \"" + exportPath + "\" /y 2>nul";
    runSystemCommand(cmd.c_str());
    RegCloseKey(hKey);

    // Undo-CL-Setup.ps1 restores the backup that was taken before the first
    // reg: marker for this key (last backup: entry preceding it in the log)
    if (std::filesystem::exists(exportPath)) {
        logAppliedChange("backup:" + subkey + "|" + exportPath);
    }

    std::lock_guard<std::mutex> lock(backupMutex);
    backedUpKeys.push_back(subkey);
}

void Helper::logAppliedChange(const std::string& change)
{
    // Undo-CL-Setup.ps1 reverts only the entries recorded here, so every
    // non-reversible-looking action (bcdedit, DISM, services, adapters) logs itself
    char basePath[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableA("LOCALAPPDATA", basePath, MAX_PATH) == 0) return;
    std::string directory = std::string(basePath) + "\\CL-Setup";
    CreateDirectoryA(directory.c_str(), NULL);
    std::ofstream log(directory + "\\applied-changes.log", std::ios::app);
    if (log.is_open()) {
        log << change << "\n";
    }
}

ServiceStatus Helper::getServiceStatus(LPCSTR serviceName)
{
    SC_HANDLE scmHandle = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scmHandle == NULL) return STATUS_SERVICE_STOPPED;
    SC_HANDLE serviceHandle = OpenService(scmHandle, serviceName, SERVICE_QUERY_STATUS);
    if (serviceHandle == NULL) { CloseServiceHandle(scmHandle); return STATUS_SERVICE_STOPPED; }
    SERVICE_STATUS_PROCESS serviceStatus;
    DWORD bytesNeeded;
    if (!QueryServiceStatusEx(serviceHandle, SC_STATUS_PROCESS_INFO, (LPBYTE)&serviceStatus, sizeof(serviceStatus), &bytesNeeded)) {
        CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle); return STATUS_SERVICE_STOPPED;
    }
    CloseServiceHandle(serviceHandle);
    CloseServiceHandle(scmHandle);
    return (ServiceStatus)serviceStatus.dwCurrentState;
}

void Helper::printInfo(const std::string& message)
{
    if (cliConfig.quiet) return;
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::LightGray);
    std::cout << "  " << message << std::endl;
}

void Checks::checkInternet()
{
    SetConsoleTitleA("Checking Internet Connection");

    auto testPing = []() -> bool {
        std::string out = Helper::runSystemCommandWithOutput("ping -n 1 -w 3000 8.8.8.8", 5000);
        return (out.find("TTL=") != std::string::npos || out.find("ttl=") != std::string::npos);
    };

    if (testPing()) {
        Helper::recordResult("Internet", "OK", "Connected");
        return;
    }

    if (!Helper::requestChange("Repair internet connection with DNS flush, Winsock reset, and IP renew")) {
        Helper::recordResult("Internet", "WARN", "No connectivity - repair pending approval");
        return;
    }

    Helper::printConcern("- No internet detected, attempting fixes...");
    Helper::runSystemCommand("ipconfig /flushdns");
    Helper::runSystemCommand("netsh winsock reset");
    Helper::logAppliedChange("info:network was repaired (DNS flush + Winsock reset) - a reboot may be required; the Winsock catalog was reset to Windows defaults");
    Sleep(1000);

    if (testPing()) {
        Helper::printSuccess("- Internet restored after DNS/Winsock reset", true);
        Helper::recordResult("Internet", "OK", "Fixed via DNS/Winsock reset");
        return;
    }

    Helper::runSystemCommand("ipconfig /release");
    Sleep(500);
    Helper::runSystemCommand("ipconfig /renew");
    Sleep(2000);

    if (testPing()) {
        Helper::printSuccess("- Internet restored after IP renew", true);
        Helper::recordResult("Internet", "OK", "Fixed via IP renew");
        return;
    }

    Helper::printError("- No internet connection - could not fix automatically");
    Helper::recordResult("Internet", "FAIL", "No connectivity - fix attempts failed");
}

void Checks::checkNetworkAdapters()
{
    SetConsoleTitleA("Checking Network Adapters");

    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_PREFIX;
    ULONG family = AF_UNSPEC;
    ULONG bufferSize = 15 * 1024;
    std::vector<BYTE> buffer(bufferSize);
    PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG result = GetAdaptersAddresses(family, flags, NULL, adapters, &bufferSize);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferSize);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(family, flags, NULL, adapters, &bufferSize);
    }

    if (result != NO_ERROR) {
        Helper::printError("- Failed to list network adapters (GLE=" + std::to_string(result) + ")");
        Helper::recordResult("Network Adapters", "FAIL", "GetAdaptersAddresses failed: " + std::to_string(result));
        return;
    }

    auto wideToUtf8 = [](const wchar_t* value) -> std::string {
        if (value == NULL || value[0] == L'\0') return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
        if (size <= 1) return "";
        std::string text(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), size, NULL, NULL);
        return text;
    };

    auto statusToString = [](IF_OPER_STATUS status) -> std::string {
        switch (status) {
        case IfOperStatusUp: return "Up";
        case IfOperStatusDown: return "Down";
        case IfOperStatusTesting: return "Testing";
        case IfOperStatusUnknown: return "Unknown";
        case IfOperStatusDormant: return "Dormant";
        case IfOperStatusNotPresent: return "Not present";
        case IfOperStatusLowerLayerDown: return "Lower layer down";
        default: return "Other";
        }
    };

    int total = 0;
    int used = 0;
    std::string summary;
    // v1.2.2: classify with the device instance already in hand (collected BEFORE the
    // approval prompt) so each candidate carries proof of being virtual
    std::map<std::string, std::string> deviceInstances;
    collectNetAdapterInstances(deviceInstances);

    struct UnusedCandidate {
        std::string name;
        std::string instanceId;
        std::string reason;
    };
    std::vector<UnusedCandidate> unusedVirtual;

    for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != NULL; adapter = adapter->Next) {
        total++;

        std::string name = wideToUtf8(adapter->FriendlyName);
        if (name.empty() && adapter->AdapterName != NULL) {
            name = adapter->AdapterName;
        }
        if (name.empty()) {
            name = "Unknown adapter";
        }

        std::string instanceId;
        if (adapter->AdapterName != NULL) {
            auto device = deviceInstances.find(toLowerCopy(adapter->AdapterName));
            if (device != deviceInstances.end()) {
                instanceId = device->second;
            }
        }

        bool hasAddress = adapter->FirstUnicastAddress != NULL;
        bool hasGateway = adapter->FirstGatewayAddress != NULL;
        bool isLoopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        bool isUsed = adapter->OperStatus == IfOperStatusUp && !isLoopback && (hasAddress || hasGateway);
        std::string reason;
        bool virtualCandidate = !isLoopback && isVirtualAdapter(name, adapter, instanceId, &reason);
        // v1.2.2: only propose removal when we actually have a device instance -
        // pnputil cannot remove anything without one (seen with "eth0" in a VM)
        if (isUsed) used++;
        else if (virtualCandidate && !instanceId.empty()) {
            unusedVirtual.push_back({ name, instanceId, reason });
        }

        std::string line = name + " - " + (isUsed ? "USED" : "NOT USED") +
            " (" + statusToString(adapter->OperStatus) + ")";
        if (isLoopback) line += " [loopback]";
        else if (virtualCandidate) line += " [virtual]";
        else if (!isUsed) line += instanceId.empty() ? " [unknown]" : " [hardware]";

        if (isUsed) {
            Helper::printSuccess("- Adapter: " + line, false);
        }
        else {
            Helper::printInfo("Adapter: " + line);
        }

        if (!summary.empty()) summary += "; ";
        summary += line;
    }

    if (total == 0) {
        Helper::printConcern("- No network adapters found");
        Helper::recordResult("Network Adapters", "WARN", "No adapters found");
        return;
    }

    std::string usage = std::to_string(used) + "/" + std::to_string(total) + " used: " + summary;

    // v1.2.1: remove unused virtual adapters (Hyper-V/VMware/VPN/Bluetooth leftovers...)
    if (unusedVirtual.empty()) {
        Helper::recordResult("Network Adapters", "OK", usage);
        return;
    }

    auto describe = [](const UnusedCandidate& entry) {
        std::string text = entry.name;
        if (!entry.instanceId.empty()) text += " (" + entry.instanceId + ")";
        if (!entry.reason.empty()) text += " - " + entry.reason;
        return text;
    };

    std::string names;
    for (const auto& entry : unusedVirtual) {
        if (!names.empty()) names += ", ";
        names += describe(entry);
    }

    if (!Helper::requestChange("Uninstall unused virtual network adapter(s): " + names)) {
        Helper::recordResult("Network Adapters", "WARN",
            usage + "; not removed yet: " + names);
        return;
    }

    int removed = 0;
    for (const auto& entry : unusedVirtual) {
        if (entry.instanceId.empty()) {
            Helper::printInfo("No device instance found for adapter: " + entry.name);
            continue;
        }

        std::string command = "pnputil /remove-device \"" + entry.instanceId + "\"";
        int exitCode = Helper::runSystemCommandExitCode(command.c_str());
        if (exitCode == 0) {
            Helper::printSuccess("- Removed unused virtual adapter: " + entry.name, true);
            appendRemovedAdapterLog(entry.instanceId, entry.name);
            removed++;
        }
        else {
            Helper::printError("- Could not remove adapter " + entry.name +
                " (pnputil exit " + std::to_string(exitCode) + ")");
        }
    }

    std::string namesShort;
    for (const auto& entry : unusedVirtual) {
        if (!namesShort.empty()) namesShort += ", ";
        namesShort += entry.name;
    }

    Helper::recordResult("Network Adapters", removed > 0 ? "OK" : "WARN",
        usage + "; removed " + std::to_string(removed) + "/" +
        std::to_string(unusedVirtual.size()) + " unused virtual adapter(s): " + namesShort);
}

void Checks::checkSecurityMitigations()
{
    SetConsoleTitleA("Checking Security Mitigations");

    bool changed = false;
    bool failed = false;
    bool warning = false;
    std::string summary;

    auto appendSummary = [&](const std::string& item) {
        if (!summary.empty()) summary += "; ";
        summary += item;
    };

    auto readDword = [](const char* subkey, const char* valueName, DWORD& value) -> bool {
        return Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, subkey, valueName, &value);
    };

    auto setDword = [&](const char* label, const char* subkey, const char* valueName, DWORD desiredValue) -> bool {
        DWORD currentValue = 0;
        bool hasValue = readDword(subkey, valueName, currentValue);
        if (hasValue && currentValue == desiredValue) {
            Helper::printSuccess("- " + std::string(label) + " already set to " + std::to_string(desiredValue), false);
            appendSummary(std::string(label) + "=" + std::to_string(desiredValue));
            return true;
        }

        if (!Helper::requestChange("Set " + std::string(label) + " to " + std::to_string(desiredValue))) {
            appendSummary(std::string(label) + "=" + (hasValue ? std::to_string(currentValue) : "missing") +
                "->" + std::to_string(desiredValue) + " pending");
            warning = true;
            return false;
        }

        HKEY hKey = NULL;
        LONG createResult = RegCreateKeyEx(HKEY_LOCAL_MACHINE, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_WRITE, NULL, &hKey, NULL);
        if (createResult != ERROR_SUCCESS) {
            Helper::printError("- Failed to open " + std::string(label) + " registry key");
            appendSummary(std::string(label) + "=write failed");
            failed = true;
            return false;
        }

        LONG writeResult = RegSetValueEx(hKey, valueName, NULL, REG_DWORD,
            reinterpret_cast<const BYTE*>(&desiredValue), sizeof(desiredValue));
        RegCloseKey(hKey);

        if (writeResult != ERROR_SUCCESS) {
            Helper::printError("- Failed to set " + std::string(label));
            appendSummary(std::string(label) + "=write failed");
            failed = true;
            return false;
        }

        std::string previous = hasValue ? std::to_string(currentValue) : "missing";
        Helper::printSuccess("- Set " + std::string(label) + " from " + previous + " to " + std::to_string(desiredValue), true);
        appendSummary(std::string(label) + "=" + previous + "->" + std::to_string(desiredValue));
        Helper::logAppliedChange("reg:" + std::string(subkey));
        changed = true;
        return true;
    };

    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management");

    setDword("EnableVirtualizationBasedSecurity",
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "EnableVirtualizationBasedSecurity", 0);
    setDword("RequirePlatformSecurityFeatures",
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "RequirePlatformSecurityFeatures", 0);
    setDword("DeviceGuard Locked",
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "Locked", 1);
    setDword("HVCI Enabled",
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", "Enabled", 0);
    setDword("HVCI Locked",
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", "Locked", 1);
    setDword("KVA FeatureSettingsOverride",
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", "FeatureSettingsOverride", 3);
    setDword("KVA FeatureSettingsOverrideMask",
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", "FeatureSettingsOverrideMask", 3);

    auto lower = [](std::string value) -> std::string {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };

    if (Helper::applyChanges) {
        HINSTANCE hSrDll = LoadLibraryA("srclient.dll");
        if (hSrDll) {
            typedef BOOL(WINAPI* SRSetRestorePointFn)(PRESTOREPOINTINFOW, PSTATEMGRSTATUS);
            SRSetRestorePointFn SRSetRestorePoint = (SRSetRestorePointFn)GetProcAddress(hSrDll, "SRSetRestorePointW");
            if (SRSetRestorePoint) {
                RESTOREPOINTINFOW rp;
                rp.dwEventType = BEGIN_SYSTEM_CHANGE;
                rp.dwRestorePtType = MODIFY_SETTINGS;
                rp.llSequenceNumber = 0;
                std::wstring desc = L"CL-Setup v" + std::wstring(Helper::g_appVersion.begin(), Helper::g_appVersion.end()) + L" - Security mitigation changes";
                wcsncpy_s(rp.szDescription, desc.c_str(), _TRUNCATE);
                STATEMGRSTATUS sms;
                if (SRSetRestorePoint(&rp, &sms)) {
                    rp.dwEventType = END_SYSTEM_CHANGE;
                    SRSetRestorePoint(&rp, &sms);
                    Helper::printSuccess("- System Restore Point created", true);
                }
            }
            FreeLibrary(hSrDll);
        }
    }

    std::string bcdOutput = lower(Helper::runSystemCommandWithOutput("bcdedit /enum {current}", 5000));
    if (bcdOutput == "timeout" || bcdOutput.empty()) {
        Helper::printConcern("- Could not read hypervisor launch status");
        appendSummary("hypervisorlaunchtype=unknown");
        warning = true;
    }
    else if (bcdOutput.find("hypervisorlaunchtype") != std::string::npos &&
        bcdOutput.find("off") != std::string::npos) {
        Helper::printSuccess("- Hypervisor launch is already off", false);
        appendSummary("hypervisorlaunchtype=off");
    }
    else {
        if (!Helper::requestChange("Set hypervisorlaunchtype to off")) {
            appendSummary("hypervisorlaunchtype=change pending");
            warning = true;
            // v1.2.2: NO early return here - keep going so the Hyper-V feature
            // pendings below are registered too, and so a result is always recorded
        }
        else {
            Helper::runSystemCommand("bcdedit /set hypervisorlaunchtype off");
            Helper::logAppliedChange("bcdedit:hypervisorlaunchtype=off");
            Helper::printSuccess("- Set hypervisor launch type to off", true);
            appendSummary("hypervisorlaunchtype=changed to off");
            changed = true;
        }
    }

    auto checkAndDisableFeature = [&](const std::string& featureName) {
        std::string queryCommand = "dism /online /get-featureinfo /featurename:" + featureName + " 2>&1";
        std::string output = lower(Helper::runSystemCommandWithOutput(queryCommand.c_str(), 15000));
        if (output == "timeout" || output.empty()) {
            Helper::printConcern("- Could not read Windows feature status: " + featureName);
            appendSummary(featureName + "=unknown");
            warning = true;
            return;
        }

        if (output.find("error") != std::string::npos ||
            output.find("fehler") != std::string::npos ||
            output.find("not found") != std::string::npos ||
            output.find("nicht") != std::string::npos ||
            output.find("0x800f") != std::string::npos) {
            Helper::printSuccess("- Windows feature is not available: " + featureName, false);
            appendSummary(featureName + "=disabled (not available)");
            return;
        }

        if (output.find("state : disabled") != std::string::npos ||
            output.find("state: disabled") != std::string::npos ||
            output.find("deaktiviert") != std::string::npos) {
            Helper::printSuccess("- Windows feature is already disabled: " + featureName, false);
            appendSummary(featureName + "=disabled");
            return;
        }

        if (output.find("state : enabled") == std::string::npos &&
            output.find("state: enabled") == std::string::npos &&
            output.find("aktiviert") == std::string::npos) {
            Helper::printInfo("Windows feature status is unclear (treated as disabled): " + featureName);
            appendSummary(featureName + "=likely disabled");
            return;
        }

        if (!Helper::requestChange("Disable Windows feature " + featureName)) {
            appendSummary(featureName + "=disable pending");
            warning = true;
            return;
        }

        std::string disableCommand = "dism /online /disable-feature /featurename:" + featureName + " /norestart";
        Helper::runSystemCommand(disableCommand.c_str());
        Helper::printSuccess("- Disabled Windows feature: " + featureName, true);
        Helper::logAppliedChange("dism:" + featureName);
        appendSummary(featureName + "=changed to disabled");
        changed = true;
    };

    checkAndDisableFeature("Microsoft-Hyper-V-Hypervisor");
    checkAndDisableFeature("Microsoft-Hyper-V-All");

    if (changed) {
        Helper::restartRequired = true;
        Helper::recordResult("Security Mitigations", (failed || warning) ? "WARN" : "OK", "Changed: " + summary);
    }
    else if (failed) {
        Helper::recordResult("Security Mitigations", "FAIL", summary);
    }
    else if (warning) {
        Helper::recordResult("Security Mitigations", "WARN", summary);
    }
    else {
        Helper::recordResult("Security Mitigations", "OK", "Already configured: " + summary);
    }
}

void Checks::checkVM()
{
    SetConsoleTitleA("Checking for Virtual Machine");

    bool isVM = false;
    std::string vmType;

    HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\VBoxGuest", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        isVM = true; vmType = "VirtualBox"; RegCloseKey(hKey);
    }
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\VMware, Inc.\\VMware Tools", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        isVM = true; vmType = vmType.empty() ? "VMware" : vmType + "/VMware"; RegCloseKey(hKey);
    }
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\ACPI\\DSDT\\VBOX__", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        isVM = true; vmType = vmType.empty() ? "VirtualBox (ACPI)" : vmType + "/VBox-ACPI"; RegCloseKey(hKey);
    }

    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm) {
        if (OpenService(scm, "VBoxService", SERVICE_QUERY_STATUS)) {
            isVM = true; vmType = vmType.empty() ? "VirtualBox" : vmType;
        }
        CloseServiceHandle(scm);
    }

    if (isVM) {
        Helper::printError("- Running inside a Virtual Machine (" + vmType + ")");
        Helper::recordResult("VM Detection", "FAIL", "Detected: " + vmType);
    }
    else {
        Helper::recordResult("VM Detection", "OK", "Bare metal");
    }
}

void Checks::checkForUpdate()
{
    SetConsoleTitleA("Checking for Updates");

    // Cache path with repo-validated cache
    auto getUpdateCachePath = [&]() -> std::string {
        char basePath[MAX_PATH];
        GetEnvironmentVariableA("LOCALAPPDATA", basePath, MAX_PATH);
        std::string dir = std::string(basePath) + "\\CL-Setup";
        CreateDirectoryA(dir.c_str(), NULL);
        return dir + "\\update-check.txt";
    };

    auto isCacheFresh = [](const std::string& path) -> bool {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
            return false;
        }
        FILETIME nowFileTime;
        GetSystemTimeAsFileTime(&nowFileTime);
        ULARGE_INTEGER now;
        now.LowPart = nowFileTime.dwLowDateTime;
        now.HighPart = nowFileTime.dwHighDateTime;
        ULARGE_INTEGER lastWrite;
        lastWrite.LowPart = data.ftLastWriteTime.dwLowDateTime;
        lastWrite.HighPart = data.ftLastWriteTime.dwHighDateTime;
        const unsigned long long oneWeek = 7ULL * 24ULL * 60ULL * 60ULL * 10000000ULL;
        return now.QuadPart >= lastWrite.QuadPart && (now.QuadPart - lastWrite.QuadPart) < oneWeek;
    };

    auto readCachedTag = [](const std::string& path, const std::string& expectedRepo) -> std::string {
        std::ifstream file(path);
        if (file.is_open()) {
            std::string storedRepo;
            std::getline(file, storedRepo);
            std::string tag;
            if (std::getline(file, tag)) {
                tag = trimCopy(tag);
                // v1.2.1: ignore poisoned/stale cache entries that are not version-like
                if (storedRepo == expectedRepo && looksLikeVersion(tag)) {
                    return tag;
                }
            }
        }
        return "";
    };

    auto writeCachedTag = [](const std::string& path, const std::string& tag, const std::string& repo) {
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << repo << "\n";
            file << tag << "\n";
        }
    };

    std::string cachePath = getUpdateCachePath();
    std::string tag = readCachedTag(cachePath, Helper::g_repoUrl);
    bool usedCache = isCacheFresh(cachePath) && !tag.empty();

    if (!usedCache) {
        std::string json = Helper::fetchURL(L"https://api.github.com/repos/" +
            std::wstring(Helper::g_repoUrl.begin(), Helper::g_repoUrl.end()) + L"/releases/latest");

        if (json.empty()) {
            if (!tag.empty()) {
                Helper::printConcern("- Could not refresh update info, using cached latest version: " + tag);
            }
            else {
                Helper::printConcern("- Could not check for updates (no response from GitHub)");
                Helper::recordResult("Auto-Update", "WARN", "Could not reach GitHub API");
                return;
            }
        }
        else {
            tag.clear();
            auto tagPos = json.find("\"tag_name\":\"");
            if (tagPos != std::string::npos) {
                tagPos += 12;
                auto endPos = json.find("\"", tagPos);
                if (endPos != std::string::npos) {
                    tag = json.substr(tagPos, endPos - tagPos);
                }
            }

            // v1.2.1: only accept real version tags (the API also returns
            // non-version tags, and garbage must never trigger the dialog)
            if (!looksLikeVersion(tag)) {
                Helper::printConcern("- Could not parse update info");
                Helper::recordResult("Auto-Update", "WARN", "Could not parse release info");
                return;
            }
            writeCachedTag(cachePath, tag, Helper::g_repoUrl);
        }
    }

    // v1.2.1: only warn when the remote tag is actually newer - equal or
    // older tags (dev builds, re-releases) must not show the dialog
    if (isNewerVersion(tag, Helper::g_appVersion)) {
        Helper::printConcern("- New version available: " + tag + " (current: v" + Helper::g_appVersion + ")");
        if (!Helper::cliConfig.headless) {
            MessageBoxA(GetConsoleWindow(),
                ("A newer Setup Checker version is available: " + tag +
                    "\n\nCurrent version: v" + Helper::g_appVersion +
                    "\n\nDownload it from the GitHub releases page.").c_str(),
                "Update available",
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        }
        Helper::recordResult("Auto-Update", "WARN", "New version: " + tag + (usedCache ? " (cached weekly check)" : ""));
    }
    else {
        Helper::printSuccess("- No new updates (v" + Helper::g_appVersion + ")", false);
        Helper::recordResult("Auto-Update", "OK",
            "Up to date (v" + Helper::g_appVersion + ")" + (usedCache ? " - checked this week" : ""));
    }
}

bool Checks::checkDefenderTamperProtection()
{
    DWORD tamperProtection = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows Defender\\Features", "TamperProtection", &tamperProtection)) {
        if (tamperProtection == 5) {
            Helper::printError("- Windows Defender Tamper Protection is ENABLED");
            Helper::printConcern("  Disable it manually: Windows Security > Virus & threat protection > Manage protection > Tamper Protection");
            Helper::recordResult("Defender Tamper Protection", "FAIL", "Enabled (value=5)");
            return true;
        }
    }
    Helper::recordResult("Defender Tamper Protection", "OK", "Disabled");
    return false;
}

bool Checks::checkAVTamperProtection(const std::string& avName)
{
    const std::vector<std::pair<std::string, std::string>> tamperServices = {
        {"Norton", "SAVService"},       {"Norton", "ccEvtMgr"},
        {"McAfee", "McShield"},         {"McAfee", "mfetd"},
        {"Kaspersky", "AVP"},           {"Kaspersky", "klnagent"},
        {"Bitdefender", "BDAuxSrv"},    {"Bitdefender", "EPSecurityService"},
        {"ESET", "ESET Service"},       {"ESET", "ekrn"},
        {"AVG", "avgsvc"},              {"Avast", "avast! Service"},
    };

    std::string lowerAV = avName;
    std::transform(lowerAV.begin(), lowerAV.end(), lowerAV.begin(), ::tolower);

    for (const auto& [name, service] : tamperServices) {
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerAV.find(lowerName) != std::string::npos) {
            ServiceStatus status = Helper::getServiceStatus(service.c_str());
            if (status == STATUS_SERVICE_RUNNING) {
                Helper::printError("- " + name + " Tamper Protection active (service: " + service + ")");
                Helper::printConcern("  Disable it manually in " + name + " settings before uninstalling");
                Helper::recordResult("AV Tamper Protection", "FAIL", name + " active");
                return true;
            }
        }
    }
    return false;
}

void Checks::checkWindowsDefender()
{
    SetConsoleTitleA("Checking Windows Defender");

    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender");

    if (checkDefenderTamperProtection()) {
        return;
    }

    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        DWORD err = GetLastError();
        Helper::printError("- Failed to check Windows Defender (Error 1, GLE=" + std::to_string(err) + ")");
        Helper::recordResult("Windows Defender", "FAIL", "SCM open failed");
        return;
    }

    SC_HANDLE service = OpenService(scm, "WinDefend", SERVICE_QUERY_STATUS);
    if (service == NULL) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            Helper::recordResult("Windows Defender", "OK", "Service removed");
        }
        else {
            Helper::printError("- Failed to check Windows Defender (Error 2, GLE=" + std::to_string(GetLastError()) + ")");
            Helper::recordResult("Windows Defender", "FAIL", "OpenService failed");
        }
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS_PROCESS status;
    DWORD bytesNeeded;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
        Helper::printError("- Failed to check Windows Defender (Error 3, GLE=" + std::to_string(GetLastError()) + ")");
        Helper::recordResult("Windows Defender", "FAIL", "QueryService failed");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return;
    }

    if (status.dwCurrentState != SERVICE_RUNNING && status.dwCurrentState != SERVICE_START_PENDING) {
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        Helper::printSuccess("- Windows Defender is disabled", false);
        Helper::recordResult("Windows Defender", "OK", "Disabled");
        return;
    }

    if (!Helper::requestChange("Disable Windows Defender service and policy settings")) {
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        Helper::recordResult("Windows Defender", "WARN", "Enabled - disable pending approval");
        return;
    }

    // Try to stop + disable
    CloseServiceHandle(service);
    service = OpenService(scm, "WinDefend", SERVICE_CHANGE_CONFIG | SERVICE_STOP | SERVICE_QUERY_STATUS);
    CloseServiceHandle(scm);

    bool serviceStopped = false;
    if (service != NULL) {
        SERVICE_STATUS stopStatus;
        if (ControlService(service, SERVICE_CONTROL_STOP, &stopStatus)) {
            Sleep(500);
            serviceStopped = true;
        }
        if (ChangeServiceConfig(service, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
            serviceStopped = true;
        }
        CloseServiceHandle(service);
    }

    // Always set registry policies
    HKEY hKey; DWORD val = 1;
    RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueEx(hKey, "DisableAntiSpyware", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegCloseKey(hKey);

    RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueEx(hKey, "DisableRealtimeMonitoring", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegSetValueEx(hKey, "DisableBehaviorMonitoring", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegSetValueEx(hKey, "DisableOnAccessProtection", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegSetValueEx(hKey, "DisableScanOnRealtimeEnable", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegCloseKey(hKey);

    // Undo-CL-Setup.ps1 re-enables the service from this marker
    Helper::logAppliedChange("service:WinDefend=disabled");
    Helper::logAppliedChange("reg:SOFTWARE\\Policies\\Microsoft\\Windows Defender");

    if (serviceStopped) {
        Helper::printSuccess("- Windows Defender disabled (service + policy)", true);
        Helper::recordResult("Windows Defender", "OK", "Disabled (changed)");
    }
    else {
        Helper::printSuccess("- Windows Defender policies set (restart required)", true);
        Helper::recordResult("Windows Defender", "OK", "Policy set, restart needed");
        Sleep(500);
        Helper::runSystemCommand("start https://www.sordum.org/9480/defender-control-v2-1/");
    }
    Helper::restartRequired = true;
}

void Checks::check3rdPartyAntiVirus()
{
    SetConsoleTitleA("Checking for 3rd Party Anti-Viruses");

    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");

    // v1.2.1: WMIC is removed by default on newer Windows 11 builds,
    // so query SecurityCenter2 through PowerShell/CIM instead (the same source
    // WMIC used). The marker tells us whether PowerShell actually ran.
    std::string psCommand =
        "powershell -NoProfile -NonInteractive -Command \""
        "(Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntiVirusProduct).displayName; 'CL-AV-DONE'"
        "\" 2>nul";
    std::string avOutput = Helper::runSystemCommandWithOutput(psCommand.c_str(), 30000);
    if (avOutput == "TIMEOUT") avOutput.clear();
    bool psWorked = avOutput.find("CL-AV-DONE") != std::string::npos;

    std::string antivirusList;
    if (psWorked) {
        std::stringstream avStream(avOutput);
        std::string line;
        while (std::getline(avStream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string wmicPrefix = "displayName="; // tolerate old WMIC-style output
            if (line.rfind(wmicPrefix, 0) == 0) line = line.substr(wmicPrefix.size());
            line = trimCopy(line);
            if (line.empty() || line == "CL-AV-DONE") continue;

            std::string lowered = line;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lowered.find("windows defender") != std::string::npos) continue;
            if (antivirusList.find(line) != std::string::npos) continue;

            if (!antivirusList.empty()) antivirusList += ", ";
            antivirusList += line;
        }
    }
    else {
        // Fallback without PowerShell/CIM: known AV products in the uninstall registry
        static const char* knownAv[] = {
            "norton", "mcafee", "kaspersky", "bitdefender", "eset", "avg", "avast",
            "avira", "sophos", "trend micro", "f-secure", "malwarebytes", "g data",
            "zonealarm", "comodo", "bullguard", "secureanywhere", "webroot"
        };
        HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
        const char* uninstallPaths[] = {
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
            "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
        };

        for (HKEY root : roots) {
            for (const char* uninstallPath : uninstallPaths) {
                HKEY hKey;
                if (RegOpenKeyEx(root, uninstallPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) continue;

                DWORD idx = 0;
                char subkeyName[256];
                DWORD subkeyNameSize = sizeof(subkeyName);
                while (RegEnumKeyEx(hKey, idx, subkeyName, &subkeyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                    HKEY hSubkey;
                    if (RegOpenKeyEx(hKey, subkeyName, 0, KEY_READ, &hSubkey) == ERROR_SUCCESS) {
                        char dn[256] = "";
                        DWORD dnSize = sizeof(dn);
                        if (RegQueryValueEx(hSubkey, "DisplayName", NULL, NULL, (LPBYTE)dn, &dnSize) == ERROR_SUCCESS) {
                            std::string display = trimCopy(dn);
                            std::string lowered = display;
                            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            for (const char* marker : knownAv) {
                                if (lowered.find(marker) != std::string::npos) {
                                    if (antivirusList.find(display) == std::string::npos) {
                                        if (!antivirusList.empty()) antivirusList += ", ";
                                        antivirusList += display;
                                    }
                                    break;
                                }
                            }
                        }
                        RegCloseKey(hSubkey);
                    }
                    subkeyNameSize = sizeof(subkeyName);
                    ++idx;
                }
                RegCloseKey(hKey);
            }
        }

        if (antivirusList.empty()) {
            Helper::printConcern("- Could not query antivirus products (PowerShell blocked, no known AV in registry)");
            Helper::recordResult("3rd Party AV", "WARN", "AV detection unavailable");
            return;
        }
    }

    if (!antivirusList.empty()) {
        // Check tamper protection before attempting uninstall
        std::string lowerList = antivirusList;
        std::transform(lowerList.begin(), lowerList.end(), lowerList.begin(), ::tolower);
        bool tamperActive = false;

        const std::vector<std::string> avNames = {
            "norton", "mcafee", "kaspersky", "bitdefender", "eset", "avg", "avast"
        };
        for (const auto& av : avNames) {
            if (lowerList.find(av) != std::string::npos) {
                if (checkAVTamperProtection(av)) tamperActive = true;
            }
        }

        if (tamperActive) {
            Helper::printConcern("- Cannot auto-uninstall while tamper protection is active");
            Helper::recordResult("3rd Party AV", "WARN", "Tamper protection active: " + antivirusList);
            return;
        }

        if (!Helper::requestChange("Attempt to uninstall detected third-party antivirus: " + antivirusList)) {
            Helper::recordResult("3rd Party AV", "WARN", "Found, uninstall pending approval: " + antivirusList);
            return;
        }

        // Try to auto-uninstall
        bool anyRemoved = false;
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD idx = 0; char subkeyName[256]; DWORD subkeyNameSize = sizeof(subkeyName);
            while (RegEnumKeyEx(hKey, idx, subkeyName, &subkeyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                HKEY hSubkey;
                if (RegOpenKeyEx(hKey, subkeyName, 0, KEY_READ, &hSubkey) == ERROR_SUCCESS) {
                    char dn[256] = ""; DWORD dnSize = sizeof(dn);
                    RegQueryValueEx(hSubkey, "DisplayName", NULL, NULL, (LPBYTE)dn, &dnSize);
                    std::string displayName(dn);
                    for (auto& av : {antivirusList}) {
                        size_t comma = 0;
                        while (comma != std::string::npos) {
                            std::string single = av.substr(comma == 0 ? 0 : comma + 2);
                            comma = av.find(",", comma == 0 ? 0 : comma + 2);
                            if (displayName.find(single) != std::string::npos) {
                                char uninstall[512] = ""; DWORD usSize = sizeof(uninstall);
                                if (RegQueryValueEx(hSubkey, "UninstallString", NULL, NULL, (LPBYTE)uninstall, &usSize) == ERROR_SUCCESS) {
                                    std::string cmd(uninstall);
                                    cmd += " /S /quiet /norestart";
                                    Helper::runSystemCommand(cmd.c_str());
                                    anyRemoved = true;
                                }
                                break;
                            }
                        }
                    }
                    RegCloseKey(hSubkey);
                }
                subkeyNameSize = sizeof(subkeyName); ++idx;
            }
            RegCloseKey(hKey);
        }

        if (anyRemoved) {
            Helper::printSuccess("- Attempted to remove 3rd party AV (" + antivirusList + ")", true);
            Helper::logAppliedChange("info:3rd party AV uninstall was attempted (" + antivirusList +
                ") - reinstall it manually if it is still needed");
            Helper::recordResult("3rd Party AV", "OK", "Uninstall attempted: " + antivirusList);
        } else {
            Helper::printError("- 3rd party Anti-Virus detected: " + antivirusList);
            Helper::recordResult("3rd Party AV", "FAIL", "Found: " + antivirusList);
        }
        return;
    }

    Helper::recordResult("3rd Party AV", "OK", "None detected");
}

void Checks::uninstallRiotVanguard()
{
    SetConsoleTitleA("Checking for Riot Vanguard");

    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        DWORD err = GetLastError();
        Helper::printError("- Failed to check if Riot Vanguard is installed (Error 6, GLE=" + std::to_string(err) + ")");
        Helper::recordResult("Riot Vanguard", "FAIL", "RegOpen failed");
        return;
    }

    DWORD subkeyIndex = 0;
    char subkeyName[256];
    DWORD subkeyNameSize = sizeof(subkeyName);
    while (RegEnumKeyEx(hKey, subkeyIndex, subkeyName, &subkeyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        HKEY hSubkey;
        result = RegOpenKeyEx(hKey, subkeyName, 0, KEY_READ, &hSubkey);
        if (result != ERROR_SUCCESS) { RegCloseKey(hKey); Helper::recordResult("Riot Vanguard", "FAIL", "RegEnum failed"); return; }

        char displayName[256];
        DWORD displayNameSize = sizeof(displayName);
        result = RegQueryValueEx(hSubkey, "DisplayName", NULL, NULL, (LPBYTE)displayName, &displayNameSize);
        if (result == ERROR_SUCCESS && strcmp(displayName, "Riot Vanguard") == 0) {
            RegCloseKey(hSubkey);
            RegCloseKey(hKey);

            if (std::filesystem::exists("C:\\Program Files\\Riot Vanguard\\installer.exe") ||
                std::filesystem::exists("C:\\Program Files (x86)\\Riot Vanguard\\installer.exe")) {
                if (!Helper::requestChange("Uninstall Riot Vanguard")) {
                    Helper::printError("- Riot Vanguard is installed");
                    Helper::recordResult("Riot Vanguard", "WARN", "Installed - uninstall pending approval");
                    return;
                }

                if (!Helper::cliConfig.headless) {
                    MessageBoxA(NULL, "When prompted to uninstall Vanguard, press YES", "Uninstall Vanguard", MB_ICONINFORMATION | MB_TOPMOST);
                }

                const char* vgPath = std::filesystem::exists("C:\\Program Files\\Riot Vanguard\\installer.exe")
                    ? "C:\\Program Files\\Riot Vanguard\\installer.exe"
                    : "C:\\Program Files (x86)\\Riot Vanguard\\installer.exe";
                _spawnl(_P_WAIT, vgPath, "installer.exe", NULL);
                Helper::logAppliedChange("info:Riot Vanguard uninstall was started - reinstall it manually if it is still needed");
                Helper::printError("- Riot Vanguard needs to be uninstalled");
                Helper::recordResult("Riot Vanguard", "FAIL", "Installed - prompted uninstall");
                return;
            }
            Helper::printError("- Failed to uninstall Riot Vanguard, manually uninstall (Error 8)");
            Helper::recordResult("Riot Vanguard", "FAIL", "Installed - manual uninstall needed");
            return;
        }
        RegCloseKey(hSubkey);
        subkeyNameSize = sizeof(subkeyName);
        ++subkeyIndex;
    }
    RegCloseKey(hKey);
    Helper::printSuccess("- Riot Vanguard is not installed", false);
    Helper::recordResult("Riot Vanguard", "OK", "Not installed");
}

void Checks::installVCRedist()
{
    SetConsoleTitleA("Downloading VCRedist");
    Helper::vcComplete = false;

    bool alreadyInstalled = (std::filesystem::exists("C:\\Windows\\System32\\vcruntime140.dll") ||
                             std::filesystem::exists("C:\\Windows\\SysWOW64\\vcruntime140.dll")) &&
                            (std::filesystem::exists("C:\\Windows\\System32\\msvcp140.dll") ||
                             std::filesystem::exists("C:\\Windows\\SysWOW64\\msvcp140.dll"));
    if (alreadyInstalled) {
        Helper::printSuccess("- VCRedist is already installed", false);
        Helper::recordResult("VC Redist", "OK", "Already installed");
        Helper::vcComplete = true;
        return;
    }

    if (!Helper::requestChange("Download and install Microsoft Visual C++ Redistributable")) {
        Helper::printConcern("- VCRedist is missing");
        Helper::recordResult("VC Redist", "WARN", "Missing - install pending approval");
        Helper::vcComplete = true;
        return;
    }

    HRESULT downloadX64 = URLDownloadToFileA(NULL, "https://aka.ms/vs/17/release/vc_redist.x64.exe",
        "C:\\Windows\\VC_redist.x64.exe", 0, NULL);
    HRESULT downloadX86 = URLDownloadToFileA(NULL, "https://aka.ms/vs/17/release/vc_redist.x86.exe",
        "C:\\Windows\\VC_redist.x86.exe", 0, NULL);

    if (downloadX64 != S_OK || !std::filesystem::exists("C:\\Windows\\VC_redist.x64.exe")) {
        Helper::printError("- Failed to download VCRedist x64 (Error 9, HR=" + std::to_string(downloadX64) + ")");
        Helper::recordResult("VC Redist", "FAIL", "Download x64 failed");
        Sleep(1000);
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x64.exe");
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x86.exe");
        Helper::vcComplete = true;
        return;
    }
    if (downloadX86 != S_OK || !std::filesystem::exists("C:\\Windows\\VC_redist.x86.exe")) {
        Helper::printError("- Failed to download VCRedist x86 (Error 10, HR=" + std::to_string(downloadX86) + ")");
        Helper::recordResult("VC Redist", "FAIL", "Download x86 failed");
        Sleep(1000);
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x64.exe");
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x86.exe");
        Helper::vcComplete = true;
        return;
    }

    SetConsoleTitleA("Installing VCRedist");
    Helper::runSystemCommand("C:\\Windows\\VC_redist.x64.exe /install /q /norestart");
    Helper::runSystemCommand("C:\\Windows\\VC_redist.x86.exe /install /q /norestart");
    Helper::logAppliedChange("info:Microsoft Visual C++ Redistributable was installed - uninstall it via Apps & Features if unwanted");

    if (!(std::filesystem::exists("C:\\Windows\\System32\\vcruntime140.dll") ||
          std::filesystem::exists("C:\\Windows\\SysWOW64\\vcruntime140.dll")) ||
        !(std::filesystem::exists("C:\\Windows\\System32\\msvcp140.dll") ||
          std::filesystem::exists("C:\\Windows\\SysWOW64\\msvcp140.dll"))) {
        Helper::printError("- VCRedist didn't install correctly (Error 11)");
        Helper::recordResult("VC Redist", "FAIL", "Install verify failed");
        Sleep(1000);
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x64.exe");
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x86.exe");
        Helper::vcComplete = true;
        return;
    }

    Helper::printSuccess("- VCRedist is installed", false);
    Helper::recordResult("VC Redist", "OK", "Installed successfully");
    Helper::vcComplete = true;
}

void Checks::checkSecureBoot()
{
    SetConsoleTitleA("Checking Secure-Boot");

    // v1.2.1: know the boot mode first - legacy BIOS systems cannot have Secure Boot at all
    DWORD firmwareType = 0; // 0 = unknown, 1 = BIOS, 2 = UEFI
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32 != NULL) {
        typedef BOOL(WINAPI* GetFirmwareTypeFn)(DWORD*);
        GetFirmwareTypeFn getFirmwareType =
            reinterpret_cast<GetFirmwareTypeFn>(GetProcAddress(kernel32, "GetFirmwareType"));
        if (getFirmwareType != NULL) getFirmwareType(&firmwareType);
    }

    if (firmwareType == 1) {
        Helper::printSuccess("- Secure-Boot is not available (legacy BIOS boot)", false);
        Helper::recordResult("Secure Boot", "OK", "Legacy BIOS - not available");
        return;
    }

    DWORD secbootStatus = 0;
    bool stateRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", "UEFISecureBootEnabled", &secbootStatus);

    if (!stateRead) {
        if (firmwareType == 2) {
            Helper::printConcern("- UEFI boot detected but the Secure-Boot state could not be read");
            Helper::recordResult("Secure Boot", "WARN", "UEFI - state unreadable");
        }
        else {
            Helper::printError("- Unable to check Secure-Boot Status (Error 12)");
            Helper::recordResult("Secure Boot", "FAIL", "Unknown state");
        }
        return;
    }

    if (secbootStatus == 0x00000000) {
        Helper::printSuccess("- Secure-Boot is disabled (UEFI)", false);
        Helper::recordResult("Secure Boot", "OK", "Disabled (UEFI)");
    }
    else if (secbootStatus == 0x00000001) {
        Helper::printError("- Secure-Boot is enabled, disable it in BIOS " + biosHint("secureboot"));
        Helper::recordResult("Secure Boot", "FAIL", "Enabled");
    }
    else {
        Helper::printError("- Unable to check Secure-Boot Status (Error 12, state=" +
            std::to_string(secbootStatus) + ")");
        Helper::recordResult("Secure Boot", "FAIL", "Unknown state");
    }
}

void Checks::isChromeInstalled()
{
    SetConsoleTitleA("Checking for Google Chrome");

    std::vector<std::wstring> paths = {
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
    };
    wchar_t localAppData[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        paths.push_back(std::wstring(localAppData) + L"\\Google\\Chrome\\Application\\chrome.exe");
    }
    for (auto& p : paths) {
        if (std::filesystem::exists(p)) {
            Helper::printSuccess("- Google Chrome is installed", false);
            Helper::recordResult("Google Chrome", "OK", "Installed");
            return;
        }
    }
    Helper::printError("- Google Chrome is not installed");
    if (!Helper::requestChange("Open Google Chrome download page")) {
        Helper::recordResult("Google Chrome", "WARN", "Not installed - download pending approval");
        return;
    }
    Helper::recordResult("Google Chrome", "FAIL", "Not installed");
    Sleep(1000);
    Helper::runSystemCommand("start https://www.google.com/chrome/");
}

void Checks::syncWindowsTime()
{
    SetConsoleTitleA("Syncing Windows Time");

    // First check if time service is already running
    ServiceStatus currentStatus = Helper::getServiceStatus("W32Time");
    if (currentStatus == STATUS_SERVICE_RUNNING) {
        // Already running - just resync
        Helper::runSystemCommand("w32tm /resync");
        Helper::printSuccess("- Windows Time is running", false);
        Helper::recordResult("Time Sync", "OK", "Running, resynced");
        return;
    }

    SC_HANDLE scmHandle = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scmHandle == NULL) {
        DWORD err = GetLastError();
        Helper::printError("- Failed to sync Windows Time (Error 14, GLE=" + std::to_string(err) + ")");
        Helper::recordResult("Time Sync", "FAIL", "SCM open failed");
        return;
    }
    if (!Helper::requestChange("Repair and start Windows Time service")) {
        CloseServiceHandle(scmHandle);
        Helper::recordResult("Time Sync", "WARN", "Disabled - repair pending approval");
        return;
    }

    Helper::runSystemCommand("w32tm /register");

    if (Helper::getServiceStatus("W32Time") == STATUS_SERVICE_STOPPED) {
        SC_HANDLE serviceHandle = OpenService(scmHandle, "W32Time", SERVICE_ALL_ACCESS);
        if (serviceHandle == NULL) {
            Helper::printError("- Failed to sync Windows Time (Error 15)");
            Helper::recordResult("Time Sync", "FAIL", "OpenService failed");
            CloseServiceHandle(scmHandle);
            return;
        }
        DWORD bytesNeeded = 0;
        QueryServiceConfig(serviceHandle, NULL, 0, &bytesNeeded);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytesNeeded == 0) {
            Helper::printError("- Failed to sync Windows Time (Error 16)");
            Helper::recordResult("Time Sync", "FAIL", "Config query failed");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        std::vector<BYTE> buffer(bytesNeeded);
        LPQUERY_SERVICE_CONFIG serviceConfig = (LPQUERY_SERVICE_CONFIG)buffer.data();
        if (!QueryServiceConfig(serviceHandle, serviceConfig, bytesNeeded, &bytesNeeded)) {
            Helper::printError("- Failed to sync Windows Time (Error 16)");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        if (serviceConfig->dwStartType == SERVICE_DISABLED) {
            Helper::printError("- Failed to sync Windows Time (Error 17)");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        if (!ChangeServiceConfig(serviceHandle, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
            Helper::printError("- Failed to sync Windows Time (Error 18)");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        // only when the start type really changed: Undo-CL-Setup.ps1 sets it back to demand
        if (serviceConfig->dwStartType != SERVICE_AUTO_START) {
            Helper::logAppliedChange("service:W32Time=auto");
        }
        if (StartService(serviceHandle, 0, NULL) == FALSE) {
            Helper::printError("- Failed to sync Windows Time (Error 19)");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(scmHandle);
        Sleep(250);
    }
    else { CloseServiceHandle(scmHandle); }

    Helper::runSystemCommand("w32tm /resync");
    Helper::printSuccess("- Successfully synced Windows time", true);
    Helper::recordResult("Time Sync", "OK", "Synced (was disabled)");
}

void Checks::disableChromeProtection()
{
    SetConsoleTitleA("Disabling Google Chrome Protection");

    bool chromeInstalled = std::filesystem::exists(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe") ||
                           std::filesystem::exists(L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe");
    if (!chromeInstalled) {
        Helper::recordResult("Chrome Protection", "SKIPPED", "Chrome not installed");
        return;
    }

    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Google\\Chrome");

    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Google\\Chrome",
        "SafeBrowsingProtectionLevel", &status)) {
        if (status == 0x00000001) {
            Helper::printSuccess("- Chrome protection is already disabled", false);
            Helper::recordResult("Chrome Protection", "OK", "Already disabled");
            return;
        }
    }

    if (!Helper::requestChange("Disable Chrome Safe Browsing protection policy")) {
        Helper::recordResult("Chrome Protection", "WARN", "Enabled - policy change pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 0x00000001;
    LONG createKey = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Google\\Chrome",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG createDWORD = RegSetValueEx(hKey, "SafeBrowsingProtectionLevel", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);
    Helper::logAppliedChange("reg:SOFTWARE\\Policies\\Google\\Chrome");

    if (createKey == ERROR_SUCCESS && createDWORD == ERROR_SUCCESS) {
        Helper::printSuccess("- Successfully disabled Chrome protection", true);
        Helper::recordResult("Chrome Protection", "OK", "Disabled (changed)");
    }
    else {
        Helper::printError("- Failed to disable Chrome protection (Error 20/21)");
        Helper::recordResult("Chrome Protection", "FAIL", "Registry write failed");
    }
}

void Checks::checkWinver()
{
    SetConsoleTitleA("Checking Winver");
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        Helper::printError("- Failed to check Winver (Error 22)");
        Helper::recordResult("Winver", "FAIL", "Registry open failed");
        return;
    }
    char buildStr[32]; DWORD buildSize = sizeof(buildStr);
    LONG ret = RegQueryValueEx(hKey, "CurrentBuild", NULL, NULL, (LPBYTE)buildStr, &buildSize);
    RegCloseKey(hKey);
    if (ret != ERROR_SUCCESS) {
        Helper::printError("- Failed to check Winver (Error 22)");
        Helper::recordResult("Winver", "FAIL", "Read failed");
        return;
    }
    int build = std::stoi(buildStr);

    char productName[128] = ""; DWORD productSize = sizeof(productName);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueEx(hKey, "ProductName", NULL, NULL, (LPBYTE)productName, &productSize);
        RegCloseKey(hKey);
    }
    std::string winver(productName);

    std::map<int, std::string> build_map = {
        {10240, "Win10 1507"}, {10586, "Win10 1511"}, {14393, "Win10 1607"}, {15063, "Win10 1703"},
        {16299, "Win10 1709"}, {17134, "Win10 1803"}, {17763, "Win10 1809"}, {18362, "Win10 1903"},
        {19041, "Win10 2004"}, {19042, "Win10 20H2"}, {19043, "Win10 21H1"}, {19044, "Win10 21H2"},
        {19045, "Win10 22H2"}, {22000, "Win11 21H2"}, {22621, "Win11 22H2"}, {22631, "Win11 23H2"},
        {26100, "Win11 24H2"}, {26200, "Win11 25H2"},
    };
    int min_build = 19041;
    std::vector<int> trouble = {19045, 22621};

    // 25H2 and every build we do not know yet are 50/50: works for some users, breaks for others
    const int fiftyFiftyFrom = 26200;

    auto it = build_map.find(build);
    if (it != build_map.end()) { winver = it->second; }
    else if (build >= fiftyFiftyFrom) { winver = "Build " + std::to_string(build); }

    if (build < min_build) {
        Helper::printError("- Winver \"" + winver + "\" is unsupported, please upgrade");
        Helper::recordResult("Winver", "FAIL", "Unsupported: " + winver + " (Build " + std::to_string(build) + ")");
    }
    else if (build >= fiftyFiftyFrom ||
        std::find(trouble.begin(), trouble.end(), build) != trouble.end()) {
        Helper::printConcern("- Winver \"" + winver + "\" is a 50/50, if error contact support");
        Helper::recordResult("Winver", "WARN", winver + " is a 50/50, if error contact support");
    }
    else {
        Helper::printSuccess("- Winver is supported (" + winver + ")", false);
        Helper::recordResult("Winver", "OK", winver);
    }
}

void Checks::deleteSymbols()
{
    SetConsoleTitleA("Deleting C:\\Symbols");
    std::string path = "C:\\Symbols";
    if (std::filesystem::exists(path)) {
        if (!Helper::requestChange("Delete " + path)) {
            Helper::recordResult("Symbols", "WARN", "Present - delete pending approval");
            return;
        }

        std::error_code ec;
        if (!std::filesystem::remove_all(path, ec)) {
            Helper::printError("- Unable to delete " + path + " (Error 24, " + ec.message() + ")");
            Helper::recordResult("Symbols", "FAIL", "Delete failed: " + ec.message());
            return;
        }
        Helper::printSuccess("- Successfully deleted " + path, true);
        Helper::logAppliedChange("info:C:\\Symbols was deleted and cannot be restored automatically");
        Helper::recordResult("Symbols", "OK", "Deleted (changed)");
        return;
    }
    Helper::printSuccess("- C:\\Symbols does not exist", false);
    Helper::recordResult("Symbols", "OK", "Not present");
}

void Checks::checkFastBoot()
{
    SetConsoleTitleA("Checking Fast-Boot");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power");
    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", "HiberbootEnabled", &status)) {
        if (status == 0) {
            Helper::printSuccess("- Fast-Boot is disabled", false);
            Helper::recordResult("Fast Boot", "OK", "Already disabled");
            return;
        }
    }
    if (!Helper::requestChange("Disable Fast Boot")) {
        Helper::recordResult("Fast Boot", "WARN", "Enabled - disable pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 0;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "HiberbootEnabled", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);
    Helper::logAppliedChange("reg:SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power");
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) {
        Helper::printSuccess("- Successfully disabled Fast-Boot", true);
        Helper::restartRequired = true;
        Helper::recordResult("Fast Boot", "OK", "Disabled (changed)");
    }
    else {
        Helper::printError("- Failed to disable Fast-Boot (Error 31/32)");
        Helper::recordResult("Fast Boot", "FAIL", "Registry write failed");
    }
}

void Checks::checkExploitProtection()
{
    SetConsoleTitleA("Checking Exploit-Protection");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection");
    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection",
        "DisallowExploitProtectionOverride", &status)) {
        if (status == 1) {
            Helper::printSuccess("- Exploit-Protection is disabled", false);
            Helper::recordResult("Exploit Protection", "OK", "Already disabled");
            return;
        }
    }
    if (!Helper::requestChange("Disable Exploit Protection override")) {
        Helper::recordResult("Exploit Protection", "WARN", "Enabled - disable pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 1;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "DisallowExploitProtectionOverride", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);
    Helper::logAppliedChange("reg:SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection");
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) {
        Helper::printSuccess("- Successfully disabled Exploit-Protection", true);
        Helper::restartRequired = true;
        Helper::recordResult("Exploit Protection", "OK", "Disabled (changed)");
    }
    else {
        Helper::printError("- Failed to disable Exploit-Protection (Error 25/26)");
        Helper::recordResult("Exploit Protection", "FAIL", "Registry write failed");
    }
}

void Checks::checkSmartScreen()
{
    SetConsoleTitleA("Checking SmartScreen");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\System");
    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows\\System", "EnableSmartScreen", &status)) {
        if (status == 0) {
            Helper::printSuccess("- SmartScreen is disabled", false);
            Helper::recordResult("SmartScreen", "OK", "Already disabled");
            return;
        }
    }
    if (!Helper::requestChange("Disable SmartScreen")) {
        Helper::recordResult("SmartScreen", "WARN", "Enabled - disable pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 0;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\System",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "EnableSmartScreen", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);
    Helper::logAppliedChange("reg:SOFTWARE\\Policies\\Microsoft\\Windows\\System");
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) {
        Helper::printSuccess("- Successfully disabled SmartScreen", true);
        Helper::restartRequired = true;
        Helper::recordResult("SmartScreen", "OK", "Disabled (changed)");
    }
    else {
        Helper::printError("- Failed to disable SmartScreen (Error 27/28)");
        Helper::recordResult("SmartScreen", "FAIL", "Registry write failed");
    }
}

void Checks::checkGameBar()
{
    SetConsoleTitleA("Checking Xbox Gamebar");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR");
    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR", "AppCaptureEnabled", &status)) {
        if (status == 1) {
            Helper::printSuccess("- Gamebar is enabled", false);
            Helper::recordResult("Game Bar", "OK", "Already enabled");
            return;
        }
    }
    if (!Helper::requestChange("Enable Xbox Game Bar capture setting")) {
        Helper::recordResult("Game Bar", "WARN", "Disabled - enable pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 1;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "AppCaptureEnabled", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);
    Helper::logAppliedChange("reg:SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR");
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) {
        Helper::printSuccess("- Successfully enabled Gamebar", true);
        Helper::restartRequired = true;
        Helper::recordResult("Game Bar", "OK", "Enabled (changed)");
    }
    else {
        Helper::printError("- Failed to enable Gamebar (Error 29/30)");
        Helper::recordResult("Game Bar", "FAIL", "Registry write failed");
    }
}

void Checks::checkModifiedOS()
{
    SetConsoleTitleA("Checking if OS is modified");
    bool modified = false;
    std::string reasons;

    auto checkService = [&](const char* name, const char* label) {
        SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
        if (scm) {
            SC_HANDLE svc = OpenService(scm, name, SERVICE_QUERY_STATUS);
            if (svc == NULL) { modified = true; reasons += std::string(" ") + label + " missing;"; }
            else CloseServiceHandle(svc);
            CloseServiceHandle(scm);
        }
    };
    checkService("WinDefend", "WinDefend");
    checkService("wscsvc", "SecurityCenter");
    checkService("wuauserv", "WindowsUpdate");
    checkService("BFE", "BFE");
    checkService("mpssvc", "Firewall");

    HKEY hKey;
    auto checkReg = [&](const char* key, const char* label) {
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            modified = true; reasons += std::string(" ") + label + ";"; RegCloseKey(hKey);
        }
    };
    checkReg("SOFTWARE\\AtlasOS", "AtlasOS");
    checkReg("SOFTWARE\\ReviOS", "ReviOS");
    checkReg("SOFTWARE\\GhostSpectre", "GhostSpectre");
    checkReg("SOFTWARE\\GGOS", "GGOS");
    checkReg("SOFTWARE\\PhoenixOS", "PhoenixOS");
    checkReg("SOFTWARE\\KernelOS", "KernelOS");
    checkReg("SOFTWARE\\XOS", "XOS");
    checkReg("SOFTWARE\\OptimusOS", "OptimusOS");

    char productName[256] = ""; DWORD size = sizeof(productName);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueEx(hKey, "ProductName", NULL, NULL, (LPBYTE)productName, &size);
        RegCloseKey(hKey);
    }
    std::string pn(productName);
    std::vector<std::string> markers = {"Atlas","Revi","Ghost","Spectre","Tiny","Lite","X-Lite","Micro","XOS","Optimus","SuperLite","Compact","Minimal","Gaming"};
    for (auto& m : markers) if (pn.find(m) != std::string::npos) { modified = true; reasons += " ProductName(" + m + ");"; break; }

    char editionID[256] = ""; DWORD edSize = sizeof(editionID);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, "EditionID", NULL, NULL, (LPBYTE)editionID, &edSize) == ERROR_SUCCESS) {
            std::string ed(editionID);
            if (ed.find("Custom") != std::string::npos || ed.find("Gaming") != std::string::npos) {
                modified = true; reasons += " Edition(" + ed + ");";
            }
        }
        RegCloseKey(hKey);
    }

    if (modified) {
        Helper::printConcern("- OS appears MODIFIED:" + reasons);
        Helper::recordResult("Modified OS", "WARN", "Modified:" + reasons);
    }
    else {
        Helper::printSuccess("- The OS appears unmodified", false);
        Helper::recordResult("Modified OS", "OK", "Clean");
    }
}

void Checks::checkTPMStatus()
{
    SetConsoleTitleA("Checking TPM");

    const char* tpmKey = "SYSTEM\\CurrentControlSet\\Control\\TPM";

    // v1.2.1: read every state value Windows exposes instead of the single TPMActive entry
    DWORD present = 0, enabled = 0, activated = 0, ready = 0, tpmActive = 0;
    bool presentRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, tpmKey, "Present", &present);
    bool enabledRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, tpmKey, "Enabled", &enabled);
    bool activatedRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, tpmKey, "Activated", &activated);
    bool readyRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, tpmKey, "Ready", &ready);
    bool activeRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, tpmKey, "TPMActive", &tpmActive);

    std::string specVersion = readRegistryStringValue(HKEY_LOCAL_MACHINE, tpmKey, "SpecVersion");
    std::string manufacturer = readRegistryStringValue(HKEY_LOCAL_MACHINE, tpmKey, "ManufacturerIdTxt");

    std::string detail;
    if (!specVersion.empty()) detail += " (" + specVersion;
    if (!manufacturer.empty()) detail += detail.empty() ? " (" + manufacturer : ", " + manufacturer;
    if (!detail.empty()) detail += ")";

    bool registryUsable = presentRead || enabledRead || activatedRead || readyRead || activeRead;
    if (registryUsable) {
        bool tpmPresent = (presentRead && present == 1) || (activeRead && tpmActive == 1);
        bool tpmRunning = (enabledRead && enabled == 1) ||
                          (activatedRead && activated == 1) ||
                          (readyRead && ready == 1);

        if (tpmPresent && tpmRunning) {
            Helper::printError("- TPM is enabled" + detail + ", disable it in BIOS " + biosHint("tpm"));
            Helper::recordResult("TPM", "FAIL", "Enabled" + detail);
        }
        else if (tpmPresent) {
            Helper::printSuccess("- TPM present but not usable (disabled in BIOS)" + detail, false);
            Helper::recordResult("TPM", "OK", "Present but not usable" + detail);
        }
        else {
            Helper::printSuccess("- TPM is disabled / not found" + detail, false);
            Helper::recordResult("TPM", "OK", "Disabled/not found" + detail);
        }
        return;
    }

    // No registry state: ask the TPM WMI provider (same source as tpm.msc / Get-Tpm)
    std::string tpmCommand =
        "powershell -NoProfile -NonInteractive -Command \""
        "(Get-CimInstance -Namespace root/cimv2/security/microsofttpm -ClassName Win32_Tpm | "
        "ForEach-Object { $_.SpecVersion + '|' + $_.IsEnabled_InitialValue + '|' + $_.IsActivated_InitialValue })\" 2>nul";
    std::string output = trimCopy(Helper::runSystemCommandWithOutput(tpmCommand.c_str(), 20000));
    if (output == "TIMEOUT") output.clear();

    size_t firstSep = output.find('|');
    if (firstSep == std::string::npos) {
        Helper::printSuccess("- TPM is disabled / not usable (no TPM reported by Windows)", false);
        Helper::recordResult("TPM", "OK", "Disabled/not usable");
        return;
    }

    size_t secondSep = output.find('|', firstSep + 1);
    std::string spec = trimCopy(output.substr(0, firstSep));
    std::string isEnabled = toLowerCopy(trimCopy(output.substr(firstSep + 1,
        secondSep == std::string::npos ? std::string::npos : secondSep - firstSep - 1)));
    std::string isActivated = secondSep == std::string::npos ? "" :
        toLowerCopy(trimCopy(output.substr(secondSep + 1)));

    bool tpmEnabledFlag = (isEnabled == "true");
    bool tpmActivatedFlag = (isActivated == "true");
    std::string wmiDetail = spec.empty() ? "" : " (" + spec + ")";

    if (tpmEnabledFlag && tpmActivatedFlag) {
        Helper::printError("- TPM is enabled" + wmiDetail + ", disable it in BIOS " + biosHint("tpm"));
        Helper::recordResult("TPM", "FAIL", "Enabled" + wmiDetail);
    }
    else if (tpmEnabledFlag || tpmActivatedFlag) {
        Helper::printConcern("- TPM is partially available (Enabled=" + isEnabled +
            ", Activated=" + isActivated + ")" + wmiDetail + " " + biosHint("tpm"));
        Helper::recordResult("TPM", "WARN", "Partially available" + wmiDetail);
    }
    else {
        Helper::printSuccess("- TPM is disabled / not usable" + wmiDetail, false);
        Helper::recordResult("TPM", "OK", "Disabled/not usable" + wmiDetail);
    }
}

void Checks::checkCoreIsolation()
{
    SetConsoleTitleA("Checking Core Isolation (VBS/HVCI)");

    // Runs before checkSecurityMitigations, so these are the pre-change exports
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity");

    DWORD hvci = 0;
    bool hvciRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", "Enabled", &hvci);

    DWORD vbs = 0;
    bool vbsRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "EnableVirtualizationBasedSecurity", &vbs);

    if (hvciRead && hvci == 0 && vbsRead && vbs == 0) {
        Helper::printSuccess("- Core Isolation (VBS/HVCI) is disabled", false);
        Helper::recordResult("Core Isolation", "OK", "Disabled");
        return;
    }

    // v1.2.2: register BOTH pendings before returning. The old early return after the
    // first unapproved change silently dropped the VBS pending from the approval list.
    bool hvciApproved = (hvci != 1) || Helper::requestChange("Disable Core Isolation HVCI");
    bool vbsApproved = (vbs != 1) || Helper::requestChange("Disable Virtualization Based Security");
    if (!hvciApproved || !vbsApproved) {
        std::string pending;
        if (hvci == 1) pending += "HVCI";
        if (vbs == 1) {
            if (!pending.empty()) pending += " + ";
            pending += "VBS";
        }
        Helper::recordResult("Core Isolation", "WARN", pending + " enabled - disable pending approval");
        return;
    }

    bool changed = false;
    std::string changedList;
    std::string failedList;
    HKEY hKey;

    auto appendName = [](std::string& list, const char* name) {
        if (!list.empty()) list += " + ";
        list += name;
    };

    if (hvci == 1) {
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            DWORD val = 0;
            if (RegSetValueEx(hKey, "Enabled", 0, REG_DWORD, (BYTE*)&val, sizeof(val)) == ERROR_SUCCESS) {
                Helper::logAppliedChange("reg:SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity");
                Helper::printSuccess("- Disabled HVCI", true);
                appendName(changedList, "HVCI");
                changed = true;
            }
            else {
                Helper::printError("- Failed to disable HVCI (access denied?)");
                Helper::printConcern("  Disable manually: Windows Security > Device Security > Core Isolation");
                appendName(failedList, "HVCI");
            }
            RegCloseKey(hKey);
        }
        else {
            appendName(failedList, "HVCI");
        }
    }

    if (vbs == 1) {
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            DWORD val = 0;
            if (RegSetValueEx(hKey, "EnableVirtualizationBasedSecurity", 0, REG_DWORD, (BYTE*)&val, sizeof(val)) == ERROR_SUCCESS) {
                Helper::logAppliedChange("reg:SYSTEM\\CurrentControlSet\\Control\\DeviceGuard");
                if (!changed) Helper::printSuccess("- Disabled VBS", true);
                appendName(changedList, "VBS");
                changed = true;
            }
            else {
                appendName(failedList, "VBS");
            }
            RegCloseKey(hKey);
        }
        else {
            appendName(failedList, "VBS");
        }
    }

    if (changed) {
        Helper::restartRequired = true;
    }

    // v1.2.2: exactly one result per check (upsert keeps the last one anyway)
    if (changed && failedList.empty()) {
        Helper::recordResult("Core Isolation", "OK", changedList + " disabled (changed)");
    }
    else if (changed) {
        Helper::recordResult("Core Isolation", "WARN",
            changedList + " disabled (changed); failed: " + failedList);
    }
    else if (!failedList.empty()) {
        Helper::printConcern("- Could not disable Core Isolation, disable manually in Windows Security");
        Helper::recordResult("Core Isolation", "FAIL",
            "Manual disable needed (" + failedList + " write failed)");
    }
    else {
        Helper::printSuccess("- Core Isolation (VBS/HVCI) is disabled", false);
        Helper::recordResult("Core Isolation", "OK", "Disabled");
    }
}

void Checks::checkDMAProtection()
{
    SetConsoleTitleA("Checking Kernel DMA Protection");
    DWORD dma = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\KernelDmaProtection", "Enabled", &dma)) {
        if (dma == 0) {
            Helper::printSuccess("- Kernel DMA Protection is disabled", false);
            Helper::recordResult("DMA Protection", "OK", "Disabled");
            return;
        }
        else {
            Helper::printError("- Kernel DMA Protection is enabled");
            Helper::recordResult("DMA Protection", "FAIL", "Enabled");
            return;
        }
    }
    DWORD dmaState = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "KernelDmaProtectionState", &dmaState)) {
        if (dmaState == 0) {
            Helper::printSuccess("- Kernel DMA Protection is disabled", false);
            Helper::recordResult("DMA Protection", "OK", "Disabled");
        }
        else {
            Helper::printError("- Kernel DMA Protection is enabled (" + std::to_string(dmaState) + ")");
            Helper::recordResult("DMA Protection", "FAIL", "State=" + std::to_string(dmaState));
        }
        return;
    }

    // v1.2.1: registry has no state -> ask Win32_DeviceGuard (the source msinfo32 uses)
    std::string deviceGuardCommand =
        "powershell -NoProfile -NonInteractive -Command \""
        "(Get-CimInstance -Namespace root/Microsoft/Windows/DeviceGuard -ClassName Win32_DeviceGuard)"
        ".AvailableSecurityProperties -join ','\" 2>nul";
    std::string available = trimCopy(Helper::runSystemCommandWithOutput(deviceGuardCommand.c_str(), 20000));
    if (available == "TIMEOUT") available.clear();

    if (!available.empty() && csvContains(available, 3)) {
        Helper::printSuccess("- Kernel DMA Protection is supported by the firmware, currently off", false);
        Helper::recordResult("DMA Protection", "OK", "Supported, not active (msinfo32: Off)");
    }
    else {
        Helper::printSuccess("- Kernel DMA Protection is not active", false);
        Helper::recordResult("DMA Protection", "OK", "Not active/not supported");
    }
}

void Checks::checkUAC()
{
    SetConsoleTitleA("Checking UAC");
    DWORD uacEnabled = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        "EnableLUA", &uacEnabled)) {
        if (uacEnabled == 0) {
            Helper::printSuccess("- UAC notifications are disabled", false);
            Helper::recordResult("UAC", "OK", "Disabled");
        }
        else {
            Helper::printConcern("- UAC is enabled, consider disabling UAC");
            Helper::recordResult("UAC", "WARN", "Enabled - consider disabling");
        }
    }
    else {
        Helper::printConcern("- Could not read UAC status");
        Helper::recordResult("UAC", "WARN", "Could not read registry");
    }
}

void Checks::checkWindowsUpdate()
{
    SetConsoleTitleA("Checking Windows Update Service");

    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        Helper::printConcern("- Could not check Windows Update service status");
        Helper::recordResult("Windows Update", "WARN", "SCM open failed");
        return;
    }

    SC_HANDLE service = OpenService(scm, "wuauserv", SERVICE_QUERY_STATUS);
    if (service == NULL) {
        Helper::printConcern("- Windows Update service not found");
        Helper::recordResult("Windows Update", "WARN", "Service not found");
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS_PROCESS status;
    DWORD bytesNeeded;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
        Helper::printConcern("- Could not query Windows Update service status");
        Helper::recordResult("Windows Update", "WARN", "Query failed");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return;
    }

    if (status.dwCurrentState == SERVICE_RUNNING) {
        Helper::printConcern("- Windows Update service is running (recommend disabling after installing updates)");
        Helper::recordResult("Windows Update", "WARN", "Running - recommend disabling");
    }
    else {
        Helper::printSuccess("- Windows Update is disabled", false);
        Helper::recordResult("Windows Update", "OK", "Disabled/stopped");
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
}

// v1.2.1: CPUID + firmware-feature based virtualization check (no WMIC dependency)
void Checks::checkCPUVirtualization()
{
    SetConsoleTitleA("Checking CPU Virtualization");

    // CPUID: VT-x (Intel, leaf 1 ECX bit 5) / SVM (AMD, ext leaf 0x80000001 ECX bit 2)
    int regs[4] = { 0, 0, 0, 0 };
    bool vmx = false;
    bool svm = false;

    __cpuid(regs, 0);
    unsigned maxLeaf = static_cast<unsigned>(regs[0]);
    if (maxLeaf >= 1) {
        __cpuid(regs, 1);
        vmx = (regs[2] & (1 << 5)) != 0;
    }

    __cpuid(regs, static_cast<int>(0x80000000u));
    unsigned maxExtLeaf = static_cast<unsigned>(regs[0]);
    if (maxExtLeaf >= 0x80000001u) {
        __cpuid(regs, static_cast<int>(0x80000001u));
        svm = (regs[2] & (1 << 2)) != 0;
    }

    const bool hwVirtualization = vmx || svm;

    // PF_VIRT_FIRMWARE_ENABLED (23): firmware has hardware virtualization turned on
    const bool firmwareEnabled = IsProcessorFeaturePresent(23) != 0;
    // PF_HYPERVISOR_PRESENT (40): a hypervisor is currently running
    bool hypervisorPresent = IsProcessorFeaturePresent(40) != 0;
    if (!hypervisorPresent) {
        // Registry fallback (written by the kernel when the hypervisor boots)
        DWORD hvReg = 0;
        if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization",
            "HypervisorPresent", &hvReg)) {
            hypervisorPresent = hvReg != 0;
        }
    }

    if (!hwVirtualization) {
        Helper::printSuccess("- CPU does not support hardware virtualization", false);
        Helper::recordResult("CPU-V", "OK", "Not supported by CPU");
        return;
    }

    const std::string tech = vmx ? "VT-x" : "SVM";
    if (firmwareEnabled) {
        std::string detail = "Enabled in BIOS (" + tech + ")";
        if (hypervisorPresent) detail += ", hypervisor active";
        Helper::printConcern("- CPU-V is enabled in BIOS (" + tech + "), disable it in BIOS if not needed " +
            biosHint("cpuv"));
        Helper::recordResult("CPU-V", "WARN", detail);
    }
    else {
        std::string detail = "Disabled in BIOS (" + tech + ")";
        if (hypervisorPresent) detail += ", hypervisor active";
        Helper::printSuccess("- CPU virtualization disabled in BIOS (" + tech + ")", false);
        Helper::recordResult("CPU-V", "OK", detail);
    }
}

// v1.2.1: raw SMBIOS identity scoring + registry cross-check to spot perm HWID spoofers
void Checks::checkSpoofer()
{
    SetConsoleTitleA("Checking for HWID Spoofer");

    SmbiosIdentity id = readSmbiosIdentity();
    if (!id.valid) {
        Helper::printConcern("- Could not read SMBIOS identity");
        Helper::recordResult("HWID Spoofer", "WARN", "SMBIOS unreadable");
        return;
    }

    struct Field { const char* name; const std::string* value; bool identityCore; };
    const Field fields[] = {
        { "System Manufacturer", &id.systemManufacturer, true },
        { "System Product",      &id.systemProduct,      true },
        { "System Version",      &id.systemVersion,      false },
        { "System Serial",       &id.systemSerial,       false },
        { "Board Manufacturer",  &id.boardManufacturer,  true },
        { "Board Product",       &id.boardProduct,       true },
        { "Board Version",       &id.boardVersion,       false },
        { "Board Serial",        &id.boardSerial,        false },
    };

    int strongHits = 0;
    int weakHits = 0;
    std::string strongList;
    std::string weakList;

    auto addHit = [](std::string& list, const std::string& entry) {
        if (!list.empty()) list += "; ";
        list += entry;
    };

    for (const Field& field : fields) {
        bool wasStrong = false;
        const int score = identityScore(*field.value, &wasStrong);
        if (score == 2) {
            // Hex/synthetic values are suspicious in ANY field
            strongHits++;
            addHit(strongList, std::string(field.name) + "=" + *field.value);
        }
        else if (score == 1 && field.identityCore) {
            // Placeholders/random tokens only count in manufacturer/product:
            // "To be filled by O.E.M." in serial/version is normal OEM sloppiness
            weakHits++;
            addHit(weakList, std::string(field.name) + "=" + *field.value);
        }
    }

    if (id.uuidNil) {
        weakHits++;
        addHit(weakList, "SMBIOS UUID is empty");
    }

    // Odd SMBIOS version (real firmware reports 2.x or 3.x)
    if (id.smbiosMajor < 2 || id.smbiosMajor > 3) {
        weakHits++;
        addHit(weakList, "SMBIOS version " + std::to_string(id.smbiosMajor) + "." +
            std::to_string(id.smbiosMinor));
    }

    // Cross-check against the registry copy written at boot: a spoofer that
    // patches the SMBIOS view but not this key (or vice versa) shows up here
    struct RegPair { const char* regName; const std::string* smbiosValue; };
    const RegPair regPairs[] = {
        { "SystemManufacturer",     &id.systemManufacturer },
        { "SystemProductName",      &id.systemProduct },
        { "BaseBoardManufacturer",  &id.boardManufacturer },
        { "BaseBoardProduct",       &id.boardProduct },
    };
    for (const RegPair& pair : regPairs) {
        const std::string regValue = readRegistryStringValue(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\BIOS", pair.regName);
        if (identityDiffers(*pair.smbiosValue, regValue)) {
            strongHits++;
            addHit(strongList, std::string(pair.regName) + " differs (SMBIOS=" +
                *pair.smbiosValue + ", registry=" + regValue + ")");
        }
    }

    if (strongHits >= 2) {
        Helper::printError("- Likely HWID spoofer detected (" + std::to_string(strongHits) +
            " strong indicators): " + strongList);
        Helper::recordResult("HWID Spoofer", "FAIL", strongList);
    }
    else if (strongHits == 1) {
        Helper::printConcern("- Suspicious identity value: " + strongList);
        Helper::recordResult("HWID Spoofer", "WARN", strongList);
    }
    else if (weakHits >= 2) {
        Helper::printConcern("- Possible identity tampering: " + weakList);
        Helper::recordResult("HWID Spoofer", "WARN", weakList);
    }
    else {
        Helper::printSuccess("- No spoofer indicators (SMBIOS + registry consistent)", false);
        Helper::recordResult("HWID Spoofer", "OK", "Identity looks consistent");
    }
}

// v1.2.2: read-only activation status - reports the state, never activates or
// modifies anything (no key installation, no watermark tampering)
void Checks::checkActivation()
{
    SetConsoleTitleA("Checking Activation Status");

    std::string command =
        "powershell -NoProfile -NonInteractive -Command \""
        "$p = Get-CimInstance -ClassName SoftwareLicensingProduct | "
        "Where-Object { $_.PartialProductKey -and $_.Name -like 'Windows*' } | Select-Object -First 1; "
        "if ($p) { $p.LicenseStatus } else { 'NA' }\" 2>nul";
    std::string out = trimCopy(Helper::runSystemCommandWithOutput(command.c_str(), 30000));
    if (out == "TIMEOUT") out.clear();

    // first token: a LicenseStatus digit, "NA", or PowerShell noise on failure
    std::string token = out.substr(0, out.find_first_of("\r\n \t"));
    bool numeric = !token.empty() &&
        token.find_first_not_of("0123456789") == std::string::npos;

    if (token.empty() || token == "NA" || !numeric) {
        Helper::printConcern("- Could not read the Windows activation status");
        Helper::recordResult("Activation", "WARN",
            token.empty() ? "Status unreadable" :
            (token == "NA" ? "No Windows product key found" : "Unexpected PowerShell output"));
        return;
    }

    // SoftwareLicensingProduct.LicenseStatus
    switch (atoi(token.c_str())) {
    case 1:
        Helper::printSuccess("- Windows is activated (licensed)", false);
        Helper::recordResult("Activation", "OK", "Activated (licensed)");
        break;
    case 2:
        Helper::printSuccess("- Windows is activated (out-of-box grace period)", false);
        Helper::recordResult("Activation", "OK", "Activated (OOB grace)");
        break;
    case 3:
        Helper::printSuccess("- Windows is activated (out-of-tolerance grace period)", false);
        Helper::recordResult("Activation", "OK", "Activated (OOT grace)");
        break;
    case 0:
        Helper::printConcern("- Windows is not activated - activate with a valid license key "
            "(Settings > System > Activation)");
        Helper::recordResult("Activation", "WARN",
            "Not activated - activate with a valid license key");
        break;
    case 4:
        Helper::printConcern("- Windows reports a non-genuine grace period");
        Helper::recordResult("Activation", "WARN", "Non-genuine grace period");
        break;
    case 5:
        Helper::printConcern("- Windows is in notification mode - the activation watermark "
            "can be visible until a valid license key is entered");
        Helper::recordResult("Activation", "WARN",
            "Notification mode - activation watermark can be visible");
        break;
    case 6:
        Helper::printConcern("- Windows is in extended grace period");
        Helper::recordResult("Activation", "WARN", "Extended grace period");
        break;
    default:
        Helper::printConcern("- Unknown activation state (LicenseStatus=" + token + ")");
        Helper::recordResult("Activation", "WARN", "Unknown LicenseStatus " + token);
        break;
    }
}

void Helper::setupConsole()
{
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, mode & ~(ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS));

    // v1.2.2: enable ANSI/VT color sequences explicitly - consoles that never
    // switch them on print raw escape codes like "[38;2;255;255;255m"
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD outMode = 0;
    if (hStdout != NULL && hStdout != INVALID_HANDLE_VALUE &&
        GetConsoleMode(hStdout, &outMode)) {
        SetConsoleMode(hStdout, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    SetConsoleTitleA("Initializing");

    // v1.2.1: start maximized so every check line is visible without scrolling
    HWND console = GetConsoleWindow();
    if (console != NULL) {
        ShowWindow(console, SW_MAXIMIZE);
    }
}

void Helper::printSuccess(const std::string& message, bool changed)
{
    if (cliConfig.quiet) return;
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::Green);
    std::cout << "[+] ";
    Color::setForegroundColor(Color::White);
    std::cout << message;
    if (changed) { Color::setForegroundColor(Color::Yellow); std::cout << " (CHANGED)"; }
    std::cout << std::endl;
}

void Helper::printConcern(const std::string& message)
{
    if (cliConfig.quiet) return;
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::Yellow);
    std::cout << "[-] ";
    Color::setForegroundColor(Color::White);
    std::cout << message << std::endl;
}

void Helper::printError(const std::string& message)
{
    if (cliConfig.quiet) return;
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::Red);
    std::cout << "[X] ";
    Color::setForegroundColor(Color::White);
    std::cout << message << std::endl;
}

void Helper::runSystemCommand(const char* command)
{
    std::string modifiedCommand = command;
    if (modifiedCommand.find("2>nul") == std::string::npos &&
        modifiedCommand.find("2>&1") == std::string::npos) {
        modifiedCommand += " 2>nul";
    }
    FILE* stream = _popen(modifiedCommand.c_str(), "r");
    if (stream == NULL) return;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), stream) != NULL) {}
    _pclose(stream);
}

int Helper::runSystemCommandExitCode(const char* command)
{
    if (command == NULL) return -1;
    std::string modifiedCommand = command;
    if (modifiedCommand.find("2>nul") == std::string::npos &&
        modifiedCommand.find("2>&1") == std::string::npos) {
        modifiedCommand += " 2>nul";
    }
    FILE* stream = _popen(modifiedCommand.c_str(), "r");
    if (stream == NULL) return -1;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), stream) != NULL) {}
    return _pclose(stream);
}

bool Helper::readDwordValueRegistry(HKEY hKeyParent, LPCSTR subkey, LPCSTR valueName, DWORD* readData) {
    HKEY hKey;
    LONG ret = RegOpenKeyEx(hKeyParent, subkey, 0, KEY_READ, &hKey);
    if (ret == ERROR_SUCCESS) {
        DWORD data;
        DWORD len = sizeof(DWORD);
        ret = RegQueryValueEx(hKey, valueName, NULL, NULL, reinterpret_cast<LPBYTE>(&data), &len);
        RegCloseKey(hKey);
        if (ret == ERROR_SUCCESS) {
            (*readData) = data;
            return true;
        }
        return false;
    }
    return false;
}

void Color::setBackgroundColor(const RGBColor& color) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0; GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING; SetConsoleMode(hOut, dwMode);
    printf(("\x1b[48;2;" + std::to_string(color.r) + ";" + std::to_string(color.g) + ";" + std::to_string(color.b) + "m").c_str());
}

void Color::setForegroundColor(const RGBColor& color) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0; GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING; SetConsoleMode(hOut, dwMode);
    printf(("\x1b[38;2;" + std::to_string(color.r) + ";" + std::to_string(color.g) + ";" + std::to_string(color.b) + "m").c_str());
}

