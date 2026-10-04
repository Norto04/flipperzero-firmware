# Portal BLE firmware fork

This fork follows official Flipper firmware `dev` and adds an exclusive BLE
central/GATT-client profile for the Skylanders mobile portal project. The
maintained branch is `portal-ble`; `dev` remains an upstream reference. The initial
base is upstream commit `2bbf7a54bae7316da858ab62f22ed8db0b4c661b` (1 October 2026).

The build selects ST's **BLE Full 1.20.0** radio stack. Ordinary peripheral profiles
remain available. Running a central app switches profiles and temporarily closes
phone/RPC/HID Bluetooth connections; exiting restores the default serial profile.
The central profile supports **one unencrypted peer, default MTU 23, writes with
response up to 20 bytes**. Pairing, simultaneous phone connections, long ATT
transfers and the console USB relay are outside this initial patch.

`applications/debug/portal_ble_test` builds a FAP under `apps/Debug/`. It targets
our portal at `D0:11:93:A4:9B:A5`; change `portal_address` in the example for another
portal. It discovers service `533e1530-3abe-f33f-cd00-594e8b0a8ea3`, notify `1542`
and write `1543`, discovers the notify characteristic's **actual CCCD**, writes
solid blue (`43 00 00 FF` padded to 20 bytes), then sends a padded `S` once per
second after successful write completion. The observed portal's notify value is
`0x000e` and CCCD `0x0010`; assuming `value_handle + 1` would write to `0x000f`.
Open the app, turn on the portal and inspect USB/UART device logs. Back restores
the default Bluetooth profile. Discovery/connect failure stops the test; automatic
retry and a production USB backend belong in the portal application.

Build from a normal clone with recursive submodules:

```sh
./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 copro_dist updater_package
./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 fap_dist
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined scripts/tests/portal_ble_packets.c -o /tmp/portal-ble-packets
/tmp/portal-ble-packets
# After sourcing scripts/toolchain/fbtenv.sh:
python3 scripts/portal_ble_flash_check.py
```

Use the generated **update package**, which includes the Full radio stack and
assets. A standalone application-CPU firmware image does not replace a previously
installed Light radio stack. Builds and packet checks are software validation;
the new Flipper profile and example still require hardware testing. The Pico
implementation's successful blue/keepalive tests do not validate this radio stack.
No firmware is automatically flashed by the build or CI.

Initial local validation (4 October 2026): release firmware, updater, SDK and all
FAPs built successfully; the Portal BLE Test import check passed; native packet
checks passed with address/undefined-behavior sanitizers. The Full radio image was
verified byte-for-byte in the update package. The firmware has 68,840 bytes of free
flash below that image (minimum required: 8,192).

Applications use `<profiles/central_profile.h>` and `bt_profile_start()` /
`bt_profile_restore_default()`. Callbacks run on the BLE event thread: copy data
into an application queue and issue commands from the app thread. Serialize ATT
operations, wait for `ProcedureComplete`, use discovered CCCD handles, and do not
use a profile instance after restoring Bluetooth. Stop scanning and wait for
`ScanStopped` before connecting. Cancel an outstanding connection attempt after
an application deadline. ATT timeout requires disconnect/reconnect, not another
request on the expired ATT channel. API additions are exported in both target
SDK tables without changing existing public structure layouts.

To incorporate official firmware updates, keep the published branch history:

```sh
git remote add upstream https://github.com/flipperdevices/flipperzero-firmware.git # if absent
git fetch upstream
git switch portal-ble
git merge upstream/dev
git submodule update --init --recursive
# Resolve conflicts, especially SDK versions, GAP/profile setup and radio options.
# Run the build and checks above, inspect git diff and both SDK tables, then commit.
git push origin portal-ble
```

CI runs on GitHub-hosted Ubuntu, builds the Full-stack package/SDK/apps, checks
packet boundaries and requires at least two free flash pages below the radio
stack. Official workflows that need Flipper's private runners/services are gated
to the upstream repository. When upgrading the base, recheck the radio-stack
version/layout, SDK exports, phone-profile restoration, portal connection,
solid-blue command, status notifications and a keepalive run beyond three minutes.

The implementation uses the official ST ACI interface. Moon Firmware's BLE client
work informed the initial research; this fork does not import its XIP loader,
UI, protocol-app restructuring or pairing/phone-companion subsystem.

---

<picture>
    <source media="(prefers-color-scheme: dark)" srcset="/.github/assets/dark_theme_banner.png">
    <source media="(prefers-color-scheme: light)" srcset="/.github/assets/light_theme_banner.png">
    <img
        alt="A pixel art of a Dophin with text: Flipper Zero Official Repo"
        src="/.github/assets/light_theme_banner.png">
</picture>

# Flipper Zero Firmware

