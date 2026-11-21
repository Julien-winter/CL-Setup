# CL-Setup

**Original Creator:** coby69 (Coby)  
**Maintained by:** Julien-winter

This code is released under the GNU General Public License (GPL).
See http://www.gnu.org/licenses/gpl-3.0.txt for more information.

The purpose of this program is to help the user setup the CL Client by doing all the things that you used to have do yourself, automatically.

## Changes & Bug Fixes

### v1.2.4

**Added**
- **Kick warning on flagged network adapters**: every adapter classified as VPN/virtual now gets an orange ` (can cause game kicks)` suffix on its console line (e.g. `NordVPN - USED (Up) [virtual] (can cause game kicks)`) - the summary/JSON entry carries the same text, and `--quiet` still shows the warning by itself

**Changed**
- **Winver 26H2 is reported as "(not supported)"**: build 26300 now reads `- Winver "Win11 26H2" (not supported)` (still WARN) instead of the generic 50/50 wording - 25H2 and unknown future builds keep the 50/50 text

### v1.2.3

**Added**
- **VM detection extended**: now also recognizes Hyper-V guests (Guest\Parameters key) and firmware identifiers in the BIOS registry block (QEMU/KVM, Bochs, Xen, Parallels, bhyve, Nitro, VirtualBox/VMware strings) - previously only VBox/VMware services were found
- **HWID spoofer check extended**: SMBIOS Type 0 (BIOS vendor/version) is now parsed and cross-checked against the registry copy (weak signal - some OEMs format it differently), and a repeated-byte UUID (`11111111-...`) counts as a strong indicator
- **Activation check hardened** (28): silent when the status cannot be read (the tool never activates anything anyway), the product is located via the fixed Windows `ApplicationId` GUID so it works on every OS language, and the OK states no longer print a console line - only problems (not activated, grace, watermark) show
- **Winver knows the current releases**: build 26300 is reported as `Win11 26H2` and 28000 as `Win11 26H1` instead of a bare `Build 26300` (the 50/50 verdict for unknown/new builds stays as it was)
- **DMA state read directly** (17): the check now asks `NtQuerySystemInformation(SystemDmaGuardPolicyInformation)` - the query behind the msinfo32 "Kernel DMA Protection" field - before falling back to the WMI support flag. Enabled is reported as FAIL with the real state instead of the vague "supported by firmware, state unknown" WARN; the old wording only remains when the query cannot be read
- **Exploit Protection OK stays off the console**: when the override lock is already set (check 12), no line is printed anymore - "locked by policy" was screenshot noise nobody acts on; log and JSON still record the result, and pending/failure cases still print

