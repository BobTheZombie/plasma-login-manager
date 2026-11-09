#!/usr/bin/env python3
"""Non-privileged source invariants, NOT authentication integration tests."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class SourceChecks(unittest.TestCase):
    def file(self, name):
        return (ROOT / name).read_text(encoding="utf-8")

    def test_wayland_only(self):
        session = self.file("src/session.cpp")
        self.assertIn("kwin_wayland", session)
        self.assertIn("startplasma-wayland", session)
        self.assertNotIn('execl("/usr/bin/Xorg"', session)

    def test_private_auth_transport(self):
        daemon = self.file("src/daemon.cpp")
        self.assertIn("SO_PEERCRED", daemon)
        self.assertIn("0600", daemon)
        self.assertIn("MaxFrame", daemon)
        self.assertIn("AF_UNIX", self.file("docs/IMPLEMENTATION.md"))
        self.assertNotIn('password=argv', daemon)

    def test_pam_lifecycle(self):
        worker = self.file("src/session.cpp")
        for term in ("pam_authenticate", "pam_acct_mgmt", "pam_setcred",
                     "pam_open_session", "pam_close_session", "setuid",
                     "initgroups", "XDG_SESSION_TYPE", "XDG_VTNR"):
            with self.subTest(term=term):
                self.assertIn(term, worker)
        self.assertIn("pam_systemd.so", self.file("data/pam/plm-greeter"))
        self.assertIn("PAM_CHANGE_EXPIRED_AUTHTOK", worker)
        self.assertIn("PROMPT", worker)
        self.assertIn("receivePamReply", worker)

    def test_no_unsafe_global_sandbox_for_user_children(self):
        service = self.file("data/plmd.service")
        for disallowed in ("NoNewPrivileges=yes", "PrivateTmp=yes",
                           "RestrictAddressFamilies=AF_UNIX", "ProtectSystem=strict"):
            self.assertNotIn(disallowed, service)
        self.assertIn("Alias=display-manager.service", service)
        self.assertIn("WatchdogSec=30s", service)
        self.assertIn("StartLimitBurst=4", service)

    def test_qt_moc_and_multiscreen(self):
        self.assertIn('addImageProvider("plmwallpaper"', self.file("src/greeter.cpp"))
        self.assertIn("qrc:/plm/Main.qml", self.file("src/greeter.cpp"))
        self.assertIn("PLM_FIRST_FRAME_MS=", self.file("src/greeter.cpp"))
        self.assertIn("qt6.compile_resources", self.file("meson.build"))
        self.assertIn("QQuickStyle::setStyle", self.file("src/greeter.cpp"))
        self.assertIn("test('wallpaper-scalar-simd-parity'", self.file("meson.build"))
        self.assertIn("AVX2", self.file("src/wallpaper.cpp"))
        self.assertIn('text: "Plasma (Wayland)"', self.file("qml/Main.qml"))
        self.assertIn("qt6.preprocess(moc_headers:", self.file("meson.build"))
        self.assertIn("screenAdded", self.file("src/greeter.cpp"))
        self.assertIn("screenRemoved", self.file("src/greeter.cpp"))
        self.assertIn("Q_PROPERTY(bool awaitingResponse", self.file("src/backend.h"))
        self.assertIn("QJsonArray m_users", self.file("src/daemon.cpp"))

if __name__ == "__main__":
    unittest.main(verbosity=2)
