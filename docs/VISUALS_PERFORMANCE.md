# Plasma-login-manager visual renderer and performance

## Visual design

The Wayland-only greeter now implements the approved Plasma 6 concept in
`qml/Main.qml`:

- Native-clock header, LPM branding, local-account rail (never fictitious
  guest accounts), avatar initial, translucent login panel and input controls
- Plasma (Wayland) fixed session label (only currently supported session)
- Confirmed suspend, reboot and shutdown controls via systemd-logind
- Primary-monitor credential form; secondary monitors show shared wallpaper
- Responsive layout from smaller displays to large UHD monitors
- Interactive password, MFA and expired-password challenges

This is a functional QML recreation of the concept, **not** the generated
mockup embedded as a static image.

## Graphics pipeline

`WallpaperProvider` prepares a decorative wave background on demand using
scalar or runtime-dispatched **AVX2** float vectors. It caches up to two
resolution variants. Qt Quick then uploads the resulting `QImage` as a
single texture and renders the wallpaper, translucent panel, gradients and
transitions using Qt's hardware-accelerated scenegraph (RHI) and KWin.

There is no continuous wallpaper re-rasterization or AVX usage in the
privileged PAM/session daemon. The AVX2 path never runs on an unsupported CPU.
The scalar renderer works on ARM, older x86, and in software-rendered CI.

## Fast cold-start path

- The production greeter now embeds Main.qml via Meson/Qt RCC. It no longer
  opens and parses an external QML file path on every boot. Note that RCC
  does **not** precompile QML bytecode; a separate qmlcachegen integration
  would be needed for that.
- Qt Quick Controls uses the lightweight **Basic** style rather than
  probing more complex system themes. All visually important controls
  have custom QML backgrounds.
- The procedural wallpaper uses `Image.asynchronous: true`, so Qt renders
  the login controls while the worker prepares image pixels.
- Full-resolution textures no longer generate unnecessary mipmaps.
- The daemon only accepts the greeter's `ready` message **after the first
  composited frame**, not merely after QML has been parsed.
- The normal hardware GPU/RHI path remains enabled; CI uses Pixman
  for its *nested Weston compositor* and Qt Quick software rendering
  to avoid a slow virtual GPU probe. Those settings are **not** applied
  to real logins on LPM.

Startup timing markers (milliseconds since each process started):

    PLM_APP_READY_MS=
    PLM_QML_READY_MS=
    PLM_FIRST_FRAME_MS=
    PLM_GREETER_READY_MS=

The last metric includes PAM/logind and KWin startup as observed by plmd;
the first-frame metric measures QML creation plus graphics initialization
inside plm-greeter. These values distinguish a slow KWin/DRM handoff from
a slow Qt/QML or GPU startup. On LPM collect:

    journalctl -b -u plmd.service -o short-monotonic | grep PLM_

Measurements are machine-specific. A passing CI test does not benchmark
the Ryzen/amdgpu cold boot path.

To compare SIMD output against the scalar reference:

    meson test -C build wallpaper-scalar-simd-parity --print-errorlogs

## CPU-specific builds

Portable LPM packages (recommended default):

    meson setup build --prefix=/usr --libexecdir=libexec \
      -Dcpu_tune=generic -Dsimd=true

Ryzen 5 4500U / Zen 2 *local build only*:

    meson setup build-ryzen --prefix=/usr --libexecdir=libexec \
      --buildtype=release -Dcpu_tune=znver2 -Dsimd=true
    meson compile -C build-ryzen
    meson test -C build-ryzen --print-errorlogs

For tuning the build machine rather than a portable package:

    meson setup build-native -Dcpu_tune=native -Dsimd=true

To disable optional SIMD explicitly:

    meson setup build-scalar -Dsimd=false

Only the greeter and wallpaper test use `-march=...`. The root daemon
and PAM helper are compiled with normal portable flags. Even in a
`znver2` build, the AVX2 instructions used for wallpaper preparation
are runtime checked. Never distribute a `-march=native` binary across
machines with different capabilities.

## GPU selection

The normal greeter uses **Qt Quick's GPU scenegraph** and KWin Wayland.
Qt RHI selects its graphics backend based on installed Qt/driver support.
On supported GPUs, administrators can experimentally select Vulkan using
`QSG_RHI_BACKEND=vulkan` for the greeter process; test before enabling
that system-wide. Do not set `QT_QUICK_BACKEND=software` for normal use:
that variable is reserved for CI compatibility testing.

The wallpaper texture can be up to 2560×1440, consuming approximately
14 MiB of uncompressed pixel data before RHI driver overhead. Smaller
screens request correspondingly smaller textures.

## Safeguards

The UI is **not** an authentication authority. It only sends bounded
requests to the privileged supervisor through its existing Unix socket.
No default guest account or unsupported Plasma/X11 session is presented.
The design exposes only actions backed by the daemon. Protecting password
QML/Qt memory and auditing PAM conversations remain required before
production use.
