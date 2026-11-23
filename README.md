# Peggle 2 Recompilation

Static Xbox 360 recompilation of *Peggle 2* powered by [ReXGlue](https://github.com/rexglue/rexglue-sdk) with native **Windows (x64 D3D12)** and **iOS (ARM64 Metal)** builds.

> **No retail game files are included or distributed.** Both releases ship asset-free; you must supply your own legally dumped copy of *Peggle 2* (Xbox 360).

## Downloads

Download the latest rolling prerelease packages from the [Releases](../../releases) page:

- **Windows (`latest-windows`)**: `Peggle2-Windows-x64.zip`
- **iOS (`latest-ios`)**: `Peggle2-iOS.ipa` (unsigned; sideload with AltStore / SideStore / Sideloadly preserving `extended-virtual-addressing` and `increased-memory-limit` entitlements)

## Supplying Your Game Files

Supported formats on both platforms:
- **Extracted game folder** containing `Default.xex` and `main.rsb`
- **Xbox 360 `.iso`** image (GDFX)
- **Single-file XBLA STFS container** (`LIVE` / `PIRS` / `CON `)

### Windows x64
1. Extract `Peggle2-Windows-x64.zip` anywhere.
2. Launch `peggle2.exe`:
   - **First-launch picker**: Click **Browse Folder...** or **Browse ISO / XEX / XBLA...** when prompted. Your selection is saved to `peggle2.toml` (`game_path`) for future launches.
   - **Auto-detect**: Place your game folder, `.iso`, or XBLA container inside a `./game/` folder next to `peggle2.exe`.
   - **CLI / Drag-and-drop**: Run `peggle2.exe --game "<path>"` or drag a folder, `.iso`, XBLA container, or `Default.xex` onto `peggle2.exe`.
   - Press **F1** in-game and click **Change Game Source** anytime to switch copies.

### iOS (ARM64)
1. Sideload `Peggle2-iOS.ipa` onto your iPhone/iPad (iOS 16+).
2. Provide game files using either option:
   - **Native in-app picker**: Tap **Select Game Folder** or **Select .ISO / .XEX File** on first launch (saved via security-scoped bookmark; iOS App Sandbox requires choosing the containing folder or a single `.iso`/STFS file rather than an isolated `Default.xex`).
   - **Files app**: Copy your extracted files, `.iso`, or XBLA container into `Files -> On My iPhone -> Peggle 2 -> game`.
   - Open the in-game **Menu** overlay and tap **Change Game Source** anytime to switch copies.
