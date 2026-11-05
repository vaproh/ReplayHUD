# ReplayHUD

[![Build](https://github.com/vaproh/ReplayHUD/actions/workflows/release.yml/badge.svg?branch=main)](https://github.com/vaproh/ReplayHUD/actions/workflows/release.yml)
[![Latest release](https://img.shields.io/github/v/release/vaproh/ReplayHUD?display_name=tag)](https://github.com/vaproh/ReplayHUD/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE.txt)

**ReplayHUD** is a lightweight native Windows companion for OBS Studio. When OBS saves a Replay Buffer clip, ReplayHUD displays a compact notification with the clip's filename. It can also report replay-buffer state changes, play a notification sound, and open a saved clip when you click its notification.

Notifications can appear on the desktop or, when a compatible game renderer is available, inside the foreground game.

![ReplayHUD app icon](assets/ReplayHUD-icon-preview.png)

## Features

- **Saved replay notifications** from OBS WebSocket v5 `ReplayBufferSaved` events.
- **Replay Buffer status** notifications when the buffer starts or stops, with duplicate state events suppressed.
- **Click to open** the last saved replay in its default Windows application.
- **System tray controls** for Settings, test notifications, the last replay, logs, and Exit.
- **Configurable notifications** with position, duration, margin, and sound settings.
- **OBS connection settings** for the local WebSocket port and optional password.
- **Automatic reconnect** after connection settings change.
- **Optional Windows startup** through a per-user startup entry.
- **In-game notifications** through Direct3D 9/10/11/12, Vulkan, and OpenGL backends, using a styled overlay with typography and visual hierarchy matched more closely to desktop notifications.
- **Single-file app distribution.** The EXE contains the x64/x86 hook DLLs and 32-bit injection helper. It extracts them to the current user's Local AppData when launched.

## Requirements

- Windows 10 or Windows 11, 64-bit.
- OBS Studio with the obs-websocket v5 server. OBS Studio 28 and newer include it.
- OBS Replay Buffer enabled and started.
- For the in-game overlay, a game using a supported graphics API that allows third-party overlays. Compatibility varies by game, renderer, driver, and display mode.

ReplayHUD connects to OBS at `127.0.0.1`. It does not need an external server or cloud account.

## Download and install

Open the [Releases](https://github.com/vaproh/ReplayHUD/releases) page. Each release provides three assets:

| Download | Contents |
|---|---|
| `ReplayHUD.exe` | Bundled 64-bit application. No adjacent hook DLL or injector is needed. |
| `ReplayHUD-Windows.zip` | Full package: the EXE, both D3D8 proxy architectures, README, and license notices. |
| `d3d8.dll` | Standalone 64-bit D3D8 compatibility proxy. The 32-bit proxy is included in the ZIP at `x86/d3d8.dll`. |

For normal use, download and run `ReplayHUD.exe`. It starts in the Windows system tray. Right-click the tray icon and select **Settings...** to configure OBS.

The application extracts its embedded game-overlay components into a build-specific directory under:

```text
%LOCALAPPDATA%\ReplayHUD\Runtime\
```

The x64 hook, x86 hook, and x86 injector are kept in architecture-specific subdirectories. The directory is versioned per build so an updated ReplayHUD does not overwrite a DLL that an older game process may still have loaded. These files are managed by ReplayHUD; users do not need to copy or launch them manually.

## Configure OBS Studio

1. Open OBS Studio.
2. Go to **Tools → WebSocket Server Settings**.
3. Enable the WebSocket server.
4. Keep the server bound to the local machine. ReplayHUD connects to `127.0.0.1`.
5. Note the server port and password. The default OBS WebSocket port is usually `4455`.
6. Enable and start **Replay Buffer**.
7. Open the ReplayHUD tray menu and choose **Settings...**.
8. Enter the same port and password used by OBS, then select **Save settings**.

If OBS WebSocket authentication is disabled, leave the password field empty. ReplayHUD reconnects after you save changed connection settings.

## Settings

Open **Settings...** from the ReplayHUD tray menu.

| Setting | Default | Details |
|---|---:|---|
| OBS WebSocket port | `4455` | Port number from OBS; valid range is 1–65535 |
| OBS WebSocket password | Empty | Optional; protected with Windows DPAPI |
| Notification sound | On | Uses the Windows `SystemAsterisk` sound |
| Notification duration | `2400 ms` | Valid range: 800–10000 ms |
| Screen edge margin | `24 px` | Valid range: 8–200 px |
| Notification position | Top right | Top right, top left, bottom right, or bottom left |
| Start with Windows | Off | Adds or removes a per-user startup entry |

### Configuration and logs

ReplayHUD stores settings and logs in:

```text
%LOCALAPPDATA%\ReplayHUD\
```

| File or directory | Contents |
|---|---|
| `ReplayHUD.ini` | Application and connection settings |
| `ReplayHUD.log` | OBS connection and application log |
| `ReplayHUDHook.log` | In-game hook diagnostics |
| `Runtime\` | Extracted, build-specific hook DLLs and x86 injector |
| `Runtime\D3D8\` | Hook extracted by the standalone D3D8 proxy |

The OBS password is protected with Windows Data Protection API (DPAPI). It is intended to be decrypted by the same Windows user on the same machine. Copying the INI file to another Windows account or computer does not make the password portable. If the saved password cannot be decrypted, enter it again in Settings.

## Using ReplayHUD

ReplayHUD runs in the system tray.

| Action | Result |
|---|---|
| **Settings...** | Configure the OBS connection and notification appearance |
| **Test notification** | Preview a notification and its sound without saving a replay |
| **Open last replay** | Open the most recently saved replay reported by OBS |
| **Open log file** | Open the application log |
| **Exit ReplayHUD** | Close the application |

ReplayHUD does not record, encode, move, rename, or edit video files. OBS remains responsible for recording and saving Replay Buffer clips.

## In-game overlay

The main application uses [Universal Dear ImGui Hook](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook) for supported graphics APIs. It detects the foreground process and selects the matching x64 or x86 runtime component. The 32-bit injector is bundled inside the EXE and is launched only when a 32-bit game needs it.

The main application publishes notification data through shared memory. The hook renders that notification in a compatible foreground game. In-game notifications use the same visual hierarchy as desktop notifications, including a colored accent, a distinct heading, readable message text, and a subdued footer. The overlay uses the same rendering path across supported backends, including OpenGL. If the in-game renderer does not become ready, ReplayHUD keeps the desktop notification as a fallback.

### Graphics API support

| Graphics API | Implementation | Notes |
|---|---|---|
| Direct3D 8 | D3D8-to-D3D9 proxy with embedded ReplayHUD hook | Requires manual, per-game proxy placement |
| Direct3D 9 | Universal Dear ImGui Hook backend | Game/driver compatibility varies |
| Direct3D 10 | Universal Dear ImGui Hook backend | Game/driver compatibility varies |
| Direct3D 11 | Universal Dear ImGui Hook backend | Game/driver compatibility varies |
| Direct3D 12 | Universal Dear ImGui Hook backend | Game/driver compatibility varies |
| Vulkan | Universal Dear ImGui Hook backend | Game/driver compatibility varies |
| OpenGL | Universal Dear ImGui Hook backend | WGL; game/driver compatibility varies |

Backend integration does not mean every game has been validated. Treat the overlay as compatibility-dependent and test it with the games and graphics settings you use.

### Direct3D 8 games

D3D8 games need a proxy DLL placed beside the game's executable. The proxy is self-contained: it embeds the architecture-matching ReplayHUD hook and extracts that hook under `%LOCALAPPDATA%\ReplayHUD\Runtime\D3D8\` when the game loads it.

1. Close the game.
2. Copy the matching `d3d8.dll` into the game's executable directory:
   - 64-bit game: use the root `d3d8.dll` from the ZIP or the standalone release download.
   - 32-bit game: use `x86/d3d8.dll` from the ZIP.
3. Start ReplayHUD, then launch the game.
4. To remove D3D8 overlay support for that game, close it and delete the `d3d8.dll` you copied into its directory.

You do **not** need to copy `ReplayHUDHook.dll` beside the game. Some older titles may behave differently under D3D8-to-D3D9 translation. A few may also need the legacy DirectX End-User Runtime for D3DX shader helpers.

For D3D9, D3D10/11/12, Vulkan, and OpenGL, ReplayHUD manages the hook DLLs and x86 helper automatically. No per-game DLL placement is required.

### Compatibility and safety notes

- Overlay support depends on the game, graphics API, driver, swap-chain configuration, and display mode. Borderless fullscreen is often a useful first configuration to test.
- HDR, unusual swap-chain formats, multiple render windows, and other overlay software can cause compatibility issues.
- ReplayHUD excludes several common desktop applications and checks for known anti-cheat modules before attempting injection. This is a best-effort filter, not a complete anti-cheat detector.
- Protected games or anti-cheat software may block third-party overlays. ReplayHUD does not bypass anti-cheat or other game protections. Do not use the overlay in games where it is prohibited.
- The desktop notification remains the fallback if the in-game hook cannot initialize or render the notification.

## Build from source

ReplayHUD is a native Windows application and must be built on Windows.

### Build requirements

- Visual Studio 2022 or Visual Studio 2022 Build Tools.
- The **Desktop development with C++** workload and a Windows 10/11 SDK.
- CMake 3.21 or newer.
- Git, for fetching the pinned CMake dependencies.

Open **x64 Native Tools Command Prompt for VS 2022**, change to the repository directory, and run:

```bat
build.bat
```

The script builds the x86 hook, injector, and D3D8 proxy first. It then builds the x64 application and D3D8 proxy. The x64 EXE embeds the x86 components and its own hook.

The local build produces:

```text
dist/ReplayHUD.exe
dist/d3d8.dll
dist/x86/d3d8.dll
ReplayHUD-Windows.zip
```

The ZIP contains the same user-facing package published on GitHub Releases. Build outputs and dependencies remain in the `build` and `build-x86` directories.

### Equivalent CMake commands

To build without the upstream backend test hosts, use:

```bat
cmake -S . -B build-x86 -A Win32 -DREPLAYHUD_BUILD_BACKEND_TESTS=OFF
cmake --build build-x86 --config Release --target ReplayHUDHook d3d8to9 ReplayHUDInjector32 --parallel

cmake -S . -B build -A x64 -DREPLAYHUD_BUILD_BACKEND_TESTS=OFF ^
  -DREPLAYHUD_X86_HOOK="%CD%\build-x86\Release\ReplayHUDHook.dll" ^
  -DREPLAYHUD_X86_INJECTOR="%CD%\build-x86\Release\ReplayHUDInjector32.exe"
cmake --build build --config Release --parallel
```

CMake fetches pinned revisions of Universal Dear ImGui Hook and `d3d8to9`. The project uses the static MSVC runtime.

## GitHub Actions and releases

The workflow in [`.github/workflows/release.yml`](.github/workflows/release.yml) runs on pushes to `main`, tags matching `v*`, and manual dispatch. It builds x86 components first, then embeds them into the x64 application. The separate [verification workflow](.github/workflows/verify.yml) builds both architectures and checks PE machine types plus the exact embedded hook and injector payloads in the application and D3D8 proxies.

A tagged release publishes exactly these three assets:

- `ReplayHUD-Windows.zip` — full package, including both D3D8 proxy architectures and license notices.
- `ReplayHUD.exe` — bundled x64 application.
- `d3d8.dll` — standalone x64 D3D8 proxy.

Pull requests are not currently built by this workflow. The release workflow compiles and packages the binaries; it does not run the upstream graphics backend test hosts.

### Create a release

Push your changes to `main`, then create and push a version tag:

```bash
git add .
git commit -m "Prepare release"
git push origin main

git tag -a vX.Y.Z -m "ReplayHUD vX.Y.Z"
git push origin vX.Y.Z
```

Replace `X.Y.Z` with the version you are releasing. The tag push starts a new workflow run. When the build succeeds, the release job publishes the three assets and generates release notes.

A manual run from the Actions tab builds and uploads an artifact. It does not publish a GitHub Release because the release job only runs for version-tag refs.

See [all workflow runs](https://github.com/vaproh/ReplayHUD/actions) and [published releases](https://github.com/vaproh/ReplayHUD/releases).

## OBS WebSocket protocol

ReplayHUD implements the OBS WebSocket v5 connection flow, including the `Hello` / `Identify` handshake and SHA-256 challenge-response authentication. It subscribes to output events and handles replay-buffer state notifications from both the dedicated start/stop events and `ReplayBufferStateChanged`. Equivalent state updates are deduplicated so one OBS state transition does not produce repeated notifications. It also handles:

- `ReplayBufferSaved` — reads `eventData.savedReplayPath`.
- `ReplayBufferStarted` — reports that the Replay Buffer started.
- `ReplayBufferStopped` — reports that the Replay Buffer stopped.
- `ReplayBufferStateChanged` — reads `eventData.outputActive`.

## License

ReplayHUD is released under the [MIT License](LICENSE.txt). The release archive includes license notices for its bundled third-party dependencies.
