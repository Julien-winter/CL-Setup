# CL-Setup

**Original Creator:** coby69 (Coby)  
**Maintained by:** Julien-winter

This code is released under the GNU General Public License (GPL).
See http://www.gnu.org/licenses/gpl-3.0.txt for more information.

The purpose of this program is to help the user setup the CL Client by doing all the things that you used to have do yourself, automatically.

## Changes & Bug Fixes

### v1.2.3

**Changed**
- The shop info box that appears after the "Apply setup changes?" dialog can now be turned off **silently**: set `showShopInfo=false` in `%TEMP%\CL-Setup\settings.ini`. The file is created with `showShopInfo=true` on the first run, nothing is ever printed in the console about it (values `false` / `0` / `no` / `off` are accepted, comments start with `#` or `;`)

### v1.2.2

**Added**
- New check **28 = Activation** (strictly read-only): reports the Windows activation state via `SoftwareLicensingProduct.LicenseStatus` - it never activates, installs a key or touches the watermark
- **Mainboard detection** (ASUS / MSI / Gigabyte / ASRock / Dell / HP / Lenovo / Acer / Medion / ...) with a short BIOS path hint **in the same line** whenever something has to be disabled in the firmware, e.g. `TPM is enabled ... [ASUS: Advanced > fTPM Config > Off]` (also for Secure Boot and CPU virtualization)
- Network adapter lines now show *why* an adapter is kept or removed (`[virtual]` / `[hardware]` / `[loopback]`), and every pending removal lists the PnP device instance + classification reason

**Fixed**
- Unused virtual adapters stayed behind (`LAN-Verbindung* 13/14`, `Microsoft KM-TEST`, Tailscale/Wintun leftovers): the marker contained a typo (`microsoft km--test` never matched) and adapters were judged by name only. Adapters are now classified by friendly-name markers **and** by the PnP device-instance bus - devices on `PCI\`, `USB\`, `NVME\`... are hardware and are never touched, while `ROOT\`, `SWD\`, `VMBUS\`... leftovers are actually removed (unknown devices are conservatively kept)
- Adapters without a PnP device instance (e.g. `eth0` in a VM) are no longer proposed for removal - `pnputil /remove-device` can never succeed without one
- Final summary undercounted checks (`17 passed`): results are no longer wiped before the apply phase and each check's result is updated in place, so all checks from both phases are counted
- Core Isolation: when HVCI was pending approval the VBS change was never added to the pending list (early `return`) - both pendings are now registered, and the check records exactly one combined result
- Security Mitigations: a pending hypervisor change returned early *without* recording a result and skipped registering the Hyper-V feature pendings
- Console colors are now switched on explicitly (VT processing) - no more raw `[38;2;255;255;255m` escape codes on consoles that do not enable them by default
- `--skip` / `--only` help text now covers checks 1-28

### v1.2.1

**Added**
- Console window is maximized automatically on start
- Auto-uninstall of **unused virtual** network adapters only (never physical NICs), with confirmation before removal; removed adapters are logged to `%LOCALAPPDATA%\CL-Setup\removed-adapters.log`
- Undo file: `Undo-CL-Setup.ps1` / `Undo-CL-Setup.bat` reverts everything the tool changed (registry backups, bcdedit, Hyper-V features, services, adapters)
- Every applied change is recorded in `%LOCALAPPDATA%\CL-Setup\applied-changes.log`
- New checks: **CPU virtualization** (CPUID VT-x/SVM + firmware + hypervisor state) and **HWID/spoofer detection** (raw SMBIOS identity scoring: hex/random tokens, placeholder strings, synthetic board patterns, SMBIOS-vs-registry mismatch)
- Shop info popup right after the "Apply setup changes?" answer

**Fixed**
- Winver: newest builds (26200+, unknown future builds) are no longer marked as supported - they now show `Winver "..." is a 50/50, if error contact support`
- TPM / Secure Boot / DMA Protection detection rewritten (firmware type, `Win32_DeviceGuard` fallback, proper status handling)
- WMIC removed - the 3rd-party AV check now uses PowerShell/CIM (`Get-CimInstance`) with a registry fallback, so no more WMIC errors on newer Windows
- Bogus "Update available" dialog fixed - only real, newer version tags are shown (stale/poisoned cache ignored)
- `reg export` failed with "Ungültiger Schlüsselname" because the hive prefix (`HKLM\`) was missing - registry backups now work
- Checks 6 (Chrome) and 20 (Network Adapters) were missing from the apply phase and never executed their approved changes

### v1.2.0

- Added async VCRedist installation (non-blocking thread)
- Added additional checks: GameBar, Modified OS detection, TPM status, Core Isolation (HVCI), DMA Protection
- Improved Winver check with Windows 11 23H2 / 24H2 support
- Improved Windows Defender check (service-based instead of registry)
- Better error handling with detailed error codes
- Improved build settings: C++20, static linking, admin rights
- Various bug fixes and stability improvements

## Undo

Run `Undo-CL-Setup.bat` (as admin) to revert the changes made by CL Setup.
It restores the registry backups, resets `hypervisorlaunchtype`, re-enables the
Hyper-V features, re-enables the Defender/Time services and re-scans removed
network adapters. A reboot is recommended afterwards.

## Build

Open `CL Setup.sln` in Visual Studio 2022 and build for Release|x64 or Release|Win32.
