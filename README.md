# ReplayHUD

A lightweight Windows notification HUD for OBS Studio's Replay Buffer. When OBS saves a replay clip, ReplayHUD displays a brief, ShadowPlay-style toast and can play a notification sound.

> ReplayHUD is a desktop notification utility. It does not record video, modify clips, or interact with games.

## Features

- **Replay-saved notifications:** Displays a toast when OBS reports that a Replay Buffer clip has been saved.
- **Native Windows app:** Built with Win32 C++17. No Electron, browser runtime, .NET runtime, or third-party DLLs.
- **OBS WebSocket v5:** Connects to OBS through its local WebSocket server and listens for the `ReplayBufferSaved` event.
- **System tray controls:** Check connection status, test a notification, open settings, or exit.
- **Configurable HUD:** Adjust notification sound, display duration, and screen-edge margin.
- **No game injection:** No game hooks, DLL injection, frame polling, or game-process access.

## Requirements

- Windows 10 or Windows 11, x64.
- OBS Studio with the built-in WebSocket server enabled.
- OBS Replay Buffer enabled.

## Download

Download the latest **ReplayHUD-Windows-x64.zip** or `ReplayHUD.exe` from the repository's [Releases](../../releases) page.

Extract the ZIP and run `ReplayHUD.exe`. On first launch, ReplayHUD creates its settings file under:

```text
%LOCALAPPDATA%\ReplayHUD\ReplayHUD.ini
```

Windows may show a security prompt for an unsigned build. Only run binaries you have obtained from a release you trust.

## Configure OBS

1. Open OBS Studio.
2. Go to **Tools → WebSocket Server Settings**.
3. Enable the WebSocket server. The default server port is `4455`.
4. Keep the server local. ReplayHUD connects to `127.0.0.1`.
5. If authentication is enabled, note the WebSocket password.
6. Enable **Replay Buffer** in OBS and start it.

Then right-click the ReplayHUD tray icon and choose **Open settings**. Edit the INI file:

```ini
[OBS]
port=4455
password=your_obs_websocket_password

[HUD]
sound=1
duration_ms=1800
margin_px=28
```

If OBS WebSocket authentication is disabled, leave `password=` empty. Restart ReplayHUD after changing the file.

**Security note:** The OBS WebSocket password is stored as plain text in the per-user INI file. Do not share that file. Keep the OBS WebSocket server bound to localhost and do not expose it to an untrusted network.

## Use

Right-click the ReplayHUD tray icon to access:

- **Test notification** — preview the HUD.
- **Open settings** — open the configuration file.
- **Exit ReplayHUD** — close the app.

To start ReplayHUD automatically when you sign in to Windows:

1. Press `Win + R`.
2. Enter `shell:startup`.
3. Place a shortcut to `ReplayHUD.exe` in the Startup folder.

For the HUD to appear over a game, use **borderless fullscreen**. A normal desktop topmost window may not appear over exclusive-fullscreen games, and some games or anti-cheat systems may block third-party overlays. ReplayHUD does not inject into or hook games.

## Settings

| Setting | Default | Description |
|---|---:|---|
| `port` | `4455` | Local OBS WebSocket port |
| `password` | empty | OBS WebSocket password, if authentication is enabled |
| `sound` | `1` | Play the Windows notification sound (`1` on, `0` off) |
| `duration_ms` | `1800` | Toast duration in milliseconds (800–7000) |
| `margin_px` | `28` | Margin from the top-right edge of the active monitor |

## Build from source

### Requirements

- Visual Studio 2022 Build Tools or Visual Studio 2022 with **Desktop development with C++**.
- CMake 3.20 or newer.
- Windows SDK.

### Build using the included script

Open **x64 Native Tools Command Prompt for VS 2022**, change to the repository directory, and run:

```bat
build.bat
```

The executable will be created at:

```text
build\Release\ReplayHUD.exe
```

### Build using CMake directly

```bat
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

The Release target uses the static MSVC runtime (`/MT`) and links against Windows system libraries.

## Automated GitHub Releases

The workflow at `.github/workflows/release.yml` builds ReplayHUD on a GitHub-hosted Windows runner.

Push a version tag beginning with `v` to trigger a build and publish a GitHub Release:

```bash
git add .
git commit -m "Prepare release"
git push origin main

git tag v1.0.0
git push origin v1.0.0
```

For each matching tag, the workflow:

1. Configures and builds the x64 Release executable.
2. Packages `ReplayHUD.exe`, this README, and the license into `ReplayHUD-Windows-x64.zip`.
3. Publishes the executable and ZIP as release assets.
4. Generates release notes automatically from the repository's changes.

You can also run the workflow manually from **Actions**. A manual run uploads workflow artifacts but does not publish a GitHub Release.

The workflow needs permission to publish releases. It requests `contents: write`; ensure the repository's **Settings → Actions → General → Workflow permissions** policy allows it. Use a new tag for each release.

## How it works

ReplayHUD connects to the OBS WebSocket v5 server on `127.0.0.1`, authenticates using OBS's challenge-response procedure when a password is configured, and subscribes to the Outputs event category. When it receives `ReplayBufferSaved`, it displays a short notification containing the saved clip information.

The HUD is a separate Windows desktop window. ReplayHUD does not alter OBS scenes, the Replay Buffer, or the saved video.

## License

See [LICENSE.txt](LICENSE.txt).
