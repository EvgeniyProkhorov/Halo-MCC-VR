# FP Vehicle Update — MCC VR Alpha 0.3.3

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

---

# ⚠️ You have to set up your own seats

First-person vehicles ship with a starting camera position for every seat in
every game. **That is a starting point, not a finished setting.** Everyone's
height, play space and headset sit differently, so a seat that looks right for
me will be too low, too far forward, or too far back for you.

**This is normal, it is expected, and it is a one-time job per seat:**

1. Get into the seat you want to fix — driver, passenger, gunner or turret.
2. Press **F1** and open the **Vehicles** category.
3. Move **Seat forward (m)**, **Seat height (m)** and **Seat left / right (m)**
   until it feels right.
4. Get out. It saves to `halomccvr.cfg` by itself.

While you're **sitting in a seat**, those sliders move **that seat alone**. A
vehicle's driver, its passengers and its gunner each remember their own
position, in each game — so do the ones you actually use and ignore the rest.

While you're **on foot**, the same sliders set the shared starting point that
every seat you haven't adjusted follows. That one moves all three games at once,
which is handy for a quick overall height nudge. If you ever push it somewhere
bad, a **Reset the universal trim** button appears under the sliders and puts it
back to the shipped value.

---

## First-person vehicles, now in all three games

0.3.2 put you inside Halo 3's vehicles. **0.3.3 does the same for ODST and
Halo: Reach**, so the whole collection is covered.

You sit in the seat instead of floating behind the vehicle. Your hands are on
the wheel and stay there while the vehicle moves. Your own body is hidden from
your view (other cameras still see you). The seat is anchored to the vehicle
itself, so bumps, jumps and barrel rolls carry you with them instead of leaving
your head behind.

**Every seat is placed, not just the driver's:**

- **Reach** — Warthog (plus the chaingun, gauss and rocket variants), Scorpion,
  Wraith and its gunner, Ghost, Revenant (both seats), Banshee, Space Banshee,
  Sabre, Falcon (pilot and both door seats), Mongoose (both seats), the Shade
  plasma and flak turrets, the plasma turret and machinegun emplacements, the
  forklift, the pickup, the cargo truck and the cart.
- **ODST** — Warthog driver and passenger, Scorpion, Prowler gunner, Hornet,
  Chopper, the mounted machinegun turret, and the Covenant walk-up Shade.
- **Halo 3** — everything from 0.3.2, plus the fixes below.

Each of those seats has its own line in `halomccvr.cfg` and its own sliders in
the F1 menu, so adjusting the Warthog's passenger seat never disturbs its
driver.

### Turrets sit still and shoot where you're pointing

Mounted turrets in **Halo 3 and ODST** used to wobble up and down, constantly
chasing your crosshair and never settling. That's gone — they hold still now.

The reticle also rides the **gun barrel** instead of your hand. A Shade turret
physically can't whip around as fast as you can flick your wrist, so the
crosshair used to promise a direction the gun hadn't reached yet and shots went
somewhere else. Now the crosshair *is* the gun: what you see is where the round
goes.

### Passenger guns line up at every range

Riding shotgun in a Warthog, your shots now leave the same line you're aiming
down, at any distance. Previously the shot line and the crosshair only crossed
at one specific range, so anything nearer or further than that missed. This is
fixed in **Halo 3, ODST and Reach**.

### Getting in faces the right way

In Halo 3, entering a seat now turns you to face the vehicle's nose whether or
not you use **View follows the vehicle**. Before, it took its heading from
wherever the entry animation happened to be pointing mid-swing, so you'd often
land in the seat looking sideways.

### Your seat settings can't get wiped any more

There was a nasty one here. A vehicle the mod couldn't identify used to write
its adjustments into the **shared** setting that all three games fall back on —
so tuning one Reach seat could silently move your Halo 3 and ODST seats too, for
days, with nothing to tell you why.

Unrecognised vehicles now keep their own line. The shared setting is left alone,
there's a one-click reset for it, and the log now says so out loud if it ever
differs from the shipped value.

