# Hotfixes Update — MCC VR Alpha 0.3.1

> ### READ THIS FIRST
>
> **DO NOT ACCEPT FIXES FOR THIS MOD FROM OUTSIDE SOURCES. WE DON'T KNOW WHAT'S
> IN THEM.**
>
> **SteamVR players: use the SteamVR Beta branch, with SteamVR set as your
> default OpenXR runtime.** That's what makes Virtual Desktop's VDXR mode work.
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
> **Please list your specs if you're having issues.** 

The big one. Halo 3, ODST and Reach all play in VR, Game Pass works, and
cutscenes now play on a real screen in front of you instead of being glued to
your face.

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
- The log now names which edition, which OpenXR runtime, which headset and which
  streaming app a session ran on, so a bug report can be tied to a setup.

## Double vision on ALVR is fixed

If you stream with ALVR you may have seen the whole image doubled, as though
each eye were showing two overlapping copies. Virtual Desktop and Steam Link were
fine on the very same headset, which is what gave it away.

The mod was telling the runtime a field of view that was not the headset's own.
Compositors that re-project to the real lens values absorbed the difference;
ALVR does not, so it drew the mismatch. The mod now submits the runtime's exact
per-eye field of view, derived entirely from what your runtime reports — there
are no per-headset constants, so this works on any headset.

## Cutscenes now play in a room-fixed theatre

Cutscenes in **all three games** no longer ride your head. The movie is placed
on a screen that stays put in the room while you keep full 6DOF head tracking,
so you can lean and look around instead of having the camera dragged off your
neck.

- The authored cinematic framing and field of view are preserved, so shots are
  composed the way Bungie shot them.
- ODST's drop-pod sequences are handled specially: the locked opening stays in
  the theatre through the clouds reveal, while the shots you can actually look
  around in stay as immersive VR.
- `cutscene_theater_depth` in the F1 menu controls the stereo separation, and
  Flip Depth swaps the eyes if it reads inside-out.

## Halo: Reach — the black world is fixed

Selecting the sniper rifle turned exact rock and terrain surfaces solid black,
differently in each eye, and switching weapons brought them back. Reach was the
only title that rebuilt each eye's camera without committing it to the renderer
before drawing the world; Halo 3 and ODST already did. It now does the same.

Reach also gets its **native vehicle controls** back, so driving and flying use
the layout the game expects instead of the on-foot mapping.

## New controls

- **L3 + R3** recenters your VR space *and* opens/closes the F1 menu. Use it any
  time the world has drifted off-centre.
- **Y + B** works as the pause chord in ODST and Reach.

On Reach the left trigger and X are still swapped compared to Halo 3 and ODST,
so grenades sit on X.

## Rebuilt F1 menu

The settings menu was a single long scroll; it is now organised into categories
down a sidebar. The panel is locked in place with a grab handle at the top so it
cannot be knocked around by accident, and it is themed in Chief green. A short
welcome page appears once per launch and can be turned off with `show_welcome`.

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
Sharpening, HUD and weapon-alignment values regress the same way, and 0.3.1 adds
new menu and theatre settings that older files do not contain at all.

If you want your own tuning back, copy your old `halomccvr.cfg` somewhere safe
first, install the new one, then re-apply your preferences through the F1 menu.

## Known issues

- **Game Pass: MCC freezes about nine seconds on the first loading screen.** Not
  a crash, not the mod — it is MCC's own loop. Measured back to back on one PC:
  the Store edition stalls 9.0 seconds every launch, Steam stalls zero times.
  Anti-cheat and encrypted game content were both tested and ruled out. Wait it
  out; it clears by itself.
- Frame rate at exactly half your headset's refresh is your runtime's motion
  smoothing / ASW, not a mod bug. The log prints `app cadence` next to `panel`
  so you can see it. Turn motion smoothing off or lower `resolution_scale`.
- The right stick still turns you while the F1 menu is open.
- On Reach, character tags and navpoints are misplaced in 3D, and the
  `hud_curvature` and `hud_vertical_offset` settings have no effect.
- ODST's first captioned opening cutscene can be black. Skip that first scene
  once; do not repeatedly skip or you will miss the working drop sequence.
- ODST brightness stays at the game default.
- MCC can retain multiple title modules after switching games. If a level
  returns to the menu, fully close and restart MCC.
- Broader weapon, turret, passenger-gun, vehicle, co-op, headset and
  long-session coverage is still needed. This is an alpha.

## Verify your download (optional)

```text
ZIP      D139C7E7D507E114EB8F54A10D4FD1DAAC797D4722D9E5C29ADB969DBBB9B2AF
DLL      5B4A852C3175021AD433373BACB825BCC5D0EDFC52AF620A26C8A2C55F00BA64
Launcher 9F510506981B882BD571E892B88AD26951CA08948C7DADB031088060E918824A
Config   5847BF57E18133954C172103CD13F65560896AC198A119CC4BA1BE1202F36C2D
```

Windows or antivirus may warn about the unsigned files; that is expected for VR
mods. Allow the two binaries (or the `Halo_MCC_VR` folder) rather than disabling
security. Launch only through the included launcher, with anti-cheat disabled.

Source commit: `60f3929f63474459e51ce3d641fc14dc5d49529b`. Built Release x64 with
Halo 3, ODST and Reach enabled.