**Fixed**
- **Duplicated console lines** (e.g. `- Fast-Boot is disabled` printed back to back): when a second CL-Setup instance ended up writing into the *same* console its output interleaved with the running one - the in-process dedup cannot see lines from another process. The tool now takes a single-instance lock per session: a second start prints a short "already running" notice and exits instead of mixing lines into the check
- **Commands could hang forever**: the timeout in `runSystemCommandWithOutput` never fired (the `std::async` future destructor waited for the child process) and `runSystemCommand`/`runSystemCommandExitCode` had no timeout at all - a hung `powershell`/`dism`/`pnputil`/installer froze the tool *before* the screenshot hint. All command execution now runs through a bounded CreateProcess runner with a real deadline (10 min default) that kills the whole process **tree** (job object); `TIMEOUT` results now actually appear, and `start https://...` targets can no longer stall the caller by inheriting the output pipe
- **Service-state comparisons were all off by one** (HIGH): the `ServiceStatus` enum started at 0 but Windows reports `SERVICE_STOPPED=1`..`PAUSED=7` - a running AV service never equalled `STATUS_SERVICE_RUNNING` (the AV tamper check was toothless), W32Time was never started in the disabled branch, and one `!= RUNNING` test was always true. The enum now carries the explicit winnt.h values
- **Chrome Safe Browsing policy inverted** (HIGH): `SafeBrowsingProtectionLevel` was checked/written as `1 = off`, but the Chrome policy is `0 = NoProtection, 1 = Standard (ON), 2 = Enhanced` - the tool reported "already disabled" while Safe Browsing kept running, and wrote `1` (still on) when applying. Now reads and writes `0`
- **Data race on the results list**: the summary counters and the JSON export iterated `g_results` without the mutex while the (detached) VCRedist thread could still modify it → snapshot under lock everywhere; the VCRedist thread is now **joined** instead of detached when it finished in time
- **Headless runs blocked on the interactive Vanguard uninstall dialog** (`_P_WAIT`) - now detached in headless mode
- **`--quiet` showed neither errors nor warnings** (the help text promises "only show errors and warnings"): `printConcern`/`printError` are no longer suppressed
- **Skipped checks flooded the console** (up to 28 `- Skipped:` lines): now silent, counted once in the summary as `~ N skipped`
- **Summary lost the actual findings**: the console has scrolled past them by then - the end summary now lists every remaining **WARN/FAIL by name with its message** plus the **log file path**, so one screenshot (or the log) is enough for support
- **Pending list was printed three times** (discovery lines + list block + dialog): the redundant console block is gone
- **Console noise removed**: no more "No new updates" line, and the UAC / Windows Update results go only to log/JSON (status `INFO`, not counted as warnings)
- **Shop info box disappeared when there was nothing to apply**: the box was only shown after the apply dialog, which no longer appears without pending changes (the Time-Sync false pending was fixed) - it now shows directly in the no-pending path too
- **System Restore Point removed**: the apply phase no longer creates a Windows restore point - on the target machines it never showed up in System Restore and it duplicated what the undo already does. Rollback is fully covered by the per-key exports in `%TEMP%\CL-Setup\Backup` (every registry write is exported **before** it happens) plus the markers in `applied-changes.log` that `Undo-CL-Setup.ps1` reverts; old `restorepoint:created` markers are still recognized
- **Undo: `DeviceGuard\Locked` never removed** - `Remove-ToolValues` searched the value name in the *entire* .reg export (including subkeys) and found `Locked` from `Scenarios\...`; it now searches only inside the exact key+subkey section
- **Undo: original W32Time start type was lost** (marker said `=auto`, Undo forced `demand`) - the marker now carries `service:W32Time=was<type>` (e.g. `was4`=disabled) and Undo restores exactly that (old markers still handled)
- **Undo: never imports a fallback backup newer than the recorded pre-change one** (a later run's backup would be imported as "original state")
- **Undo: adapter restore is now verified** - after `pnputil /scan-devices` each adapter is checked with `Get-PnpDevice` and reported as `[+] back` / `[!] still missing`
- **`checkWinver` crashed on an unexpected `CurrentBuild` value** (`std::stoi` exception killed the whole run) - validated digits + `atoi`, buffer zero-initialized and force-terminated; same hardening for the Vanguard `DisplayName` buffer
- **Uninitialized path buffers** when `GetTempPathW`/`GetEnvironmentVariableA` fail (undefined behaviour, logs landing in `CWD`): buffers initialized, return values checked, fallback to `C:\Windows\Temp`
- **Windows Defender reported success without checking the registry writes**: every `RegCreateKeyEx`/`RegSetValueEx` is now verified; failure = `FAIL` instead of a false "disabled (changed)"
- **Defender blocked by Tamper Protection vanished from summary/JSON** (early `return` without recording) - now recorded as `SKIPPED`
- **Exit code 0 with "0 passed"** when nothing ran (e.g. `--only 99`): now exit code `3`
- **CLI parsing**: check numbers validated to 1-28, unknown arguments and missing values reported, `--only` precedence over `--skip` made explicit instead of silently dropping
- **Invalid JSON on control characters**: remaining C0 bytes are now `\u`-escaped in the export
- **Pending/DISM state bug**: the PowerShell feature-state result was lowercased but compared against `"TIMEOUT"` - the timeout case could never match (dead condition)
- **Update-check User-Agent** hardcoded `CL-Setup/2.0` - now uses the real version
- **Console section without a heading** (checks 19-28 group) - added "Network, update and hardening checks"
- **UAC false warning**: the "Never notify" slider keeps `EnableLUA=1` and only sets `ConsentPromptBehaviorAdmin=0` - the check read `EnableLUA` alone and reported "UAC is enabled" on machines where UAC was already off. Both values are checked now
- **Windows Update warning on every machine**: the check judged by "is the service running right now" (the Windows default is a manual/trigger-start service that runs anyway). It now judges by the **start type**: `automatic` = warning, `manual`/`disabled` = OK - language-independent registry read
- **Time Sync always asked for a change**: a pending "Repair and start Windows Time service" was raised whenever the service was not running *at that moment* (default manual start = every run). Now: only a **disabled** service needs approval; manual/auto-but-stopped services are started silently, and a failed resync is reported as a warning without creating a pending change. `w32tm /resync` success is checked via the exit code (was ignored before)
- **Internet check was language- and firewall-dependent**: it parsed the localized ping output for `TTL=`. It now uses the ping **exit code** and falls back to an HTTP probe (`msftconnecttest.com`, 5-8s timeouts) when ICMP is blocked by a firewall - blocked ping no longer means "no internet"
- **Windows feature state (DISM) was text-matched** in English/German only: now read from `Get-WindowsOptionalFeature.State` (a fixed enum, works in every OS language), with the old localized DISM text matching as fallback
- **bcdedit hypervisor check**: only the `hypervisorlaunchtype` line is evaluated now - searching the whole output for `off` could false-match other entries; the German value `aus` is accepted too
- **3rd party AV filter** now also ignores the newer `Microsoft Defender Antivirus` display name (was only filtering `Windows Defender`)
- **CPU-V false positive**: the firmware-virtualization probe called `IsProcessorFeaturePresent(23)` (`PF_FASTFAIL_AVAILABLE`, virtually always true) and `IsProcessorFeaturePresent(40)` (`PF_AVX2_INSTRUCTIONS_AVAILABLE`) - every machine reported `CPU-V is enabled in BIOS` even with virtualization off (Task Manager showed *Disabled*). Now uses `PF_VIRT_FIRMWARE_ENABLED` (21) + the CPUID hypervisor bit (leaf 1, ECX bit 31) with the registry fallback, and treats "hypervisor running" as firmware-enabled (Windows masks the flag once Hyper-V is up)
- Duplicate output: the apply phase re-runs checks and printed every status line a second time - each console line is now printed **at most once per run**
- Apply section listed unchanged items: re-run results that did not change are no longer re-printed or re-logged (`(updated)` marker only when a result actually changed)
- Log files: filename now includes seconds - runs started in the same minute no longer append to the same file (several runs/versions used to end up in one log); apply phase writes a `=== phase 2 ===` marker
- **Exploit Protection wording wrong**: `DisallowExploitProtectionOverride=1` only *locks the UI* (users cannot change the settings) - the protections stay active. Check and apply label now say "override locked by policy" instead of "disabled"
- **Unguarded registry writes**: 5 `RegCreateKeyEx`/`RegSetValueEx` pairs (FastBoot, Exploit Protection, SmartScreen, Game Bar, Chrome policy) used the `HKEY` even when creating the key failed (undefined handle) - now checked and closed conditionally
- **Chrome Protection missed the default install path**: only `Program Files` was checked while Chrome usually lives in `%LOCALAPPDATA%` - the check skipped on typical systems
- **Game Bar false pending**: a missing `AppCaptureEnabled` value (the Windows default = capture on) was treated as "disabled" and raised a pending change on every Windows 11 install
- **`deleteSymbols` reported success on a partial delete**: only the `remove_all` count was checked (files gone, directory locked = still success) - the directory is now verified as gone
- **3rd party AV uninstall**: the list split kept the whole list as first token (with 2+ AVs only the last one was ever matched), the `UninstallString` was unquoted (paths with spaces broke), MSI products got the wrong silent switch (now `/qn`), and the exit code was ignored (now checked; failure = FAIL, 3010 = success)
- **`bcdedit /set` and `dism /disable-feature` results were assumed OK** (exit code discarded) - a failed run reported "changed to off"/"disabled Windows feature"; both now check the exit code (3010 accepted) and record FAIL on error
- **DMA Protection could never fail**: `AvailableSecurityProperties=3` means DMA protection is *supported by the firmware*, not "currently off" - now reported as WARN "supported, state unknown" (verify in msinfo32) instead of a false OK
- **Modified OS check**: any `OpenService` error (e.g. access denied) counted as "service missing" - only `ERROR_SERVICE_DOES_NOT_EXIST` does now
- **TPM WMI fallback**: the query now distinguishes *no TPM* from a *failed query* - Windows reporting no TPM instance is OK ("no TPM reported by Windows"), while a real query failure (access denied, WMI blocked, timeout) is reported as WARN "state unknown" instead of a false "disabled"
- **JSON summary**: `INFO` results (UAC, Windows Update) were not counted, so the counts never matched `results.length` - added an `info` counter
- **VCRedist "already installed" check was an OR per architecture**: only one of x64/x86 present counted as installed (x86 games failed later) - both architectures are now required per DLL, a 32-bit build disables WOW64 redirection so `System32` is really checked, and the post-install verify uses the same rule
- **VCRedist phase-1/phase-2 race**: a still-running detached phase-1 thread and the apply-phase re-run wrote to the same `C:\Windows\VC_redist.*.exe` - a run guard now serializes them (the second run waits for the first, bounded); `closeLogging`/`logWrite` take the log mutex for the whole check-write sequence
- **HWID spoofer false positive**: two *weak* hits were enough for "Possible identity tampering" - a nil SMBIOS UUID plus a model-number-like board product (`H610M-K`) triggered it on stock hardware; the threshold is now three weak indicators (strong indicators unchanged)
- **HWID summary lost its wording**: the yellow end-summary showed `HWID Spoofer: Board Serial=...` without the context that made the console line understandable - the summary now carries the same text (`Suspicious identity value: ...` / `Possible identity tampering: ...` / `N strong indicators: ...`)
- **CPU-V BIOS hint was Intel-only**: an AMD machine (SVM) was sent to `OC > CPU Features > VT-x`, a menu entry that does not exist on those boards - the hint is now tech-aware and shows the SVM Mode path on AMD boards (Intel paths unchanged)
- **VM check leaked the `VBoxService` handle** (never closed, result ignored)
- **Internet check**: the HTTP fallback returned true for *any* non-empty body (captive portals, error pages counted as online) - only the expected `Microsoft Connect Test` body counts now

### v1.2.2

**Added**
- New check **28 = Activation** (strictly read-only): reports the Windows activation state via `SoftwareLicensingProduct.LicenseStatus` - it never activates, installs a key or touches the watermark
- **Mainboard detection** (ASUS / MSI / Gigabyte / ASRock / Dell / HP / Lenovo / Acer / Medion / ...) with a short BIOS path hint **in the same line** whenever something has to be disabled in the firmware, e.g. `TPM is enabled ... [ASUS: Advanced > fTPM Config > Off]` (also for Secure Boot and CPU virtualization)
- Network adapter lines now show *why* an adapter is kept or removed (`[virtual]` / `[hardware]` / `[loopback]`), and every pending removal lists the PnP device instance + classification reason
- The shop info box after the apply dialog can be turned off silently: set `showShopInfo=false` in `%TEMP%\CL-Setup\settings.ini` (created with `true` on the first run, nothing is printed in the console)

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
- Undo file: `Undo-CL-Setup.ps1` (right-click → Run with PowerShell, elevates itself; `Undo-CL-Setup.bat` as an alternative wrapper) reverts the recorded reversible changes (registry backups, bcdedit, Hyper-V features, services, adapters); actions that cannot be rolled back (Winsock reset, `C:\Symbols`, VCRedist install, AV/Vanguard uninstall) are listed as `info:` in its output
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

Right-click `Undo-CL-Setup.ps1` → **Run with PowerShell** (it asks for admin itself), or run `Undo-CL-Setup.bat` as admin. If Windows blocks the downloaded script, run: `powershell -NoProfile -ExecutionPolicy Bypass -File .\Undo-CL-Setup.ps1`
It restores the registry backups, resets `hypervisorlaunchtype`, re-enables the
Hyper-V features, re-enables the Defender/Time services and re-scans removed
network adapters. A reboot is recommended afterwards.

## Build

Open `CL Setup.sln` in Visual Studio 2022 and build for Release|x64 or Release|Win32.