- [Flipper Zero Official Website](https://flipper.net) - A simple way to explain to your friends what Flipper Zero can do.
- [Flipper Zero Firmware Update](https://flipper.net/pages/downloads) - Improvements for your dolphin: latest firmware releases, upgrade tools for PC and mobile devices.
- [User Documentation](https://docs.flipper.net/zero) - Learn more about your dolphin: specs, usage guides, and anything you want to ask.
- [Developer Documentation](https://developer.flipper.net/flipperzero/doxygen) - Dive into the Flipper Zero Firmware source code: build system, firmware structure, and more.

# Contributing

Our main goal is to build a healthy and sustainable community around Flipper, so we're open to any new ideas and contributions. We also have some rules and taboos here, so please read this page and our [Code of Conduct](/CODE_OF_CONDUCT.md) carefully.

## I need help

The best place to search for answers is our [User Documentation](https://docs.flipper.net/zero). If you can't find the answer there, check our [Discord Server](https://flipp.dev/discord). If you want to contribute to the firmware development or modify it for your own needs, you can also check our [Developer Documentation](https://developer.flipper.net/flipperzero/doxygen).

## I want to report an issue

If you've found an issue and want to report it, please check our [Issues](https://github.com/flipperdevices/flipperzero-firmware/issues) page. Make sure the description contains information about the firmware version you're using, your platform, and a clear explanation of the steps to reproduce the issue.

## I want to propose a new feature

If you have a feature request or want to vote on an existing one, please use [Discussions](https://github.com/flipperdevices/flipperzero-firmware/discussions) and follow our [Discussion Guidelines](https://github.com/flipperdevices/flipperzero-firmware/discussions/4395).

## I want to contribute code

Before opening a PR, please confirm that your changes must be contained in the firmware. Many ideas can easily be implemented as external applications and published in the [Flipper Application Catalog](https://github.com/flipperdevices/flipper-application-catalog). If you are unsure, reach out to us on the [Discord Server](https://flipp.dev/discord) or the [Issues](https://github.com/flipperdevices/flipperzero-firmware/issues) page, and we'll help you find the right place for your code.

Also, please read our [Contribution Guide](/CONTRIBUTING.md) and our [Coding Style](/CODING_STYLE.md), and make sure your code is compatible with our [Project License](/LICENSE).

Finally, open a [Pull Request](https://github.com/flipperdevices/flipperzero-firmware/pulls) and make sure that CI/CD statuses are all green.

# Development

Flipper Zero Firmware is written in C, with some bits and pieces written in C++ and armv7m assembly languages. An intermediate level of C knowledge is recommended for comfortable programming. C, C++, and armv7m assembly languages are supported for Flipper applications.

## Requirements

Supported development platforms:

- Windows 10+ with PowerShell and Git (x86_64)
- macOS 12+ with Command Line tools (x86_64, arm64)
- Ubuntu 20.04+ with build-essential and Git (x86_64)

Supported in-circuit debuggers (optional but highly recommended):

- [Flipper Zero Wi-Fi Development Board](https://flipper.net/products/wifi-devboard)
- CMSIS-DAP compatible: Raspberry Pi Debug Probe and etc...
- ST-Link (v2, v3, v3mods)
- J-Link

Flipper Build System will take care of all the other dependencies.

## Cloning source code

Make sure you have enough space and clone the source code:

```shell
git clone --recursive https://github.com/flipperdevices/flipperzero-firmware.git
```

## Building

Build firmware using Flipper Build Tool:

```shell
./fbt
```

## Flashing firmware using an in-circuit debugger

Connect your in-circuit debugger to your Flipper and flash firmware using Flipper Build Tool:

```shell
./fbt flash
```

## Flashing firmware using USB

Make sure your Flipper is on, and your firmware is functioning. Connect your Flipper with a USB cable and flash firmware using Flipper Build Tool:

```shell
./fbt flash_usb
```

## Documentation

- [Flipper Build Tool](/documentation/fbt.md) - building, flashing, and debugging Flipper software
- [Applications](/documentation/AppsOnSDCard.md), [Application Manifest](/documentation/AppManifests.md) - developing, building, deploying, and debugging Flipper applications
- [Hardware combos and Un-bricking](/documentation/KeyCombo.md) - recovering your Flipper from the most nasty situations
- [Flipper File Formats](/documentation/file_formats) - everything about how Flipper stores your data and how you can work with it
- [Universal Remotes](/documentation/UniversalRemotes.md) - contributing your infrared remote to the universal remote database
- And much more in the [Developer Documentation](https://developer.flipper.net/flipperzero/doxygen)

# Project structure

- `applications`        - Applications and services used in firmware
- `applications_users`  - Place for your additional applications and services
- `assets`              - Assets used by applications and services
- `documentation`       - Documentation generation system configs and input files
- `furi`                - Furi Core: OS-level primitives and helpers
- `lib`                 - Our and 3rd party libraries, drivers, tools and etc...
- `site_scons`          - Build system configuration and modules
- `scripts`             - Supplementary scripts and various python libraries
- `targets`             - Firmware targets: platform specific code

Also, see `ReadMe.md` files inside those directories for further details.

# Links

- Discord: [flipp.dev/discord](https://flipp.dev/discord)
- Reddit: [reddit.com/r/flipperzero](https://www.reddit.com/r/flipperzero/)
- Website: [flipper.net](https://flipper.net)
- Kickstarter: [kickstarter.com](https://www.kickstarter.com/projects/flipper-devices/flipper-zero-tamagochi-for-hackers)

## SAST Tools

- [PVS-Studio](https://pvs-studio.com/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source) - static analyzer for C, C++, C#, and Java code.
