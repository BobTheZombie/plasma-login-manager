# Test plan

Never test an unfinished display manager as the only login path.

## Static checks
Run: python3 tests/test-source-layout.py
This validates basic source invariants only; it does not compile the code.

## Build
meson setup build --prefix=/usr --libdir=lib
meson compile -C build

## VM environment requirements
- systemd-logind operational, seat0 and /dev/tty{1,2}
- plm sysusers account created, /var/lib/plm owned by plm
- Qt Wayland client plugin, Qt QML Controls, KWin Wayland compositor
- Plasma startplasma-wayland at /usr/bin/startplasma-wayland
- PAM system-login includes pam_systemd and required auth modules
- Greeter user has a logind session allowing KWin DRM access
- A known-good getty on tty3 or serial console for recovery

## Manual integration tests
1. Validate unit: systemd-analyze verify path/to/plmd.service
2. Stop other display manager inside a disposable VM.
3. Start plmd and inspect journalctl -fu plmd.service.
4. Check loginctl show-session <greeter-id> -p Class -p Type -p Seat -p VTNr.
5. Sign in with correct user/password; confirm VT2 session is Type=wayland.
6. Log out; confirm VT1 greeter returns and PAM closes the user session.
7. Repeat with an incorrect password; ensure no user session is created.
8. Attempt to connect to socket as unprivileged non-plm user; must fail.
9. Hotplug, rescale and remove outputs; observe Qt windows and KWin logs.
10. Test reboot/poweroff actions only in a disposable VM.
11. Deliberately crash greeter, root daemon and KWin separately;
    verify recovery and no lingering privileged session processes.

## Expected shortcomings
No MFA, remote seats, Bluetooth, Wi-Fi login, fingerprint,
password-change dialogs, user picker or keyboard/layout settings.

Do not mark production-ready merely because a password login succeeds.
