# ReplayHUD

ReplayHUD is a lightweight native Windows notification app for OBS Studio's Replay Buffer. When OBS saves a replay, ReplayHUD shows a desktop toast with the saved filename and can play a notification sound.

![ReplayHUD app icon](assets/ReplayHUD-icon-preview.png)

## Features

- **Saved-clip notifications:** listens for OBS WebSocket v5 `ReplayBufferSaved` events and displays the actual saved filename.
- **Click to open:** click a replay notification to open the saved clip in its default application.
- **System tray menu:** open Settings, test the notification, open the last replay, view the log, or exit.
- **Settings window:** configure the OBS WebSocket port/password, sound, notification duration, screen position, edge margin, and Windows startup.
- **Password protection:** stores the OBS WebSocket password using Windows DPAPI for the current Windows user account.
- **Native Windows app:** C++17 and Win32 APIs; no Electron, browser runtime, .NET runtime, game injection, or third-party DLLs.
- **Embedded icon:** the ReplayHUD icon is compiled into the executable and used for the app, tray, and notification.
- **Automatic reconnect:** changing the OBS connection settings requests a clean WebSocket shutdown and reconnect.

## Requirements

- Windows 10 version 1703 or newer, 64-bit.
- OBS Studio with the built-in WebSocket server (OBS Studio 28 or newer is recommended).
- OBS Replay Buffer enabled and started.

## Download

Download `ReplayHUD-Windows-x64.zip` or `ReplayHUD.exe` from the repository's [Releases](../../releases) page. Extract the ZIP and run `ReplayHUD.exe`.

On first launch, ReplayHUD starts in the system tray. Click the tray icon with the right mouse button and choose **Settings...**.

## Configure OBS

1. Open OBS Studio.
2. Go to **Tools → WebSocket Server Settings**.
3. Enable the WebSocket server.
4. Keep the server on its local machine interface. ReplayHUD connects only to `127.0.0.1`.
5. Note the port (OBS commonly uses `4455`) and password, if authentication is enabled.
6. Enable and start **Replay Buffer**.
7. Open ReplayHUD's tray menu → **Settings...** and enter the same port/password.
8. Click **Save settings**. ReplayHUD reconnects using the new values.

If OBS WebSocket authentication is disabled, leave the password field empty.

## Settings

Open Settings from the ReplayHUD tray menu.

| Setting | Default | Range / meaning |
|---|---:|---|
| OBS WebSocket port | `4455` | 1–65535; must match OBS |
| OBS WebSocket password | empty | Encrypted with DPAPI for the current Windows user |
| Notification sound | On | Plays the Windows `SystemAsterisk` sound |
| Notification duration | `2400 ms` | 800–10000 ms |
| Screen edge margin | `24 px` | 8–200 px |
| Position | Top right | Top right, top left, bottom right, bottom left |
| Start with Windows | Off | Adds/removes a per-user Run registry entry |

Settings are saved to:

```text
%LOCALAPPDATA%\ReplayHUD\ReplayHUD.ini
```

The connection log is saved to:

```text
%LOCALAPPDATA%\ReplayHUD\ReplayHUD.log
```

The password is stored as a DPAPI-protected value. It is intended to be decrypted by the same Windows user on the same machine; it is not a portable password backup.

## Use

- **Right-click tray icon → Settings...** opens the settings window.
- **Right-click tray icon → Test notification** previews the notification and sound.
- **Right-click tray icon → Open last replay** opens the most recently saved clip.
- **Right-click tray icon → Open log file** opens the connection log.
- **Right-click tray icon → Exit ReplayHUD** closes the app.
- **Click a saved-clip notification** to open that clip.

ReplayHUD does not record video, modify OBS scenes, or change Replay Buffer files. It only reacts to OBS's save event.

## Build from source

### Requirements

- Visual Studio 2022 or Visual Studio 2022 Build Tools with **Desktop development with C++**.
- CMake 3.20 or newer.
- Windows 10/11 SDK.

### Build

Open **x64 Native Tools Command Prompt for VS 2022**, change to the repository directory, and run:

```bat
build.bat
```

The executable is created at:

```text
build\Release\ReplayHUD.exe
```

You can also build directly with CMake:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

The build uses the static MSVC runtime (`/MT`). The application icon and Windows file-version metadata are embedded from `assets/ReplayHUD.ico` and `ReplayHUD.rc.in`.

## Automated GitHub Releases

The workflow at `.github/workflows/release.yml` builds a Windows x64 executable whenever a tag matching `v*` is pushed.

```bash
git add .
git commit -m "Prepare release"
git push origin main

git tag -a v1.1.0 -m "ReplayHUD v1.1.0"
git push origin v1.1.0
```

For a tagged build, GitHub Actions:

1. Configures and compiles the x64 Release executable on a Windows runner.
2. Packages `ReplayHUD.exe`, `README.md`, and `LICENSE.txt` into `ReplayHUD-Windows-x64.zip`.
3. Publishes the executable and ZIP to a GitHub Release.
4. Generates release notes automatically from repository changes.

A manual run from the Actions tab builds and uploads workflow artifacts but does not publish a GitHub Release. The workflow requests `contents: write`; repository Actions policy must allow release publishing.

## Display limitations

ReplayHUD is a normal topmost Windows desktop window, not an injected game overlay. Use borderless-windowed mode for the most reliable visibility over games. Exclusive-fullscreen applications and some anti-cheat or overlay policies can prevent third-party desktop windows from appearing above the game.

## Troubleshooting

- Check the tray tooltip. It reports whether ReplayHUD is connected, waiting, or has an OBS WebSocket error.
- Confirm OBS WebSocket is enabled and the port/password match.
- Confirm Replay Buffer is started, not merely configured.
- Use **Test notification** to verify the Windows notification UI independently of OBS.
- Open `%LOCALAPPDATA%\ReplayHUD\ReplayHUD.log` for connection details.

## Protocol

ReplayHUD uses OBS WebSocket v5's `Hello` / `Identify` handshake, SHA-256 challenge-response authentication, and the `Outputs` event subscription. It handles the `ReplayBufferSaved` event and reads `eventData.savedReplayPath`.

## License

MIT. See [LICENSE.txt](LICENSE.txt).
