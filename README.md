<img width="1280" height="720" alt="lowmccvr" src="https://github.com/user-attachments/assets/f5e01d00-8bcf-4897-afb5-66c9ead9c158" />
### REACH/3/ODST Now Playable in VR 
### Work on HALO 4 will begin SOON after these hotfixes release. Yes Halo 1/2 will be in the collection eventually!

## 🆕 What's new in 0.3.1

1. **Microsoft Store / Xbox App (Game Pass) support** — the same download now
   works on both MCC editions, and you do not rename any game file.
2. **ALVR / Virtual Desktop (VDXR) / Meta Link support fixed** — the doubled,
   overlapping image some streaming setups showed is gone.
3. **3D Cutscene Theatre Mode** — cutscenes play on a room-fixed screen with
   real stereo depth instead of riding your head, with a depth slider in the
   F1 menu (`cutscene_theater_depth`).
4. **New launch page and a rebuilt F1 menu** — a one-time welcome page, plus a
   sidebar-organized, locked F1 panel with a grab handle.

## 🚧 What I'm working on RN
| Feature                                            | Status         |
| -------------------------------------------------- | -------------- |
| Xbox App / Game Pass Support                       | ✅ Added in 0.3.1 |
| ALVR / Virtual Desktop / Meta Link Support         | ✅ Fixed in 0.3.1 |
| Cutscene 3d Theater Mode                           | ✅ Added in 0.3.1 |
| Complete F1 Menu UI Restructure and Reorganization | ✅ Added in 0.3.1 |
| Reach: black-world fix on sniper                   | ✅ Fixed in 0.3.1 |
| SMAA T2X Support                                   | not possilbe atm |
| Gamepad support/Head-Aiming Mode                   | 🟡 In Progress |
| Scopes and weapon zoom fixes                       | 🟡 In Progress |
| Theater mode subtitles                              | 🟡 In Progress |

