HALO MCC VR - MANUAL SETUP
===========================

Supports Halo 3, Halo 3: ODST and Halo: Reach. Reach is new in 0.3.0.

On Reach the left trigger and X are swapped compared to Halo 3 and ODST, so
grenades sit on X.

Works with both the Steam copy of MCC and the Microsoft Store / Xbox app
(Game Pass) copy. You do NOT need to rename anything - if you renamed
MCCWinStore-Win64-Shipping.exe to make an older build work, rename it back.

There is no installer, uninstaller, deploy, or restore script.

INSTALL
-------
1. Open the main game folder.

   Steam:            right-click Halo: The Master Chief Collection and choose
                     Manage > Browse local files.
   Microsoft Store:  in the Xbox app, click the "..." next to Halo: MCC and
                     choose Manage > Files > Browse... Then open the Content
                     folder inside it.

   The correct folder is the one that contains the MCC folder. On the
   Microsoft Store that is the Content folder, next to MicrosoftGame.config.

2. In that folder, create a folder named exactly:

   Halo_MCC_VR

3. Copy these three release files into it:

   halo3xr.dll
   halo3xr_launcher.exe
   halomccvr.cfg

4. Make SteamVR the default OpenXR runtime, then run halo3xr_launcher.exe.

   Steam:            start Steam and SteamVR first.
   Microsoft Store:  start SteamVR, and be signed in to the Xbox app. Steam
                     itself does not have to be running.

   On the Microsoft Store the launcher starts the game for you through the
   Xbox app - do NOT start Halo: MCC first. The game takes a few seconds
   longer to appear than on Steam; that is normal, just wait.

The final path must be one of:

   Halo The Master Chief Collection\Halo_MCC_VR\halo3xr_launcher.exe
   ...\Halo- The Master Chief Collection\Content\Halo_MCC_VR\halo3xr_launcher.exe

Do not put the files loose in the main MCC folder. Launch only through the
included launcher and never use the mod in anti-cheat-enabled matchmaking.

The launcher writes the edition it detected into halo3xr_launcher.log, so if it
ever picks the wrong one that line tells you.

UPDATE - REPLACE YOUR CONFIG
----------------------------
Close MCC completely, then replace ALL THREE files: halo3xr.dll,
halo3xr_launcher.exe AND halomccvr.cfg.

Replace the config. Do not keep your old one. This is different from previous
updates, which told you to keep it.

0.3.0 adds settings that older config files do not contain, and there is no
migration step: any setting your old file is missing silently falls back to a
built-in default instead of the shipped value. The most visible casualty is
fit_desktop_window, whose built-in default is off while the shipped config turns
it on - keeping an old config can therefore cap your headset frame rate.
Sharpening, HUD and weapon-alignment values regress the same way.

If you want your own tuning back, copy your old halomccvr.cfg somewhere safe
first, install the new one, then re-apply your preferences through F1.

SETTINGS
--------
halomccvr.cfg ships in the ZIP as a tuned configuration, not bare defaults.
Every value has a description, default, and allowed range. Edit it with MCC
closed or use the in-game F1 menu.

If the file is missing the game regenerates it, but a regenerated file contains
bare built-in defaults rather than the shipped tuning - so keep a copy of the
shipped one.

Required MCC settings:

   Video Max Frame Rate:            120
   Video V-Sync:                    Off
   Halo 3 Field of View:            120
   ODST Look Sensitivity:           Maximum
   ODST Look Acceleration:          Off
   MCC FSR:                         Off

VERIFY
------
For a published release, compare the DLL, launcher, and ZIP hashes with the
official GitHub release page. For a local build, use CANDIDATE-MANIFEST.json in
that unique package. A local candidate is not headset-accepted merely because
it was built from accepted source.

If two PCs behave differently, first compare their installed hashes and
halomccvr.cfg files, confirm SteamVR is the default OpenXR runtime on both, and
fully close every MCC process before relaunching.

Windows security software may warn on unsigned injection-based VR mods. Download
only from the official GitHub release, verify the hashes, and allow only the DLL
and launcher rather than disabling security software globally.

REMOVE
------
Close MCC completely, then delete only the dedicated Halo_MCC_VR folder you
created. Never delete the main Halo The Master Chief Collection folder.
