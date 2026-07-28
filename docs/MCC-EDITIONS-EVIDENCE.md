# MCC edition evidence — Steam vs Microsoft Store / Xbox app

Measured on the dev box, 2026-07-28. Both editions installed side by side:

| Edition | Install root used by the launcher |
| --- | --- |
| Steam | `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection` |
| Microsoft Store / Xbox app | `N:\XBOX\Halo- The Master Chief Collection\Content` |

Both report build tag `2025.08.16.178512.1-Release`.

## The game modules are the same code

`halo3.dll`, `halo3odst.dll`, `haloreach.dll`, `halo1.dll`, `halo2.dll`,
`halo4.dll` and `groundhog.dll` all have **identical file lengths** and
**different SHA-256** between the two editions. A full byte diff explains the
difference completely:

```
haloreach.dll   13229016 bytes   2109 differing bytes in 17 clusters
  0x000001C0..0x000001C1   PE OptionalHeader.CheckSum
  0x00C99E0B..0x00C9DBD6   inside the certificate table

halo3.dll       11127768 bytes   2099 differing bytes in 17 clusters
  0x000001B8..0x000001B9   PE OptionalHeader.CheckSum
  0x00A98E0B..0x00A9CBD6   inside the certificate table
```

The PE `IMAGE_DIRECTORY_ENTRY_SECURITY` entry of both `haloreach.dll` copies is
offset `0x00C99E00`, size `15832` — so every non-checksum differing byte lies
inside the Authenticode signature. `TimeDateStamp` is `0x68A0EFE1` in both.

**Consequence:** every AOB signature, offset, struct layout, bone, marker and
tag meaning verified against a Steam module is equally valid on the Store
module. One cumulative build serves both editions, and an edition difference is
never an explanation for a signature or offset failing to match.

## What actually differs

Only the shipping executable and the surrounding install layout.

| | Steam | Microsoft Store |
| --- | --- | --- |
| Shipping exe | `MCC\Binaries\Win64\MCC-Win64-Shipping.exe` | `MCC\Binaries\Win64\MCCWinStore-Win64-Shipping.exe` |
| Exe size | 67179480 | 65333720 |
| Licensing | Steam | Xbox app / Gaming Services |
| Root marker | *(none)* | `MicrosoftGame.config`, `appxmanifest.xml` |

The DLL never scans the MCC executable with a signature. The only two places it
touches that image are structural and name-agnostic: `Input_ClaimXInputIat()`
walks the PE import table by DLL name/ordinal, and `InitExeRange()` computes the
image bounds from the PE headers. Neither is affected by the executable
differing between editions.

### Store package identity

`appxmanifest.xml` declares package family `Microsoft.Chelan_8wekyb3d8bbwe` with
two applications, both entry point `Windows.FullTrustApplication`:

- `HaloMCCShipping` — normal launch
- `HaloMCCShippingNoEAC` — *"Halo: MCC Anti-Cheat Disabled (Mods and Limited
  Services)"*

`MicrosoftGame.config` maps `HaloMCCShippingNoEAC` to
`MCC\Binaries\Win64\MCCWinStore-Win64-Shipping.exe`.

**`CreateProcess` on that executable can never work.** Measured 2026-07-28 with
no mod involved at all:

```
bare exe, no args, no injection   -> EXITED after 211 ms, exit code 0
our exact render args, no mod     -> EXITED after 223 ms, exit code 0
```

Exit code 0, no `Application Error` event, and MCC writes no log of its own: a
deliberate early self-exit, not a crash. The process has no package identity and
therefore no licence. There is **no** `CreateProcess` attribute that grants
identity — the full `PROC_THREAD_ATTRIBUTE_*` list in the Windows SDK
(`10.0.26100.0`, `WinBase.h`) contains nothing package-related, and
`PROC_THREAD_ATTRIBUTE_PACKAGE_FULL_NAME` does not exist. **Renaming the
executable to the Steam name therefore cannot fix this**, because the name was
never what failed.

The supported route is packaged activation, and it is what the launcher now
uses. Verified end to end on the installed candidate:

```
activation OK (helper pid 3692)          <- ActivateApplication returns S_OK
game process appeared, pid 31528         <- 4.2 s later
game loader ready; injecting             <- kernel32 present in module list
DLL injected OK into the running game    <- 22 ms after the readiness gate
game still running after 12s
```

Facts that follow, all measured:

- `IApplicationActivationManager::ActivateApplication` returns the pid of
  **`GameLaunchHelper.exe`**, not the game. The game process appears roughly
  4 seconds later and must be found by name.