> **DO NOT ACCEPT FIXES FOR THIS MOD FROM OUTSIDE SOURCES. WE DON'T KNOW WHAT'S
> IN THEM.**
>
> [Quad-Views-Foveated](https://github.com/mbucchia/Quad-Views-Foveated) is
> **optional and no longer required** — try it only if you want a GPU
> performance boost.
>
> **Steam VR players: use the SteamVR Beta branch**, with SteamVR set as your
> default OpenXR runtime — that's what makes Virtual Desktop's VDXR mode work.
>
> **WINSTORE/XBOX APP: do not rename your game .exe to the Steam version.** The
> launcher now looks for the real Game Pass executable — if you renamed
> `MCCWinStore-Win64-Shipping.exe` in a previous update, rename it back.
>
> Compatibility across headsets may vary. If it doesn't work, try a different
> way to connect if you can. or leave an issue with your spec set up. I'll try to find a play tester with your headset.
>
> **Let me know in the [issues](https://github.com/pancreations/Halo-MCC-VR/issues)
> ** and someone or I can help you.
>
> **Please list your specs if you're having issues.** Saves me a bit of time

# Halo MCC VR

> **Hi, I'm [pancreations](https://www.instagram.com/pancreations/)** — a 3D
> animator. I really don't like AI art, but I also really want to play MCC in
> VR, so I'm taking one for the team. Follow me on Instagram if you like silly
> animations made by humans in Blender. 
>
>
A native OpenXR VR mod for Halo 3, Halo 3: ODST and Halo: Reach in Halo: The
Master Chief Collection — both the Steam edition and the Microsoft Store / Xbox
app (Game Pass) edition.

The current known-good release is
[MCC VR Alpha 0.3.1](https://github.com/pancreations/Halo-MCC-VR/releases/tag/MCC_VR_ALPHA_0.3.1),
a hotfixes update over 0.3.0 that adds **Microsoft Store / Xbox app (Game Pass)
support**, fixes **double vision on ALVR**, adds a **room-fixed 3D cutscene
theatre** to all three games, and fixes Halo: Reach's sniper-triggered
black-world bug. It is an alpha: use it at your own risk, launch only without
anti-cheat, and expect incomplete hardware and gameplay coverage.

## What works

- Per-eye stereo and 6DOF head tracking.
- Motion-controller input, weapon aim, arm IK, snap/smooth turning, melee,
  grenades, and menu control.
- Native HUD, authored floating weapon crosshair, scopes, resolution scaling,
  comfort controls, and a shared F1 configuration menu.
- Halo 3 campaign behavior, including cutscenes, pause/resume, death/respawn,
  and mission transitions.
- ODST stereo, controls, weapons/hands, native HUD, cutscenes, vibration,
  death/respawn recovery, and one tested drivable car.
- Halo: Reach stereo, controls, weapon aim, hands/gun, HUD, cutscenes and
  vibration, plus native vehicle controls and a fix for sniper-triggered black
  static-world geometry, both new in 0.3.1. Reach is new in 0.3.0 and is the
  earliest of the three titles.
- Both MCC editions from one download: Steam, and Microsoft Store / Xbox app
  (Game Pass). All three titles work on either, and no game file is renamed.
- A room-fixed 3D cutscene theatre for all three games, new in 0.3.1: cutscenes
  play on a screen that stays put in the room instead of riding your head, with
  the authored cinematic framing and field of view preserved. Controlled by
  `cutscene_theater_depth` and Flip Depth in the F1 menu.
- L3+R3 recenters VR space and toggles the F1 menu; Y+B is the pause chord in
  ODST and Reach. Both new in 0.3.1.
- A rebuilt F1 menu: sidebar categories, a locked panel with a grab handle, and
  a one-time welcome page. New in 0.3.1.

On Reach the left trigger and X are swapped compared to Halo 3 and ODST, so
grenades sit on X.

Known limitations:

- ODST's first captioned opening cutscene can be black. Skip that first scene
  once; do not repeatedly skip or you will miss the working drop sequence.
- MCC can retain multiple title modules after switching games. If a level
  returns to the menu, fully close and restart MCC.
- ODST brightness stays at the game default.
- On Reach, character tags and navpoints are misplaced in 3D, and the
  `hud_curvature` and `hud_vertical_offset` settings have no effect.
- The right stick still turns you while the F1 menu is open.
- **Microsoft Store / Game Pass only: MCC freezes for about nine seconds on the
  first loading screen. It has not crashed — wait it out.** See below.
- Broader ODST and Reach weapon, turret, passenger-gun, vehicle, co-op, headset,
  and long-session coverage is still needed.

### Game Pass: the nine-second freeze on the first loading screen

On the Microsoft Store / Xbox app edition, MCC's own loading screen locks up for
roughly nine seconds shortly after launch. In the headset this looks alarming:
the picture freezes, and because your headset keeps re-projecting the last frame
it was given, you can still look around a completely still image. It reads
exactly like a crash.

**It is not a crash, and it is not the mod.** It is MCC's own game loop stopping.
Measured on the same PC, same headset, same build, back to back: the Store
edition stalls 9.0 seconds every single launch; the Steam edition records zero
stalls. Two obvious explanations were tested and ruled out — no anti-cheat module
is loaded, and both editions read the same 44 plain, identical map files off the
same SSD. The stall lands within 74 ms of exactly 9.0 seconds on every run, which
looks like something timing out rather than something loading.

Just wait. It clears on its own and the game runs normally afterwards. If you
want to confirm what happened, `Halo_MCC_VR\halo3xr.log` records it as a `STALL`
line naming how long the game was gone.

Frame rate on Game Pass is **not** worse than Steam — both settle at the same
ceiling. If yours feels like it halves, that is your VR runtime's motion
smoothing / ASW pacing the game at half your headset's refresh because it cannot
hold the full rate; the log prints `app cadence` next to `panel` so you can see
it. Turn motion smoothing off, or lower `resolution_scale`, to get out of it.

The exact accepted source and artifact hashes are in
[docs/CURRENT-STATE.md](docs/CURRENT-STATE.md).

## Install

There is no installer script. Both the Steam copy of MCC and the Microsoft Store
/ Xbox app (Game Pass) copy are supported by the same download, and neither
needs any game file renamed.

1. Download the binary asset
   `MCC_VR_ALPHA_0.3.1.zip` from the official `0.3.1` release page.
2. Open MCC's main game folder — the one that contains the `MCC` folder.
   - **Steam:** **Manage > Browse local files**.
   - **Microsoft Store / Game Pass:** in the Xbox app, **... > Manage > Files >
     Browse**, then open the `Content` folder inside it (the one next to
     `MicrosoftGame.config`).
3. Create a folder named exactly `Halo_MCC_VR` in that folder.
4. Copy `halo3xr.dll`, `halo3xr_launcher.exe` and `halomccvr.cfg` into that
   folder.
5. Make SteamVR the default OpenXR runtime and start SteamVR, then run
   `halo3xr_launcher.exe`. On Steam, start Steam as well; the Microsoft Store
   edition does not need Steam running for the game itself, but you must be
   signed in to the Xbox app.

On the Microsoft Store edition the launcher starts the game for you through the
Xbox app, so do **not** start Halo: MCC yourself first. The game takes a few
seconds longer to appear than on Steam — that is expected.

The final path must end in one of:

```text
Halo The Master Chief Collection\Halo_MCC_VR\halo3xr_launcher.exe
Halo- The Master Chief Collection\Content\Halo_MCC_VR\halo3xr_launcher.exe
```

Do not place the files loose in the MCC root. To uninstall, close MCC and delete
only the dedicated `Halo_MCC_VR` folder.

If you previously renamed `MCCWinStore-Win64-Shipping.exe` to the Steam name to
force an older build to launch, rename it back. The launcher records the edition
it detected in `halo3xr_launcher.log`.

### Updating to 0.3.1 — replace your config

**Replace `halomccvr.cfg` with the one in the ZIP. Do not keep your old one.**
This is different from updates before 0.3.0, which told you to keep it.

0.3.0 and later add settings that older config files do not contain, and there is
no migration step: any setting your old file is missing silently falls back to a
built-in default rather than the shipped value. The most visible casualty is
`fit_desktop_window`, whose built-in default is off while the shipped config
turns it on — keeping an old config can therefore cap your headset frame rate.
Sharpening, HUD and weapon-alignment values regress the same way.

The shipped config is a tuned, tested configuration rather than bare defaults.
If you want your own tuning back, copy your old `halomccvr.cfg` somewhere safe
first, install the new one, then re-apply your preferences through the F1 menu.

If one PC behaves differently, first confirm SteamVR is still the default
OpenXR runtime, fully close every MCC process before relaunching, compare the
installed hashes below, and compare `halomccvr.cfg`. Do not use a repository
build folder as an installation source.

Release `0.3.1` hashes:

```text
ZIP      D139C7E7D507E114EB8F54A10D4FD1DAAC797D4722D9E5C29ADB969DBBB9B2AF
DLL      5B4A852C3175021AD433373BACB825BCC5D0EDFC52AF620A26C8A2C55F00BA64
Launcher 9F510506981B882BD571E892B88AD26951CA08948C7DADB031088060E918824A
```

Windows security software may flag or quarantine unsigned injection-based VR
mods. Download only from the official release, verify the hashes, inspect the
source if desired, and allow only the two release binaries rather than disabling
security software globally.

## Required MCC settings

| Setting | Value |
| --- | --- |
| Video > Max Frame Rate | unlimited |
| Video > V-Sync | Off |
| Halo 3 > Field of View | 120 |
| ODST > Look Sensitivity | Maximum |
| ODST > Look Acceleration | Off |
| MCC FSR | Off |

ODST's look settings control how quickly bullet direction catches the
motion-controller crosshair. If shots trail, confirm sensitivity is at maximum
and acceleration is off.

Settings live in `Halo_MCC_VR\halomccvr.cfg`, shipped in the ZIP as a tuned
configuration. The F1 menu edits the same values, and the game regenerates the
file if it is missing — but a regenerated file contains bare built-in defaults,
not the shipped tuning, so keep a copy of the shipped one.

## Build from source

See [BUILDING.md](BUILDING.md). A clean build uses the accepted source and
configuration, but produces a new, unaccepted candidate and file hash. Only the
published hashes remain accepted until that rebuilt candidate passes a headset
test. Use the released ZIP when you need the exact accepted binaries.

## Development evidence

- [Current accepted baseline](docs/CURRENT-STATE.md)
- [Halo 3 reverse-engineering facts](docs/RE-notes.md)
- [ODST signatures](docs/ODST-SIGNATURE-EVIDENCE.md)
- [ODST camera layout](docs/ODST-CAMERA-LAYOUT.md)
- [ODST weapon/IK evidence](docs/ODST-WEAPON-IK-EVIDENCE.md)
- [Reach evidence manifest](docs/REACH-EVIDENCE-MANIFEST.json)
- [Reach signature evidence](docs/REACH-SIGNATURE-EVIDENCE.md)
- [Shared title-runtime ownership candidate](docs/TITLE-RUNTIME-OWNERSHIP.md)
- [Per-title evidence policy](docs/EDITING-KIT-EVIDENCE.md)

The code was written by Claude and Codex under the direction of a human modder
who made the product and reverse-engineering decisions and performed the headset
tests. No human reviewed every line. The project is licensed under the
[MIT License](LICENSE).

Inspired by HaloCEVR by LivingFray and ReclaimerVR by Nibre. Halo is a Microsoft
trademark; this project is not affiliated with Microsoft or Halo Studios.
