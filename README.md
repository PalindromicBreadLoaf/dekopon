<h1 align="center">Dekopon</h1>

<p align="center">
  <img src="docs/dekopon_square.webp" alt="Dekopon logo" width="220">
</p>

<p align="center"><sub>Logo by <a href="https://stickymangorice.com/">StickyMangoRice</a></sub></p>

<b>Dekopon</b> is an open-source 3DS emulator for the Nintendo Switch based on Azahar.

Many many thanks to the Azahar team for creating such an amazing project in the first place.

<p align="center">
  <a href="https://ko-fi.com/palindromicbreadloaf">Support Dekopon development on Ko-fi</a>
</p>


# Project status

Currently, the project boots and runs games at mostly full speed (see the compatibility list for details: https://cryptpad.fr/sheet/#/2/sheet/view/PJKtoq0haezswSwH8qgEJkp7NWO57qNNL7cq04JsJAM/)\
\
Other features include:
- Full gyro support
- CIA installation support
- Switch software (and hardware) keyboard support
- Multiple screen layouts via R3 (Press the right stick)
- Virtual touch input
- Full button remapping support
- In-game menu accessible via '+' and '-'
- Cheat, mod (LayeredFS), and texture-pack support
- System language/region toggle
- Arctic Base/Azahar Setup Tool support
  - It should be noted that Streetpass and Spotpass should not be used in games.
  - There may be a crash related to trying to access these features, so do try to avoid them.
- Full 3D support via Nintendo Labo VR Kit/Virtual Boy for Nintendo Switch
- (Virtual) Cartridge insertion support
- Resolution Upscaling
- Amiibo support via .bin amiibo images placed in `amiibo`
- Camera support via static images placed in `camera`
- Loading ROMs via USB Mass Storage
- In-built auto-updater
- Save management
- Save states
- And other things I'm probably forgetting.

Features currently in the pipeline are:
- More performance improvements
- Streetpass/spotpass (If even possible)

Feel free to request more features, but do note they may or may not be implemented due to either feasibility or time.

# Installation

Installation is as simple as downloading the release nro from the [releases](https://github.com/PalindromicBreadLoaf/dekopon/releases) page
and copying it to your SD card in your standard homebrew location (probably /switch).

Your legally acquired ROMs go in `/switch/dekopon/roms/`
(This can be changed in settings)

# Cheats, mods and texture packs

All three use the same folder layout as desktop Azahar/Citra, rooted at the dekopon directory on
your SD card (`/switch/dekopon/` by default). In every path below, `<TITLE_ID>` is the game's Title ID
in uppercase (e.g. `00040000000EC800`). If you have changed the default path, please reflect that in any of the directories below.

## Cheats

- Put a cheat file at `/switch/dekopon/cheats/<TITLE_ID>.txt`. The format is the standard Gateway / Action Replay format.
- Open the in-game menu (`+` and `-`) to switch individual cheats on and off while the game runs.
  Your choices are written back to the cheat file, so they persist across launches.

## Mods

Place mod files under `/switch/dekopon/load/mods/<TITLE_ID>/`:

Mods are applied when the game boots.

Currently, selecting a mod from a list is not supported. Be sure that the folders under titleID are
`romfs` `exefs` and/or `exheader.bin`

## Texture packs

- Loading: drop a pack in `/switch/dekopon/load/textures/<TITLE_ID>/`, then enable
  **Custom Textures** in the in-game menu (`+` and `-`). This toggle applies immediately and is
  remembered. For large packs, `preload_textures = true` loads the whole pack at boot to avoid 
  in-game hitching, at the cost of more memory. This may run you out of RAM depending on the texture pack size.
  It's best to use no more than 1080p textures since you'll run out of RAM fast using 4K textures for basically zero visual gain.
  Also, note that you may run into a crash trying to use too many custom textures and higher resolutions than 1x. There are
  some safeguards in place to prevent crashes, but it may still happen.
- Dumping): set `dump_textures = true` in the `[Utility]` config section. Textures
  the game uses are written to `/switch/dekopon/dump/textures/<TITLE_ID>/`. This setting takes
  effect on the next launch. (You should also really just do this on PC. Performance will be degraded
  using this option.)

# Save management

Press `+` on a title in the library to open its details panel, then `Y` for the save tools.
Inside that panel:

- `X` makes a new backup of the save currently on the emulated card.
- `A` restores the highlighted backup over the emulated save.
- `Y` deletes the highlighted backup.
- Left/Right switches between save data and extdata, if the title has extdata.

Backups are written to the SD card in Checkpoint's layout, so a real 3DS running Checkpoint
reads them directly and vice versa:

```
sdmc:/3ds/Checkpoint/saves/0x<UNIQUEID> <Title>/<backup name>/
sdmc:/3ds/Checkpoint/extdata/0x<EXTDATAID> <Title>/<backup name>/
```

**Games that use secure values (notably Pokemon games) will not work copying saves from
Dekopon -> 3DS.** Going the other way is fine.

# Loading games from USB storage

Dekopon mounts USB mass storage automatically, so drives plugged into the dock (or into the console)
show up without any extra setup. Yyou'll get a notice naming the mount point when one is inserted or
the opposite if it is removed.

FAT12/16/32, exFAT, NTFS, and EXT2/3/4 (recommended) are the supported filesystems.

To use a ROM folder on an external drive, go to the Paths tab and set **second ROM folder** to a
folder on said drive.

Please only use the second folder rather for USB. This folder is rescanned asynchronously on each 
boot so it's okay to be swapped around constantly.

# Updating

Dekopon can update itself. This is found under **Settings > General**.

- **Check for Updates** asks GitHub what the newest release is and offers to install it, or
  says if you are on the latest version.
- **Update Channel** picks between *Stable* and *Prerelease*. Stable releases are less likely to
  have bugs, but may be behind in features.
- **Release Notes** shows the notes for the build you're on.
- **Show What's New** controls whether you get a pop-up on boot after a new update explaining what
  has changed.

Obviously, this needs a working internet connection.

# Build instructions

## Required packages
The current build requires DevkitPro. Please install from here [DevkitPro Install](https://devkitpro.org/wiki/Getting_Started)
### DevkitPro Packages
- switch-dev
- switch-freetype
- switch-bzip2
- switch-libpng
- switch-zlib
- switch-curl
- switch-ntfs-3g and switch-lwext4 *(optional. Without them USB drives mount FAT and exFAT only\*\*)*
- switch-mesa *(only for the legacy GLES backend\*)*
### System Packages
- cmake
- git

\*The default GPU backend is Vulkan via [NXVK](https://github.com/PalindromicBreadLoaf/nxvk)
NXVK and switch-mesa cannot be included simultaneously, so the driver must be chosen at buildtime.
NXVK is default since it supports both GL and Vulkan, however, it is possible to swap back to 
switch-mesa if so desired.

\*\*Both are detected at configure time. If they are missing, configuring prints
`libusbhsfs: FAT and exFAT support only` and NTFS/EXT drives simply won't mount in that build.

## 1. Clone the repository
```shell
git clone --recursive https://github.com/PalindromicBreadLoaf/dekopon.git
cd dekopon
```

## 2. Build NXVK
NXVK has it's own build documentation that lives in the [NXVK repository](https://github.com/PalindromicBreadLoaf/nxvk).
Please follow that and create a libnvk.a file. Once you've done that you can return here.

Configuring looks for `switch/build/cross/src/nouveau/vulkan/libnvk.a` under `externals/nxvk`.
If you built nxvk in-place, it should be found automatically.

## 3. Configure
```shell
cmake -S . -B build/switch \
    -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

## 4. Build
```shell
cmake --build build/switch --target citra_switch_nro -j$(nproc)
```

The output nro should be located in build/switch/src/citra_switch/dekopon.nro

# How can I contribute?

### Pull requests

If you want to implement a change and have the technical capability to do so, we would be happy to accept your contributions.

If you are contributing a new feature, it is highly suggested that you first make a Feature Request issue to discuss the addition before writing any code. This is to ensure that your time isn't wasted working on a feature which isn't deemed appropriate for the project.

After creating a pull request, please don't repeatedly merge `master` into your branch. A maintainer will update the branch for you if/when it is appropriate to do so.

### Compatibility reports

Do not ever contact the upstream Azahar project about any issues regarding this port. 
They have no relation to this project whatsoever and do not wish to deal with random issues regarding it.
Please only create issues in this repository regarding bugs found here unless they are directly applicable upstream and also happen 
the exact same as here.

To do so, simply read https://github.com/azahar-emu/compatibility-list/blob/master/CONTRIBUTING.md and follow the instructions.

Contributing compatibility data helps more accurately reflect the current capabilities of the emulator, so it would be highly appreciated if you could go through the reporting process after completing/playing a game.
