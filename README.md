# qemu-libretro

This is a libretro QEMU core, allowing to run VMs in a libretro frontend.

This fork of [io12/qemu-libretro](https://github.com/io12/qemu-libretro)
focuses on older RetroArch frontends:

- Fixes content loading on RetroArch 1.7.5 — the core previously read the
  game path after the frontend had freed it.
- Error messages now display on older frontends (fallback from
  `SET_MESSAGE_EXT`, which needs RetroArch 1.9+, to the original API).
- **Hardware virtualization on Windows (WHPX)** is compiled into the win64
  build: near-native guest CPU speed instead of software emulation.
- Builds with `-O2` (faster emulation) and fixes building with plain
  mingw-w64.

## Setup: firmware

QEMU needs its firmware files (BIOS, VGA ROMs, PowerPC OpenBIOS). Copy the
files from this repo's `pc-bios/` folder into your RetroArch **system**
directory under a `qemu` subfolder (i.e. `system/qemu`). Without them, VMs
fail with `could not load PC BIOS 'bios-256k.bin'`.

## Usage

You can open a `.iso`, `.img`, `.qcow`, or `.qcow2` file,
and it will be run with the command

``` sh
qemu-system-x86_64 -audiodev libretro,id=snd0 -machine pcspk-audiodev=snd0 -device AC97,audiodev=snd0 PATH_TO_OPENED_FILE
```

If you need more customization, such as increasing memory or using a different architecture than x86_64,
you can provide your own QEMU command in a `.qemu_cmd_line` file.
It uses the [`g_shell_parse_argv()`](https://docs.gtk.org/glib/func.shell_parse_argv.html) function and not an actual shell to parse the command, so it will only supports simple shell features.
The core interprets relative paths as relative to the directory containing the `.qemu_cmd_line` file.

**Note:** commands in a `.qemu_cmd_line` file run exactly as written — the
audio flags shown above are *not* added automatically, so include
`-audiodev libretro,id=snd0 -machine pcspk-audiodev=snd0 -device …,audiodev=snd0`
yourself if you want sound.

### Hardware virtualization (Windows)

Add this to the start of your command for near-native CPU speed:

``` sh
qemu-system-x86_64 -accel whpx -accel tcg ...
```

This uses the Windows Hypervisor Platform (enable it under *Windows
Features* if it isn't already) and falls back to software emulation (`tcg`)
when unavailable. x86 guests only.

### Choosing a video card

The core outputs a 32-bit software framebuffer. Recommendations:

- **MS-DOS**: the default (`std` VGA) — best VESA mode support.
- **Windows 3.x / 98 / 2000 / XP**: `-vga cirrus` — these ship a Cirrus
  driver in the box, giving real resolutions and color depths with no driver
  installs. Needs `vgabios-cirrus.bin` (in `pc-bios/`).
- Avoid 16-bit color with `-vga vmware`, `bochs-display`, or `virtio` on
  this core: those devices hand their framebuffer to the frontend without
  conversion and will render wrong colors at anything but 32-bit. `std` and
  `cirrus` are safe at every depth.

## Examples

### KolibriOS

1. Download the [live CD image](https://builds.kolibrios.org/en_US/latest-iso.7z).
2. Open `kolibri.iso` with the QEMU core.

### Windows XP

1. Create an empty disk image with
   ``` sh
   qemu-img create -f qcow2 winxp_disk.qcow2 12G
   ```
   Change `12G` to the size of the disk you want.
   This core doesn't provide a way to run `qemu-img`, so you will need to make the disk image separately and copy it into the games folder.
2. Copy the installer ISO to `winxp_installer.iso` in the games folder.
3. Create a text file `winxp.qemu_cmd_line` in the games folder containing the command
   ``` sh
   qemu-system-x86_64 -accel whpx -accel tcg -vga cirrus -hda winxp_disk.qcow2 -cdrom winxp_installer.iso -boot d -m 1G
   ```
4. Load `winxp.qemu_cmd_line` with the QEMU core and complete the installation.
5. Modify `winxp.qemu_cmd_line` to contain
   ``` sh
   qemu-system-x86_64 -accel whpx -accel tcg -vga cirrus -hda winxp_disk.qcow2 -m 1G
   ```
   Change the `1G` to a different amount of RAM if desired. The `winxp_installer.iso` file isn't needed anymore and can be deleted.
6. Load `winxp.qemu_cmd_line` with the QEMU core to boot into Windows XP.

### Windows 11

1. Create an empty disk image with
   ``` sh
   qemu-img create -f qcow2 win11_disk.qcow2 64G
   ```
   Change `64G` to the size of the disk you want.
   This core doesn't provide a way to run `qemu-img`, so you will need to make the disk image separately and copy it into the games folder.
2. Copy the installer ISO to `win11_installer.iso` in the games folder.
3. Create a text file `win11.qemu_cmd_line` in the games folder containing the command
   ```sh
   qemu-system-x86_64 -accel whpx -accel tcg -hda win11_disk.qcow2 -cdrom win11_installer.iso -boot d -m 1G --cpu Skylake-Client-v3
   ```
4. Load `win11.qemu_cmd_line` with the QEMU core and complete the installation.
5. Modify `win11.qemu_cmd_line` to contain
   ```sh
   qemu-system-x86_64 -accel whpx -accel tcg -hda win11_disk.qcow2 -m 1G --cpu Skylake-Client-v3
   ```
   Change the `1G` to a different amount of RAM if desired. The `win11_installer.iso` file isn't needed anymore and can be deleted.
6. Load `win11.qemu_cmd_line` with the QEMU core to boot into Windows 11.

### MacOS 9.2

1. Create an empty disk image with
   ``` sh
   qemu-img create -f qcow2 mac9_disk.qcow2 2G
   ```
   Change `2G` to the size of the disk you want.
   This core doesn't provide a way to run `qemu-img`, so you will need to make the disk image separately and copy it into the games folder.
2. Copy the installer ISO to `mac9_installer.iso` in the games folder.
3. Create a text file `mac9.qemu_cmd_line` in the games folder containing the command
   ```sh
   qemu-system-ppc -boot d -m 512 -L pc-bios -hda mac9_disk.qcow2 -drive file=mac9_installer.iso,format=raw,media=cdrom -M mac99,via=pmu
   ```
4. Load `mac9.qemu_cmd_line` with the QEMU core and complete the installation.
   Open "Drive Setup" in the "Utilities" folder, and initialize the volume called "not initialized".
   Then open the "Mac OS Install" program and complete the installation.
5. Modify `mac9.qemu_cmd_line` to contain
   ``` sh
   qemu-system-ppc -boot c -m 512M -L pc-bios -hda mac9_disk.qcow2 -M mac99,via=pmu
   ```
   Change the `512M` to a different amount of RAM if desired. The `mac9_installer.iso` file isn't needed anymore and can be deleted.
6. Load `mac9.qemu_cmd_line` with the QEMU core to boot into MacOS 9.2.
   Mac OS 9 needs `openbios-ppc` and `qemu_vga.ndrv` from `pc-bios/` in your
   RetroArch `system/qemu` folder (see *Setup: firmware* above).

## Compile instructions (Ubuntu)

```sh
sudo apt-get install build-essential python3 python3-venv ninja-build flex bison zlib1g-dev
git clone --recursive https://github.com/webhead2oo9/qemu-libretro
cd qemu-libretro
mkdir build
cd build
CFLAGS="-O2 -Wno-error -Wno-nested-externs -Wno-redundant-decls" ../configure \
    --without-default-features \
    --glib=internal \
    --zlib=internal \
    --disable-pie \
    --enable-fdt=internal \
    --disable-modules \
    --disable-plugins \
    --enable-libretro \
    --audio-drv-list=libretro \
    --disable-sdl \
    -Dwrap_mode=forcefallback
make -j$(nproc) libqemu_libretro.so
```

## Cross-compiling the Windows core (from Ubuntu or WSL)

```sh
sudo apt-get install build-essential gcc-mingw-w64-x86-64 ninja-build flex bison python3-venv python3-pip pkgconf git
git clone --recursive https://github.com/webhead2oo9/qemu-libretro
cd qemu-libretro
(cd zlib && ./configure)
mkdir build
cd build
CFLAGS="-O2 -Wno-error -Wno-nested-externs -Wno-redundant-decls" ../configure \
    --target-list=x86_64-softmmu \
    --without-default-features \
    --glib=internal \
    --zlib=internal \
    --disable-pie \
    --enable-fdt=internal \
    --disable-modules \
    --disable-plugins \
    --enable-libretro \
    --audio-drv-list=libretro \
    --disable-sdl \
    -Dwrap_mode=forcefallback \
    --cross-prefix=x86_64-w64-mingw32- \
    --extra-ldflags=-static \
    --enable-whpx
make -j$(nproc) libqemu_libretro.dll
```

Drop the resulting DLL into RetroArch's `cores` folder as
`qemu_libretro.dll`. Add more targets to `--target-list` (e.g.
`ppc-softmmu` for Mac OS 9) if you need them.

For help compiling for other platforms, see the [CI build script](libretro-gitlab-build.sh).
