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
`MCC\Binaries\Win64\MCCWinStore-Win64-Shipping.exe`. **Starting that executable
directly is the Store edition's own anti-cheat-off path**, exactly as starting
`MCC-Win64-Shipping.exe` directly is Steam's. The launcher therefore uses the
same suspended-create + `CreateRemoteThread`/`LoadLibraryW` injection for both,
and EAC is never started on either.

`PROC_THREAD_ATTRIBUTE_PACKAGE_FULL_NAME` does **not** exist in the Windows SDK
(checked `10.0.26100.0`, `processthreadsapi.h` and `WinBase.h`), so there is no
supported `CreateProcess` attribute for granting package identity. Nothing in
the mod needs it.

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

## Open

Halo: Reach was reported not to hook, and to have no controls, on the Store
edition under the rename workaround. Because the modules are byte-identical,
that cannot be a code, signature or offset difference. It is unexplained and is
the next thing to investigate; do not attribute it to the edition's binaries.
