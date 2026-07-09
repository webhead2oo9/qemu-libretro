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
- **3D acceleration** on the win64 build via
  [qemu-3dfx](https://github.com/kjliew/qemu-3dfx) pass-through — Glide
  and OpenGL games render on the host GPU (see *3D acceleration* below).
- **User-mode networking**: `-netdev user` (slirp) is built in.
- Performance work throughout: frame presentation, audio, and input no
  longer block the emulator on the frontend (fixes audio crackle), WHPX
  guests get hardware dirty-page tracking (idle desktops stop burning
  CPU on redraws) and far fewer hypercalls on emulated string I/O.
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

On Windows 10 1903 or newer the core also uses hardware dirty-page
tracking, so an idle guest desktop costs almost no CPU. Older Windows
versions still work; they just re-render the display every tick.

### Networking

User-mode NAT networking is built in:

``` sh
qemu-system-x86_64 ... -netdev user,id=net0 -device rtl8139,netdev=net0
```

Use `pcnet` for Windows 9x guests (in-box driver), `rtl8139` for
2000/XP, or `e1000` for newer systems.

### Audio buffer

If audio crackles, enlarge the core's audio ring with `out.buffer-length`
(microseconds; the default is ~23 ms):

``` sh
-audiodev libretro,id=snd0,out.buffer-length=46440
```

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

## 3D acceleration (qemu-3dfx pass-through)

The win64 core includes the Glide and OpenGL pass-through devices from
[qemu-3dfx](https://github.com/kjliew/qemu-3dfx). Games inside the guest
render on the host GPU; finished frames are read back and presented
through the frontend like any other core output — no extra window
appears.

**What it accelerates:** the 3D APIs of the late-90s/early-2000s —
**Glide** (3dfx Voodoo), **OpenGL**, and **Direct3D** (bridged in-guest by
a D3D-to-OpenGL wrapper such as WineD3D). It is *application-level* API
pass-through, not a virtual GPU: the Windows desktop itself still draws on
the emulated Cirrus/VGA card, and modern APIs (DX10+, Vulkan, core-profile
GL) are not bridged. This is an XP/9x-era games feature.

**Easiest path — the EmuVR VM Wizard.** The wizard's *3D acceleration kit*
builds a setup CD that installs the correct files for the guest OS and
writes the host-side config for you. The rest of this section documents
what that automates, for hand setup.

### Installing the guest wrappers

The wrappers are **signature-locked to the core build** (they check a
build stamp against the pass-through device and fail *silently* if it
doesn't match — no error, 3D just never engages). Use the wrappers that
ship with your core (the wizard bundles the matching set); if you build
your own core, rebuild the wrappers from the same qemu-3dfx checkout.

**Windows 95 / 98 / ME:**

- Copy `FXMEMMAP.VXD` to `C:\WINDOWS\SYSTEM\`. This kernel shim is opened
  by *every* wrapper — without it both Glide and OpenGL fail silently.
- **Glide:** copy `GLIDE.DLL`, `GLIDE2X.DLL`, `GLIDE3X.DLL` to
  `C:\WINDOWS\SYSTEM\`, then pick the "3dfx Glide" renderer in-game.
- **OpenGL:** put the wrapper `OPENGL32.DLL` in the *game's own folder*
  (never a system folder).
- Copying to a directory on Win9x must name the file explicitly
  (`copy A.VXD C:\WINDOWS\SYSTEM\A.VXD`) — a trailing-backslash
  destination is silently rejected by `COMMAND.COM`.

**Windows 2000 / XP:**

- Copy `FXPTL.SYS` to `C:\WINDOWS\system32\drivers\` and run `INSTDRV.EXE`
  once. This creates the auto-start `MAPMEM` service that *all*
  pass-through (OpenGL **and** Glide) needs; it's idempotent.
- **OpenGL:** put the wrapper `OPENGL32.DLL` in the *game's own folder*,
  not `system32` (there it would shadow the host driver).
- **Glide:** `GLIDE*.DLL` alongside the game (or in `system32`).
- **Direct3D:** drop a WineD3D set (`ddraw`/`d3d8`/`d3d9` + `wined3d`)
  into the game's folder. This is the most-tested D3D path.

### Host-side config (next to the `.qemu_cmd_line`)

The pass-through devices read their config from the content directory (the
core `chdir`s there), so these files go **beside your `.qemu_cmd_line`**,
not in the RetroArch folder:

- `mesagl.cfg` (OpenGL) / `glide.cfg` (Glide).
- **Era games need `ExtensionsYear,1998` in `mesagl.cfg`.** Unfiltered,
  a modern GPU's giant `GL_EXTENSIONS` string overflows fixed buffers in
  90s games and they crash on GL init (Quake II is a classic offender).
  The device log shows `Year ALL` when the cfg wasn't found — use that to
  confirm placement.
- **WineD3D needs the opposite** — modern GL — so *omit* the
  `ExtensionsYear` cap for Direct3D titles. Keep per-game configs.
- **Frame pacing:** the core disables host vsync on the offscreen swap
  chain, so games that relied on vsync as their speed limit run uncapped.
  Cap per game with `FpsLimit,60` in `glide.cfg`/`mesagl.cfg`.

### Limitations

- The asynchronous readback adds one frame of display latency.
- Save states are unreliable while 3D pass-through is active.

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