The sliders themselves also behave the same way in all three games now, and a
seat's reset button returns it to its own built-in starting point rather than to
the shared one.

## Brightness works in ODST and Reach

The `game_brightness` slider only ever moved Halo 3. One slider now moves all
three games.

There is also a **Steady brightness in the seat** option for ODST: looking down
at a dark dashboard fills the game's light meter with a big dark surface, which
makes its auto-exposure ramp the whole scene. This holds it steady while you're
seated.

## No more loading a level twice

If you've had the game bounce you back to the main menu on the first load of a
level, and then work fine on the second try — that's fixed. The mod no longer
touches a game while its level is still loading. It waits until the level is
actually running before it does anything at all.

Related: in Reach, pausing and unpausing while in a vehicle used to make the mod
think you'd left and re-entered the seat, which recentered your play space, and
could drop you out to the menu a couple of seconds later. A pause is now treated
as a pause.

## Faster

- **ODST gets into VR about two seconds sooner.** Its startup was doing two
  waits back to back that had no reason not to overlap.
- **The startup scan is about 7× faster** in all three games — the pass that
  finds the game's own code went from roughly 37 ms to 5.5 ms per scan. Nothing
  about the checking was relaxed to get there; it just stopped testing positions
  that could never match.

## Install

**Replace all three files, including `halomccvr.cfg`. Do not keep your old
config.**

There is no migration step, so any setting your old file doesn't contain
silently falls back to a built-in default rather than the shipped value. The
worst offender is `fit_desktop_window`, whose built-in default is off while the
shipped config turns it on — keeping an old config can cap your headset frame
rate. 0.3.3 adds the entire first-person vehicle block, including a starting
position for every ODST and Reach seat, which an older config has no equivalent
for at all.

If you want your own tuning back: copy your old `halomccvr.cfg` somewhere safe,
install the new one, then re-apply your preferences through the F1 menu.

Full install steps are in `MANUAL-README.txt` inside the ZIP and in the
[README](https://github.com/pancreations/Halo-MCC-VR#install). Both the Steam and
the Microsoft Store / Xbox app (Game Pass) editions use this same download, and
neither needs any game file renamed.

### Required MCC settings

| Setting | Value |
| --- | --- |
| Video > Max Frame Rate | 120 |
| Video > V-Sync | Off |
| Halo 3 > Field of View | 120 |
| ODST > Look Sensitivity | Maximum |
| ODST > Look Acceleration | Off |
| MCC FSR | Off |

### Verify your download

```text
ZIP      C1CC84C1F2278E622F0A439E4DC3791A4E2264DEE8F1F71E48D61346D3AFE69D
DLL      44A82E28B65F8FD6D0A52FF2C87A55C37EFC8B5888DEE6836DEE9AEF89DE026D
Launcher 930BEA232BFC3F8010BC2B385834DEBF796CD3DBEC02ECD0E8475E0DE8A72CE6
Config   E941BC189B57B9ED11EB62DCF8D6AE1C5787936074C2358EB6AF99A377C97975
```

Windows security software may flag unsigned injection-based VR mods. Download
only from the official release, verify the hashes, and allow only the two
release binaries rather than disabling security software globally.

## Still broken / still coming

- **Vehicle seats need your own adjustment.** See the notice at the top.
- **Reach passenger seats don't draw your floating hands yet.** The seat, the
  aim and the shooting all work — the hands are still being chased down.
- Microsoft Store / Game Pass: MCC freezes for about nine seconds on the first
  loading screen, every launch. That's MCC's own loop, not the mod, and not a
  crash. Wait it out.
- The right stick still turns you while the F1 menu is open.
- Reach: character tags and navpoints are misplaced in 3D.
- Reach: `hud_curvature` and `hud_vertical_offset` have no effect.
- ODST: the first captioned opening cutscene can be black. Skip that one scene
  once — don't keep skipping or you'll miss the working drop sequence.
- MCC can hold on to several game modules after you switch titles. If a level
  drops you back to the menu, fully close and restart MCC.
- Broader co-op, headset and long-session coverage is still needed.

**Next up: Halo 4.**
