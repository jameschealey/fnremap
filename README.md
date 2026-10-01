# FnRemap

Remap the Fn key on Apple Magic Keyboard (USB-C) to Left Control on Windows.

The Apple Fn key is a vendor-specific HID usage (page 0x00FF, usage 0x0003) that is invisible to standard Windows remapping tools like PowerToys, SharpKeys, or AutoHotkey. This project works around that by replacing the HID driver on the keyboard's vendor-specific interface with WinUSB, reading raw USB reports, and injecting keyboard events via SendInput.

## How it works

1. **WinUSB driver** (`fnremap.inf`) replaces the default HID driver on the keyboard's second USB interface (MI_01), which carries the vendor-specific Fn key data.
2. **fnremap.exe** reads raw 10-byte HID reports from the USB interrupt endpoint, detects the Fn bit in the Apple vendor byte, and injects Left Control via SendInput.
3. A **Windows service** launches fnremap.exe in the active user session at boot, with automatic reconnection on keyboard unplug/replug.

## Requirements

- Apple Magic Keyboard with USB-C (VID 05AC, PID 0322)
- Windows 10/11 (x64)
- [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022) (MSVC compiler)
- [Windows Driver Kit (WDK)](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk) (for inf2cat and signtool)

## Build

Open a "Developer Command Prompt for VS" or run vcvarsall.bat, then:

```
cl /nologo /W4 /Ox fnremap.c /link winusb.lib setupapi.lib user32.lib wtsapi32.lib userenv.lib advapi32.lib
```

## Install

Run in an elevated PowerShell:

```powershell
Set-ExecutionPolicy Bypass -Scope Process -Force
.\fnremap_install.ps1
```

The installer will:
1. Create a self-signed certificate for driver signing
2. Generate and sign the driver catalog
3. Install the WinUSB driver for the keyboard
4. Install fnremap.exe to `C:\Program Files\FnRemap\`
5. Create and start the FnRemap Windows service

Unplug and replug the keyboard after installation if it's not working immediately.

## Uninstall

```powershell
Set-ExecutionPolicy Bypass -Scope Process -Force
.\fnremap_uninstall.ps1
```

Unplug and replug the keyboard afterward to restore normal operation. If it doesn't respond, restart Windows.

## Important caveats

- **The keyboard only works through fnremap.exe while the WinUSB driver is installed.** The WinUSB driver replaces the standard HID driver on the keyboard interface, so if the service is stopped, the keyboard will stop working until the service is restarted or the driver is uninstalled.
- **Pre-login screens** (BIOS/UEFI, BitLocker, Windows login): the keyboard works normally at these stages because the WinUSB driver hasn't loaded yet. After Windows login, the service takes over.
- **Other Magic Keyboard models**: this is configured for PID 0322 (USB-C model). For other models, update the PID in both `fnremap.inf` and recompile. Use Device Manager to find your keyboard's PID.

## Interactive mode

For testing without installing the service, run fnremap.exe directly. It will prompt you to press Fn three times to auto-detect the Fn bit:

```
fnremap.exe
```

Or skip detection with a known bit mask:

```
fnremap.exe --fn-bit=0x02
```

## License

MIT
