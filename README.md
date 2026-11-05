# ReplayHUD

[![Build](https://github.com/vaproh/ReplayHUD/actions/workflows/release.yml/badge.svg?branch=main)](https://github.com/vaproh/ReplayHUD/actions/workflows/release.yml)
[![Latest release](https://img.shields.io/github/v/release/vaproh/ReplayHUD?display_name=tag)](https://github.com/vaproh/ReplayHUD/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE.txt)

**ReplayHUD** is a lightweight native Windows companion for OBS Studio. When OBS saves a Replay Buffer clip, ReplayHUD displays a compact notification with the clip's filename. It can also show replay-buffer state changes, play a notification sound, and open the saved clip when you click its notification.

ReplayHUD can display notifications on the desktop or, where a compatible game renderer is available, inside the foreground game.

![ReplayHUD app icon](assets/ReplayHUD-icon-preview.png)

## Features

- **Saved replay notifications** — receives OBS WebSocket v5 \`ReplayBufferSaved\` events and displays the path reported by OBS.
- **Replay Buffer status** — notifies you when the buffer starts or stops.
- **Click to open** — click a saved-replay notification to open the clip with its default Windows application.
- **System tray controls** — open Settings, preview a notification, open the last replay, view the log, or exit.
- **Configurable notifications** — choose the screen corner, edge margin, duration, and notification sound.
- **OBS connection settings** — configure the local WebSocket port and optional password.
- **Automatic reconnect** — changing the OBS connection settings reconnects using the new configuration.
- **Optional Windows startup** — launch ReplayHUD automatically when you sign in.
- **In-game notifications** — a companion hook can render notifications through supported Direct3D, Vulkan, and OpenGL backends.
- **Native and lightweight** — written in C++17 with Win32 APIs. No Electron, browser runtime, or .NET runtime is required.

## Requirements

- Windows 10 or Windows 11, 64-bit.
- OBS Studio with the obs-websocket v5 server. OBS Studio 28 and newer include it.
- OBS Replay Buffer enabled and started.
- For the in-game overlay: a game using a supported graphics API and allowing third-party overlays. Compatibility varies by game, renderer, driver, and display mode.

ReplayHUD connects to OBS on the local machine (\`127.0.0.1\`). It does not need an external server or cloud account.

## Download and install

1. Open [Releases](https://github.com/vaproh/ReplayHUD/releases).
2. Download \`ReplayHUD-Windows.zip\` from the release you want.
3. Extract the entire ZIP to a folder. Keep the included DLLs and helper executable alongside \`ReplayHUD.exe\` in the supplied directory structure.
4. Run \`ReplayHUD.exe\`.

ReplayHUD starts in the Windows system tray. Right-click its tray icon and select **Settings...** to configure the OBS connection.

### Package contents

The release ZIP contains:

| File | Purpose |
|---|---|
| \`ReplayHUD.exe\` | Main 64-bit desktop application |
| \`ReplayHUDHook.dll\` | 64-bit in-game notification hook |
| \`d3d8.dll\` | 64-bit Direct3D 8-to-9 proxy |
| \`ReplayHUDInjector32.exe\` | Helper used to load the 32-bit hook into 32-bit games |
| \`x86/ReplayHUDHook.dll\` | 32-bit in-game notification hook |
| \`x86/d3d8.dll\` | 32-bit Direct3D 8-to-9 proxy |
| \`README.md\` | This guide |
| \`LICENSE*.txt\` | ReplayHUD and third-party license notices |

**Keep the folder structure intact.** The main application expects the 32-bit helper and hook in these relative locations.

## Configure OBS Studio

1. Open OBS Studio.
2. Go to **Tools → WebSocket Server Settings**.
3. Enable the WebSocket server.
4. Keep the server bound to the local machine. ReplayHUD connects to \`127.0.0.1\`.
5. Note the server port and password. The default OBS WebSocket port is usually \`4455\`.
6. Enable and start **Replay Buffer**.
7. Open the ReplayHUD tray menu and choose **Settings...**.
8. Enter the same port and password used by OBS, then select **Save settings**.

If OBS WebSocket authentication is disabled, leave the password field empty. ReplayHUD reconnects after you save changed connection settings.

## Settings

Open **Settings...** from the ReplayHUD tray menu.

| Setting | Default | Details |
|---|---:|---|
| OBS WebSocket port | \`4455\` | Port number from OBS; valid range is 1–65535 |
| OBS WebSocket password | Empty | Optional; protected with Windows DPAPI |
| Notification sound | On | Uses the Windows \`SystemAsterisk\` sound |
| Notification duration | \`2400 ms\` | Valid range: 800–10000 ms |
| Screen edge margin | \`24 px\` | Valid range: 8–200 px |
| Notification position | Top right | Top right, top left, bottom right, or bottom left |
| Start with Windows | Off | Adds or removes a per-user startup entry |

### Configuration and logs

ReplayHUD stores its files in:

\`\`\`text
%LOCALAPPDATA%\\ReplayHUD\\
\`\`\`

| File | Contents |
|---|---|
| \`ReplayHUD.ini\` | Application and connection settings |
| \`ReplayHUD.log\` | OBS connection and application log |
| \`ReplayHUDHook.log\` | In-game hook diagnostics |

The OBS password is protected with Windows Data Protection API (DPAPI). It is intended to be decrypted by the same Windows user on the same machine; copying the INI file to another Windows account or computer does not make the password portable. If the saved password cannot be decrypted, enter it again in Settings.

## Using ReplayHUD

ReplayHUD runs in the system tray.

| Action | Result |
|---|---|
| **Settings...** | Configure the OBS connection and notification appearance |
| **Test notification** | Preview a notification and its sound without saving a replay |
| **Open last replay** | Open the most recently saved replay reported by OBS |
| **Open log file** | Open the application log |
| **Exit ReplayHUD** | Close the application |

When OBS reports a saved replay, ReplayHUD displays the filename/path. Click the notification to open the clip in its default application.

ReplayHUD does not record, encode, move, rename, or edit video files. OBS remains responsible for recording and saving the Replay Buffer clips.

## In-game overlay

ReplayHUD includes a companion hook based on [Universal Dear ImGui Hook](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook). The main application publishes notification data through shared memory. The hook renders that notification in a compatible foreground game; if the in-game renderer does not become ready, ReplayHUD keeps the desktop notification as a fallback.

### Graphics API support

The current build integrates these renderer backends:

| Graphics API | Implementation | Notes |
|---|---|---|
| Direct3D 8 | \`d3d8to9\` proxy + ReplayHUD hook | Requires manual, per-game proxy placement |
| Direct3D 9 | Universal Dear ImGui Hook backend | Game/driver compatibility varies |
| Direct3D 10 | Universal Dear ImGui Hook backend | Game/driver compatibility varies |
| Direct3D 11 | Universal Dear ImGui Hook backend | Game/driver compatibility varies |
| Direct3D 12 | Universal Dear ImGui Hook backend | Game/driver compatibility varies |
| Vulkan | Universal Dear ImGui Hook backend | Game/driver compatibility varies |
| OpenGL | Universal Dear ImGui Hook backend | WGL; game/driver compatibility varies |

Backend integration does not mean every game has been validated. Treat the overlay as experimental and test it with the specific games and graphics settings you use.

### Direct3D 8 games

D3D8 games use the bundled \`d3d8to9\` proxy to translate Direct3D 8 calls to Direct3D 9. To try the overlay in a D3D8 game:

1. Close the game.
2. Copy the architecture-matching \`d3d8.dll\` and \`ReplayHUDHook.dll\` into the game's executable directory.
   - 64-bit game: use the two files in the ZIP root.
   - 32-bit game: use \`x86/d3d8.dll\` and \`x86/ReplayHUDHook.dll\`.
3. Start ReplayHUD, then launch the game.
4. To undo the proxy installation, close the game and remove the local \`d3d8.dll\` and \`ReplayHUDHook.dll\` files you copied there.

The proxy loads ReplayHUD's hook from the same directory. Some older games may behave differently under D3D8-to-D3D9 translation. A few titles may also need the legacy DirectX End-User Runtime for D3DX shader helpers.

For D3D9, D3D10/11/12, Vulkan, and OpenGL, ReplayHUD attempts to hook the foreground process when it detects a supported graphics runtime. The main application is 64-bit and delegates 32-bit injection to \`ReplayHUDInjector32.exe\`.

### Compatibility and safety notes

- Overlay support depends on the game, graphics API, driver, swap-chain configuration, and display mode. Borderless fullscreen is often a useful first configuration to test.
- HDR, unusual swap-chain formats, multiple render windows, and other overlay software can cause compatibility issues.
- ReplayHUD excludes several common desktop applications and checks for known anti-cheat modules before attempting injection. This is a best-effort filter, not a complete anti-cheat detector.
- Protected games or anti-cheat software may block third-party overlays. ReplayHUD does not bypass anti-cheat or other game protections. Do not use the overlay in games where it is prohibited.
- The desktop notification remains the fallback if the in-game hook cannot initialize or render the notification.
- Hook diagnostics are written to \`%LOCALAPPDATA%\\ReplayHUD\\ReplayHUDHook.log\` in the target process.

## Build from source

ReplayHUD is a Windows application and must be built on Windows.

### Build requirements

- Visual Studio 2022 or Visual Studio 2022 Build Tools.
- The **Desktop development with C++** workload and a Windows 10/11 SDK.
- CMake 3.21 or newer.
- Git, for fetching the pinned CMake dependencies.

### Build the application and companion binaries

Open **x64 Native Tools Command Prompt for VS 2022**, change to the repository directory, and run:

\`\`\`bat
build.bat
\`\`\`

The script configures x64 and x86 builds, builds the application and companion components, and enables the upstream backend test hosts for local development.

Expected primary outputs:

\`\`\`text
build\\Release\\ReplayHUD.exe
build\\Release\\ReplayHUDHook.dll
build\\d3d8-proxy\\Release\\d3d8.dll
build-x86\\Release\\ReplayHUDInjector32.exe
build-x86\\Release\\ReplayHUDHook.dll
build-x86\\d3d8-proxy\\Release\\d3d8.dll
\`\`\`

### Build without the upstream test hosts

The GitHub Actions release workflow disables the upstream backend test hosts to keep release builds focused on compiling and packaging the application. To reproduce that build locally:

\`\`\`bat
cmake -S . -B build -A x64 -DREPLAYHUD_BUILD_BACKEND_TESTS=OFF
cmake --build build --config Release --parallel

cmake -S . -B build-x86 -A Win32 -DREPLAYHUD_BUILD_BACKEND_TESTS=OFF
cmake --build build-x86 --config Release --target ReplayHUDHook d3d8to9 ReplayHUDInjector32 --parallel
\`\`\`

CMake fetches pinned revisions of Universal Dear ImGui Hook and \`d3d8to9\`. The project uses the static MSVC runtime.

## GitHub Actions and releases

The workflow in [\`.github/workflows/release.yml\`](.github/workflows/release.yml) has two triggers:

- A push to \`main\` builds x64 and x86 binaries and uploads a workflow artifact.
- A push of a tag matching \`v*\` builds the same binaries, uploads the artifact, then publishes a GitHub Release containing the ZIP and individual binaries.

Pull requests are not currently built by this workflow. The release workflow is a build-and-package pipeline; it does not run the removed graphics test suite.

### Create a release

Push your changes to \`main\`, then create and push a version tag:

\`\`\`bash
git add .
git commit -m "Prepare release"
git push origin main

git tag -a vX.Y.Z -m "ReplayHUD vX.Y.Z"
git push origin vX.Y.Z
\`\`\`

Replace \`X.Y.Z\` with the version you are releasing. The tag push starts a new workflow run. When the build succeeds, the release job publishes the package and generates release notes.

If you move or recreate an existing tag, push the updated ref explicitly:

\`\`\`bash
git tag -f vX.Y.Z <commit>
git push origin refs/tags/vX.Y.Z --force
\`\`\`

A manual run from the Actions tab builds and uploads an artifact. It does not publish a GitHub Release because the release job only runs for version-tag refs.

See [all workflow runs](https://github.com/vaproh/ReplayHUD/actions) and [published releases](https://github.com/vaproh/ReplayHUD/releases).

## OBS WebSocket protocol

ReplayHUD implements the OBS WebSocket v5 connection flow, including the \`Hello\` / \`Identify\` handshake and SHA-256 challenge-response authentication. It subscribes to output events and handles:

- \`ReplayBufferSaved\` — reads \`eventData.savedReplayPath\`.
- \`ReplayBufferStarted\` — reports that the Replay Buffer started.
- \`ReplayBufferStopped\` — reports that the Replay Buffer stopped.
- \`ReplayBufferStateChanged\` — reads \`eventData.outputActive\`.

## License

ReplayHUD is released under the [MIT License](LICENSE.txt). The release archive also includes license notices for its bundled third-party dependencies.
