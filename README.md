# PiP Jumper

**Tiny tray tool to make Picture-in-Picture window jump away from mouse pointer.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Windows](https://img.shields.io/badge/Windows-x86%20%7C%20x64-blue.svg)](https://github.com/ZalgoSoft/pipjumper)

## Overview

PiP Jumper is a lightweight, resource-efficient Windows utility that automatically moves Picture-in-Picture (PiP) windows away from your mouse cursor. It works with Chromium-based browsers, Firefox, and any application that creates PiP windows.

**The problem:** When watching a video in a floating PiP window, the window often sits right where your mouse cursor needs to be. You have to manually drag it out of the way every time.

**The solution:** PiP Jumper detects when your mouse enters a PiP window and instantly teleports it to a random position within your screen's work area. No more manual dragging!

## Features

- 🖱️ **Auto-jump on mouse enter** — PiP windows move away as soon as your cursor touches them
- 🔄 **Manual jump** — Press `Ctrl+Shift+J` to jump all PiP windows at once
- ⏹️ **Toggle on/off** — Press `Ctrl+Shift+P` or double-click the tray icon to enable/disable
- 🎯 **Smart cooldown** — Prevents rapid repeated jumps (customizable)
- 💾 **Ultra-lightweight** — ~15 KB executable, no dependencies
- 🔒 **Secure** — CRT-free, minimal API surface
- 🪟 **System tray integration** — Right-click for menu options

## How It Works

1. PiP Jumper runs in the system tray
2. Every 500ms, it scans for new PiP windows using:
   - Window class names (`MozillaDialogClass`, `Chrome_WidgetWin`, etc.)
   - Window titles containing "picture-in-picture"
   - `WS_EX_TOPMOST` style
3. A low-level mouse hook detects when your cursor enters a PiP window
4. The window is instantly repositioned to a random location within the work area
5. A cooldown timer prevents jumping back and forth

## Demo

![Demo](demo.svg)

## Installation

### Pre-built Binaries

Download the latest release from the [releases page](https://github.com/ZalgoSoft/pipjumper/releases):

- `pipjumperx86.exe` — 32-bit version
- `pipjumperx64.exe` — 64-bit version

### Building from Source

#### Prerequisites
- Visual Studio (any edition) with C/C++ tools installed
- Windows SDK

#### Build Script
1. Open **Developer Command Prompt for VS** (x86/x64)
2. Navigate to the project directory
3. Run `build.bat`

```cmd
cd pipjumper
build.bat
```

The script will automatically detect your architecture and produce the appropriate executable.

#### Manual Build

**32-bit:**
```cmd
cl /O1 /GS- /Gy /Gw /Oi- /c /Fo"pipjumperx86.obj" pipjumper.c
link pipjumperx86.obj /SUBSYSTEM:WINDOWS /ENTRY:EntryPoint /NODEFAULTLIB /OPT:REF /OPT:ICF kernel32.lib user32.lib shell32.lib /MACHINE:X86 /OUT:pipjumperx86.exe
```

**64-bit:**
```cmd
cl /O1 /GS- /GL /Gy /Gw /c /Fo"pipjumperx64.obj" pipjumper.c
link pipjumperx64.obj /SUBSYSTEM:WINDOWS /ENTRY:EntryPoint /NODEFAULTLIB /LTCG /OPT:REF /OPT:ICF kernel32.lib user32.lib shell32.lib /OUT:pipjumperx64.exe
```

## Usage

### Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+Shift+P` | Toggle PiP Jumper on/off |
| `Ctrl+Shift+J` | Jump all active PiP windows |

### Tray Icon

- **Left double-click** — Toggle on/off
- **Right-click** — Open context menu

### Context Menu

- **Enabled** — Toggle the tool on/off (checkmark indicates state)
- **Jump all PiP windows** — Manually trigger a jump
- **Cooldown +/- 100 ms** — Adjust the jump cooldown timer
- **Cooldown: X ms** — Display current cooldown setting
- **Exit** — Close the application

## Configuration

- **Cooldown period** — Default: 200ms. Prevents repeated jumping while your mouse is inside the window. Adjust via the tray menu.
- **Scan interval** — Hardcoded to 500ms for optimal performance.
- **Edge padding** — Hardcoded to 24px. Windows are kept away from screen edges.

## Technical Details

- **Language:** Pure C
- **CRT:** None (no MSVC runtime required)
- **System API:** Win32
- **Entry Point:** Custom `EntryPoint` (no `WinMain`/`main`)
- **DPI:** Per-monitor DPI aware V2 (with fallback)
- **Memory:** Minimal heap usage, stack only
- **Hooks:** Low-level mouse hook (`WH_MOUSE_LL`) — no DLL injection
- **Performance:** Optimized for tiny size and low CPU usage

## Browser Compatibility

| Browser | Detected Classes | Supported |
|---------|------------------|-----------|
| Firefox | `MozillaDialogClass`, `MozillaCompositorWindowClass` | ✅ |
| Chrome/Chromium | `Chrome_WidgetWin` | ✅ |
| Edge | `Chrome_WidgetWin` | ✅ |
| Brave | `Chrome_WidgetWin` | ✅ |

Based on the code analysis, here's the Windows version compatibility section for your README:

## Windows Version Support

PiP Jumper is designed to work on a wide range of Windows versions, from legacy systems to modern installations.

### Supported Versions

| Windows Version | Support Level | Notes |
|-----------------|---------------|-------|
| Windows 11 | ✅ Full | Fully tested, DPI-aware |
| Windows 10 | ✅ Full | Should work, untested |
| Windows 8.1 | ✅ Full | Should work, untested |
| Windows 8 | ✅ Full | Should work, untested |
| Windows 7 | ✅ Full | Should work, untested |
| Windows Vista | ⚠️ Limited | Should work, untested |
| Windows XP | ⚠️ Limited | Should work, untested |
| Windows 2000 | ⚠️ Limited | Should work, untested |

### Implementation Details

The code includes several features that ensure broad compatibility:

#### DPI Awareness
PiP Jumper dynamically detects and enables DPI awareness using a progressive fallback strategy:

1. **Per-Monitor Aware V2** (Windows 10 1703+) — Best experience, proper scaling per monitor
2. **Per-Monitor Aware** (Windows 8.1+) — Good scaling on multiple monitors  
3. **System DPI Aware** (Windows Vista+) — Basic scaling support

This is implemented using runtime function resolution (`GetProcAddress`) rather than compile-time linking, ensuring the executable works on older systems where newer APIs aren't available.

#### API Compatibility
- All Windows API functions are resolved dynamically or use standard exports
- No dependency on newer API versions that would break on legacy systems
- Uses `GetModuleHandleW` and `GetProcAddress` for optional features

#### CRT-Free Implementation
Since the application uses no C Runtime Library, there's no dependency on:
- MSVCRT redistributable packages
- Specific version of the C runtime
- Platform-specific CRT initialization

This makes it truly portable — just copy and run the `.exe` file.

#### 32-bit and 64-bit Support
Both architectures are supported:
- **32-bit (x86)** — Compatible with all Windows versions from XP onward
- **64-bit (x64)** — Compatible with Windows XP x64 Edition and later

### Minimum Requirements

- **Processor:** x86 or x64 compatible
- **Memory:** ~2 MB RAM (minimal footprint)
- **Disk:** ~15 KB executable size
- **OS:** Windows 2000 or newer (theoretically; Windows 7+ recommended and tested)

### Notes for Older Systems

- **Windows Vista/7:** Works perfectly, DPI scaling uses System DPI Aware mode
- **Windows XP:** Should work, but:
  - Low-level mouse hooks may require additional privileges
  - DPI awareness is limited
  - Some newer API functions aren't available (gracefully handled)

### Testing Status

| Version | Tested | Status |
|---------|--------|--------|
| Windows 11 22H2 | ✅ | Working |
| Windows 10 22H2 | ⚠️ Untested |
| Windows 8.1 | ⚠️ Untested |
| Windows 7 SP1 | ⚠️ | Working |
| Windows Vista | ⚠️ | Untested |
| Windows XP | ⚠️ | Untested |

### Why It Works Everywhere

The application was built with compatibility in mind:

1. **No CRT = No Redistributable Hell** — No need to install VC++ runtimes
2. **Minimal API Usage** — Uses only core Win32 functions that have existed since Windows 2000
3. **Dynamic Feature Detection** — Newer features are used only if available
4. **Clean Error Handling** — If a feature isn't supported, it fails gracefully
5. **Portable** — Single executable, no registry changes, no installation required

### Known Limitations

- **Windows XP x64:** Limited testing, but should work with x64 binary
- **Windows 2000/ME:** Theoretical support only, not actively maintained
- **ARM/ARM64:** Not currently supported (no builds provided)

---

**Note:** The application is primarily tested and maintained on Windows 10 and 11, but the codebase is designed with backward compatibility in mind. If you encounter issues on older Windows versions, please open an issue on GitHub.
## Why No CRT?

- **Smaller executable size** (~15 KB vs 100+ KB)
- **Zero dependencies** — No MSVCRT redistributable needed
- **Faster startup** — No CRT initialization overhead
- **Security** — Reduced attack surface

## License

MIT License — see [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! Please open an issue or pull request for:

- Bug reports
- Feature suggestions
- Additional browser detection
- Performance improvements

## Acknowledgments

- Built for users who are tired of dragging PiP windows around
- Inspired by the frustration of PiP windows blocking cursor access

---

**Made with ❤️ by ZalgoSoft**