- **Activation arguments are forwarded to the game verbatim.** The live process
  command line was
  `"...\MCCWinStore-Win64-Shipping.exe" -WINDOWED -ResX=3262 -ResY=2352`, so the
  Store edition gets the same VR render surface as Steam. (The Xbox app's own
  Play button passes **no** arguments — that command line is bare.)
- `OpenProcess(PROCESS_ALL_ACCESS)` on the packaged, full-trust game process
  succeeds from an ordinary medium-integrity process, so remote injection works.
- The process cannot be created suspended, so the launcher polls at 5 ms and
  gates injection on `kernel32.dll` appearing in the module list rather than
  injecting into the uninitialised-loader window.

Activating `HaloMCCShippingNoEAC` is the Store equivalent of running
`MCC-Win64-Shipping.exe` directly on Steam, so EasyAntiCheat is still never
started on either edition.

The running executable's real path is
`C:\Program Files\WindowsApps\Microsoft.Chelan_1.3528.0.0_x64__8wekyb3d8bbwe\MCC\Binaries\Win64\`,
**not** the `N:\XBOX\...\Content` path the launcher discovers. The content folder
is only a view; discovery and injection are independent, so this does not matter.

### Filesystem facts that matter for deployment

- The package content folder is **writable** — creating `Halo_MCC_VR` inside
  `...\Content\` succeeds (`Authenticated Users: Modify`).
- The two registered launchable executables (`MCCWinStore-Win64-Shipping.exe`
  and `mcclauncher.exe`) are **read-blocked** by Gaming Services even though
  their ACL grants `ReadAndExecute`; execute still works. Everything else,
  including all the game modules, reads normally. So tooling may `Test-Path`
  and execute the Store exe but must never try to hash or copy it.
- `HKLM\SOFTWARE\Microsoft\GamingServices\PackageRepository\Root\...` records the
  root as `\\?\N:\WindowsApps\Microsoft.Chelan_1.3528.0.0_x64__8wekyb3d8bbwe\`,
  which is **not readable**. Discovery must therefore keep walking up from the
  launcher's own folder rather than reading that path.

## Edition detection rule

`MicrosoftGame.config` sits beside the `MCC` folder in the Store package and in
no Steam install. The launcher treats an install as Store if the Store
executable name is present **or** that marker file is, so a Store install whose
executable was renamed to the Steam name — the community workaround this
replaces — is still detected as Store and is not asked to start Steam.

## Why Reach stayed stock on the Store edition — solved

Two logs from the **same DLL build** (`fdf6d6b`), one per edition:

```
Steam       Reach render cold preflight PASS: exact retail image, ...
Game Pass   Reach render cold preflight FAIL (backing-file-identity):
            main=0 inner=0 frustum=0; stock Reach remains active
```

Reach was detected fine on both (`detected supported title Halo: Reach
(haloreach.dll)`). What rejected it was `HashBackingFile()` in
`reach_render_preflight.cpp`: it SHA-256s the **whole file on disk** backing the
loaded module and compared it against a single pinned digest, the Steam one.

The Store copy the game actually loads is
`C:\Program Files\WindowsApps\Microsoft.Chelan_1.3528.0.0_x64__8wekyb3d8bbwe\haloreach\haloreach.dll`
— readable (only the launchable *executables* are read-blocked) and byte-diffed
against the Steam copy with the same result as every other module: **2109
differing bytes, all of them the PE checksum at `0x1C0` or inside the
certificate table at `0x00C99E00`. Zero code bytes.**

```
Steam           len=13229016  sha=738DD2D24EA3AEA1...  <- the pinned digest
Store (C: pkg)  len=13229016  sha=F9F39CF058FF28C2...  <- what the game loads
Store (N: view) len=13229016  sha=F9F39CF058FF28C2...  <- identical to it
```

So a whole-file hash is a hash of the *signature* as much as of the code, and it
rejected identical code purely for being signed by a different storefront.
`kReachRetailModuleSha256` is now the list of both signings of this one build.
That is not a loosening: image size, PE timestamp and every RVA in
`reach_render_logic.h` were already specific to this single MCC build, so an MCC
update invalidates the table either way.

Two diagnosis lessons are now built into the code:

- `BackingFileIdentity` used to mean both "file unreadable" and "wrong build".
  It is now split, with `BackingFileUnreadable` for the former.
- The failure log names the digest it actually read, so a rejected build
  identifies itself instead of only saying it did not match.

The reported "no controls" on Reach is consistent with this: the Store log shows
XInput hooked normally, so controls were not separately broken — Reach simply
stayed stock, which is what a failed render preflight does by design.
