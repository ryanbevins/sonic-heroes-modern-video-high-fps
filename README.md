# Sonic Heroes: Modern Video and High FPS

**Beta (0.3.0-beta).** The features below work in testing, but expect rough edges. Please report bugs on the [Issues](../../issues) page with your resolution, window mode, FPS setting and the stage where it happened.

A [Reloaded-II](https://github.com/Reloaded-Project/Reloaded-II) mod for the PC version of Sonic Heroes. It adds a **Video** menu where you can change display and graphics settings while the game is running and see the result right away. It can also render above 60 FPS using frame interpolation, while gameplay still runs at the original 60 Hz.

Open **Options** and select the new **TV icon**. The menu works with a mouse, the keyboard, the D-pad or the left stick.

![Options menu with the new Video Setting (TV) icon](images/options-menu.webp)

![Video Setting menu, Display page](images/video-setting-display.webp)

## Features

### Display
- **Window type:** Windowed, Borderless or Fullscreen.
- **Resolution** and **refresh rate** (refresh rate applies to fullscreen).
- **V-Sync** on or off.
- **Anti-aliasing:** MSAA where your graphics card supports it.
- **FPS:** 60, 120, 180, 240, 360, 480 or Unlocked.
- **Safe preview:** display changes are previewed first. If you don't confirm within 15 seconds, they revert automatically, so a bad resolution can't lock you out.

### High FPS
- Camera, characters, animation, effects and menus are interpolated, so motion is smoother above 60 FPS.
- Game logic, physics and timing stay at the original 60 Hz, so the game plays at its normal speed at any frame rate.
- Smoother frame pacing at 60 FPS. The game's own frame timer sometimes doubles a frame; this mod replaces that wait with a steadier clock.
- If V-Sync is holding you at 60 FPS, the menu says so.

### Graphics
- **Texture filtering:** Bilinear, Trilinear, or 2x to 16x Anisotropic.
- **Mipmap detail:** sharper or softer distant textures.
- **Distance fog** on or off.
- **Character shadows:** Simple, Original, High (512) or Ultra (1024).
- **Field of view:** 50% to 150%. Widescreen aspect correction is kept.
- **Enemy shadows:** animated shadows from the enemy's actual model instead of a round blob.

### Interface
- **Brightness** adjustment.
- **FPS display** overlay.

## Requirements

| Mod | Link |
| --- | --- |
| Reloaded-II (mod loader) | https://github.com/Reloaded-Project/Reloaded-II |
| SH Essentials: Widescreen Revamp | https://gamebanana.com/mods/33976 |
| SH Essentials: Graphics | https://github.com/Sewer56/Heroes.Graphics.Essentials.ReloadedII |

Widescreen Revamp needs these too. Reloaded-II normally installs them along with it:

| Mod | Link |
| --- | --- |
| CRI Filesystem Hook | https://github.com/Sewer56/CriFsHook.ReloadedII |
| Reloaded File Redirector | https://gamebanana.com/mods/51225 |
| TONERR | https://github.com/Sewer56/TONERR.ReloadedII |

Reloaded-II may offer to download missing dependencies when you enable this mod. If it doesn't, use the links above.

## Installation

1. Install Reloaded-II and add Sonic Heroes (`Tsonic_win.exe`).
2. Download `ModernVideo-0.3.0-beta-Reloaded-II.zip` from [Releases](../../releases).
3. Drag the `.zip` onto the Reloaded-II window.
4. Enable **Sonic Heroes: Modern Video and High FPS** and its dependencies for Sonic Heroes, then launch the game through Reloaded-II.

## Known limitations (beta)

- Frame rates above 60 need V-Sync off, except in exclusive fullscreen when the FPS setting equals the refresh rate (for example 240 FPS at 240 Hz).
- Object shadows and self shadows appear in the menu but are disabled in this beta.
- Enemy shadows show as "Unavailable" if the graphics setup doesn't support them.
- It needs the PC `Tsonic_win.exe` with the original game code. Copies that differ only in headers or data, such as the 4GB patch, work. If the code differs, or another mod already hooked the same code, it shows an error naming the problem and stays off instead of patching unknown code.
- Not all stages and transitions have been tested at every frame rate.

## Building from source

Requires Windows and Visual Studio 2022 with the C++ x86 build tools.

```powershell
.\build.ps1
```

The mod is written to `dist\sonicheroes.graphics.modern-video`. Copy that folder into `Reloaded-II\Mods`.

`.\build.ps1 -Test` also runs the isolated checks. Some checks read menu art extracted from your own copy of the game and files from an installed Widescreen Revamp, which are not included in this repository, so they fail without them. A few frame-timing checks measure real time and can fail on a busy system.

## Credits

- [Reloaded-II](https://github.com/Reloaded-Project/Reloaded-II) and SH Essentials: Graphics by Sewer56.
- [SH Essentials: Widescreen Revamp](https://github.com/RaphaelDrewBoltman/sh-widescreen-revamp) by RaphaelDrewBoltman.
- Frame pacing and thread scheduling improvements by [Cole144](https://github.com/Cole144).

Sonic Heroes is a trademark of SEGA. This project is not affiliated with or endorsed by SEGA.

## License

[MIT](LICENSE)
