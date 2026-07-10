# AGENTS.md

## Project intent

This is a QEMU 9.0-derived libretro core, not a general-purpose upstream QEMU
checkout. The primary target is x86 guests in old RetroArch frontends,
especially RetroArch 1.7.5 as embedded by EmuVR. Preserve that compatibility
when changing libretro APIs: newer frontend APIs need an old-API fallback.

The Windows x64 release is the feature-complete build. It includes WHPX,
TCG fallback, user-mode slirp networking, and the qemu-3dfx Glide/OpenGL
pass-through. Do not assume a change that works in standalone QEMU works under
the libretro frontend/threading lifecycle.

Read `README.md` for checked-in user-facing behavior. This working copy may
also contain ignored local planning notes such as `ROADMAP.md` and
`docs/roadmap/`; use them for context when present, but do not assume they
exist in a fresh clone or add them without the user's direction.

## Important areas

- `ui/libretro.c`: libretro API, lifecycle, frame/audio/input handoff, core
  options, content parsing, and disk control.
- `ui/libretro-3dfx.c`, `hw/3dfx/`, `hw/mesa/`: Windows qemu-3dfx integration.
- `include/ui/libretro-vars.h`: cross-thread core-option overrides used by 3D.
- `meson.build`, `meson_options.txt`, `configure`: feature and target wiring.
- `libretro-gitlab-build.sh`: canonical multi-platform CI build and release
  artifact naming. It deletes `build/` and may run package-manager upgrades;
  inspect it rather than invoking it casually on a development machine.
- `tests/retro-host/retro_host.c` (local-only when present): Windows minimal
  frontend used for lifecycle, QMP, core-option, frame-dump, and disk-control
  regression tests.
- `pc-bios/`: runtime firmware copied to RetroArch's `system/qemu` directory.

`libretro-common` and several `roms/`/`tests/` directories are submodules.
Initialize them before configuring and do not edit or advance their commits
unless the task explicitly requires it.

## Checkout and prerequisites

Shell scripts and `configure` must have LF endings. Run clone and submodule
commands with Linux Git from the Ubuntu/WSL shell; submodules do not inherit
the parent repository's `.gitattributes`. After changing attributes in an
older Windows checkout, re-clone or explicitly renormalize it before building
in WSL.

The core build needs the `libretro-common` and `zlib` submodules:

```sh
git submodule update --init libretro-common zlib
```

Initialize other submodules only when work or tests in those areas require
them; `git submodule update --init --recursive` downloads every firmware and
test submodule and is usually unnecessary for a core-only build.

On Ubuntu/WSL, install:

```sh
sudo apt-get update
sudo apt-get install build-essential git ninja-build flex bison python3 \
  python3-venv python3-pip pkgconf zlib1g-dev
```

For a Windows x64 cross-build, also install `gcc-mingw-w64-x86-64`.

Always use an out-of-tree build directory. The documented manual build is the
baseline; keep its configure switches aligned with `libretro-gitlab-build.sh`.
In particular, do not accidentally drop `--enable-slirp` or
`-Db_staticpic=true` from release-like builds, or
`--enable-whpx --enable-qemu-3dfx` from Windows x64 builds. QEMU defaults
static libraries to non-PIC; the PIC override is required when bundled slirp
is linked into the shared libretro core on ELF platforms.

## Build and verification

For a quick native Linux compile, follow the Ubuntu recipe in `README.md` and
build `libqemu_libretro.so`. For the primary Windows artifact, follow the WSL
cross-build recipe and build `libqemu_libretro.dll`. The manual Windows recipe
targets `x86_64-softmmu`; CI builds the broader target list in
`libretro-gitlab-build.sh`.

Keep `--enable-kvm` in the native x86 recipe even if runtime testing will use
TCG. The forward-ported x86 XSAVE helpers currently rely on state fields gated
by `CONFIG_KVM`, `CONFIG_HVF`, or `CONFIG_WHPX`, so a Linux x86 build without
KVM does not compile.

After changing configuration or Meson files, configure from a clean build
directory. After ordinary C changes, an incremental build is preferred.
Report the exact configure command and build target used.

Use proportional verification:

- Always compile the affected core target.
- Run relevant QEMU unit/qtests when changing shared QEMU subsystems; from a
  configured build directory, `make check-help` lists the available suites.
- For libretro lifecycle, frontend compatibility, disk control, QMP, core
  options, or presentation changes, exercise `tests/retro-host/retro_host.c`
  when present, or RetroArch/EmuVR. A successful compile alone is insufficient.
- For 3D changes, test on Windows with matching guest wrappers. The wrappers
  are signature-locked to the qemu-3dfx headers, and a mismatch fails silently.
- Runtime tests need firmware from `pc-bios/` under the frontend system
  directory's `qemu/` subdirectory.

When present, compile the Windows x64 harness from WSL with:

```sh
x86_64-w64-mingw32-gcc -O2 -Ilibretro-common/include \
  tests/retro-host/retro_host.c -o retro_host.exe -lws2_32 -lpsapi
```

The harness currently has a developer-specific default `system_dir` near the
top of the source; adjust it locally or add a configurable override before
using it on another machine. Do not commit another personal absolute path.

## Change discipline

Preserve unrelated worktree changes. This fork carries substantial behavior
not present in upstream QEMU, so inspect fork history and nearby libretro code
before forward-porting or resolving conflicts. Avoid broad mechanical rewrites
of QEMU code when a focused compatibility patch will do.

Keep the frontend thread responsive: blocking QEMU work belongs on the
emulation side, and shared state needs explicit ownership/synchronization.
Pay special attention to unload/reload, callbacks arriving during teardown,
and behavior when optional frontend interfaces are absent.

User-visible behavior belongs in `README.md`. If local planning notes are part
of the task, add concrete reproduction and verification details when closing
one of their tracked issues.
