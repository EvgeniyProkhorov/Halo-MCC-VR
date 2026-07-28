# MCC VR Alpha 0.3.1

> ### READ THIS FIRST
>
> **DO NOT ACCEPT FIXES FOR THIS MOD FROM OUTSIDE SOURCES. WE DON'T KNOW WHAT'S
> IN THEM.**
>
> **SteamVR must be your default OpenXR runtime.** The SteamVR Beta is no longer
> required for the double vision — 0.3.1 fixes that properly (see below) — but it
> does no harm if you are already on it.
>
> Compatibility across headsets may vary. If it doesn't work, try a different
> way to connect if you can.
>
> **Let me know in the [issues](https://github.com/pancreations/Halo-MCC-VR/issues)
> if your headset is not working** and someone or I can help you.
>
> For a GPU performance boost, try
> [Quad-Views-Foveated](https://github.com/mbucchia/Quad-Views-Foveated).
>
> **Please list your specs if you're having issues.** It's a guessing game over
> here.

A hotfix over 0.3.0. Two things, both of which stopped people playing.

## Game Pass / Xbox app / Microsoft Store MCC is now supported

The same download now works on **both** editions of MCC, and **you do not rename
any game file**. If you previously renamed `MCCWinStore-Win64-Shipping.exe` to
the Steam name to force an older build to start, rename it back.

Three separate things were wrong, and all three are fixed:

- **The game could never be started.** A Store app has no package identity when
  it is launched as a plain executable, so MCC exited by itself in about a fifth
  of a second, every time. The launcher now starts it the way Windows does, and
  injects once the game is really up.
- **Halo: Reach stayed stock** — no VR, no controls — while Halo 3 and ODST
  worked. Reach verifies the game file it hooks, and that check covered only the
  Steam signature. The two editions are the *same code*; only the certificate
  differs. Both signatures are now accepted.
- The log now names which edition, which OpenXR runtime, and which streaming app
  a session ran on, so a bug report can be tied to a setup.

**Known: MCC freezes for about nine seconds on the first loading screen on Game
Pass. It has not crashed — wait it out.** This is MCC's own loop stopping, not
the mod: measured back to back on one PC, the Store edition stalls 9.0 seconds
every launch and the Steam edition stalls zero times. Anti-cheat and encrypted
game content were both tested and ruled out. It clears by itself.

Frame rate on Game Pass is **not** worse than Steam — both settle at the same
ceiling. If yours feels halved, that is your VR runtime's motion smoothing / ASW
pacing the game at half your headset's refresh; turn motion smoothing off or
lower `resolution_scale`.

## Double vision on ALVR is fixed

If you stream with ALVR you may have seen the whole image doubled, as though
each eye were showing two overlapping copies. Virtual Desktop and Steam Link were
fine on the very same headset, which is what gave it away.

The mod was telling the runtime a field of view that was not the headset's own.
Compositors that re-project to the real lens values absorbed the difference;
ALVR does not, so it drew the mismatch. The mod now submits the runtime's exact
per-eye field of view.

## Install

1. Download `MCC_VR_ALPHA_0.3.1.zip` below and unzip it.
2. Copy `halo3xr.dll`, `halo3xr_launcher.exe` and `halomccvr.cfg` into a folder
   named exactly `Halo_MCC_VR` inside your MCC install.
   - **Steam:** Manage > Browse local files.
   - **Microsoft Store / Game Pass:** in the Xbox app, ... > Manage > Files >
     Browse, then open the `Content` folder (the one next to
     `MicrosoftGame.config`).
3. Make SteamVR the default OpenXR runtime, start SteamVR, then run
   `halo3xr_launcher.exe`.
   - On Steam, start Steam too.
   - On Game Pass, be signed in to the Xbox app. **Do not start MCC yourself** —
     the launcher starts it for you, and it takes a few seconds longer to appear
     than on Steam.

### Updating — replace your config

**Replace `halomccvr.cfg` with the one in the ZIP. Do not keep your old one.**

There is no migration step: any setting your old file is missing silently falls
back to a built-in default rather than the shipped value. The most visible
casualty is `fit_desktop_window`, whose built-in default is off while the shipped
config turns it on — so keeping an old config can cap your headset frame rate.
Sharpening, HUD and weapon-alignment values regress the same way.

If you want your own tuning back, copy your old `halomccvr.cfg` somewhere safe
first, install the new one, then re-apply your preferences through the F1 menu.

## Known issues

- **Game Pass: MCC freezes about nine seconds on the first loading screen.** Not
  a crash, not the mod. Wait it out.
- On Reach, character tags and navpoints are misplaced in 3D, and the
  `hud_curvature` and `hud_vertical_offset` settings have no effect.
- ODST's first captioned opening cutscene can be black. Skip that first scene
  once; do not repeatedly skip or you will miss the working drop sequence.
- MCC can retain multiple title modules after switching games. If a level
  returns to the menu, fully close and restart MCC.
- ODST brightness stays at the game default.

## Verify your download (optional)

```text
ZIP      __ZIP_SHA256__
DLL      __DLL_SHA256__
Launcher __LAUNCHER_SHA256__
```

Windows or antivirus may warn about the unsigned files; that is expected for VR
mods. Allow the two binaries (or the `Halo_MCC_VR` folder) rather than disabling
security. Launch only through the included launcher, with anti-cheat disabled.

Source commit: `__SOURCE_COMMIT__`. Built Release x64 with Halo 3, ODST and Reach
enabled.
