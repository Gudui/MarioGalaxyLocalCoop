# Super Mario Galaxy Local Co-op

Super Mario Galaxy Local Co-op adds a second full playable character (Luigi) to **Super Mario Galaxy** (Nintendo Switch, **Update 1.3.1**), enabling two players to play together in dynamic/permanent split-screen on one console or emulator.

This is a local co-op modification built using an ExLaunch `subsdk9` module; it is not an online or networked multiplayer mod.

This is an early alpha release. Save backups are recommended. Known limitations and gameplay notes are documented in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Support status

| Target | Status | Notes |
|---|---|---|
| Super Mario Galaxy (Switch) | **Update 1.3.1** | Title ID `010099C022B96000`. **Update `1.3.1` is required** (Base `1.0.0` has a different `main` binary and is unsupported). |
| Atmosphere / physical Switch | Supported (alpha) | Tested on real Switch hardware; includes generated `main.npdm`. |
| Eden / Ryujinx | Supported (alpha) | Tested on Eden v0.2.1; works with two configured controllers. |

## What the mod adds

- **Two playable characters (Mario & Luigi)** on the same stage, each controlled by an independent controller.
- **Permanent split-screen with independent cameras**:
  - Player 1 (Mario) uses the native camera eased out up to 1.5x (capped at +800 units over native).
  - Player 2 (Luigi) has an independent follow camera that orbits smoothly behind his movement and adapts to spherical planet gravity.
  - Launch Stars, Sling Stars, and green pipes preserve the split.
- **Split-screen bloom & rendering parity**: each half runs its own EFB bloom pass so castle garden glow and lamp lighting render cleanly without cross-half flicker.
- **Independent health & lives HUD**: separate 1P (Mario) and 2P (Luigi) health meters and life counters, with 1-Up routing and isolated per-player death handling.
- **Prologue & stage co-op**: Peach's Castle Garden and Gateway Galaxy are playable in co-op, with synchronized spin unlocks, Gateway flip panels, and nearer-player NPC/object interaction.
- **Stability & crash guards**: guarded emitter pools, safe resource lookup fallbacks, and cross-player status close routing.

## Requirements

- A legally obtained copy of **Super Mario Galaxy** for Nintendo Switch (**Update 1.3.1**, Title ID `010099C022B96000`). Base `1.0.0` without Update `1.3.1` will not work.
- A Switch running Atmosphere custom firmware, or a compatible emulator (Eden / Ryujinx).
- Two controllers configured as Player 1 and Player 2.
- The `MarioGalaxyLocalCoop-v0.1.0-alpha.1.zip` release package.

## Install

### Atmosphere (real Switch)

1. Download `MarioGalaxyLocalCoop-v0.1.0-alpha.1.zip` and its `.sha256` sidecar from the [GitHub Releases](https://github.com/Gudui/MarioGalaxyLocalCoop/releases) page.
2. Verify the package checksum:

   ```powershell
   Get-FileHash .\MarioGalaxyLocalCoop-v0.1.0-alpha.1.zip -Algorithm SHA256
   ```

3. Extract the ZIP archive and copy `payload/contents/010099C022B96000/` into `sd:/atmosphere/contents/010099C022B96000/` (or run `.\install.ps1 -Target Atmosphere -ContentsRoot X:\atmosphere\contents`).
4. Launch Super Mario Galaxy (do not hold `L`, which bypasses Atmosphere mods).

### Emulator (Eden / Ryujinx)

Extract the ZIP archive and run:

```powershell
Unblock-File .\install.ps1
.\install.ps1 -Target Eden
```

(Or `-Target Ryujinx` for Ryujinx). The installer verifies every file against `CHECKSUMS.txt` and backs up any replaced files before installing.

## Diagnostics log

While running, the mod writes a session log to `sd:/GalaxyCoop/smg1_log.txt` (or the emulator's SDMC equivalent), keeping the previous session in `smg1_log_prev.txt`.

## How it works

The mod is implemented as an ExLaunch `subsdk9` native module targeting Title ID `010099C022B96000` (validated at boot against the `main` executable's module build ID).

- **Dual actor spawning**: spawns a second native `MarioActor` with Luigi's model and binds it to controller port 2.
- **Dual-pass split rendering**: renders each player's half-viewport with independent camera matrices, J3D draw-matrix twins, and per-half EFB copy rectangles for bloom post-processing.
- **Per-player life & death routing**: isolates black-hole, sink, non-stop, and damage deaths per player so a single player's death enters a temporary ghost/respawn cycle near their partner instead of triggering a full stage reload.
- **Player-aware stage objects**: routes interactive actors, flip panels, and status closures to whichever player triggered them while keeping cutscene locks anchored cleanly on Mario.

## Building from source

Clone the repository with its pinned ExLaunch submodule:

```powershell
git clone --recurse-submodules https://github.com/Gudui/MarioGalaxyLocalCoop.git
Set-Location MarioGalaxyLocalCoop
```

Build with a local devkitPro installation:

```powershell
.\scripts\build.ps1 -DevkitProRoot C:\devkitPro
.\scripts\package.ps1
.\scripts\verify.ps1
```

Or build through the pinned official devkitPro container:

```powershell
.\scripts\build.ps1 -UseDocker
```

Outputs are written below `artifacts/`. The build refuses an ExLaunch checkout that is not at the repository's pinned commit.

## Contributing and credits

Bug reports and focused pull requests are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) first, especially the rules against including game files, keys, dumps, or private information.

Project and third-party credits are in [CREDITS.md](CREDITS.md) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Source is licensed under [GPL-2.0-only](LICENSE).

Super Mario Galaxy and all related game assets are copyright Nintendo. This is an unofficial fan project and is not affiliated with or endorsed by Nintendo.
