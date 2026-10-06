"""Stage only public UI files; never copy firmware, secrets or tests to assets."""
from pathlib import Path
import shutil

root = Path(__file__).resolve().parent.parent
destination = root / "cloudflare/public"
destination.mkdir(parents=True, exist_ok=True)
files = ["index.html", "styles.css", "notes.css", "notes.js", "landing.css", "account.js", "config.js", "app.js",
         "protocol_v2.js", "push.js", "sw.js", "manifest.webmanifest",
         "vendor/jsQR.min.js", "docs/MAYAP_Huong_dan_van_hanh_A5_v1.3_E503.pdf"]
files += [str(path.relative_to(root)) for path in (root / "icons").glob("*.png")]
allowed = set(files) | {"_headers"}
for path in destination.rglob("*"):
    if path.is_file() and path.relative_to(destination).as_posix() not in allowed:
        path.unlink()
for name in files:
    target = destination / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / name, target)
(destination / "_headers").write_text("""/*
  X-Content-Type-Options: nosniff
  Referrer-Policy: strict-origin-when-cross-origin
  X-Frame-Options: DENY
  Permissions-Policy: geolocation=(), microphone=()
/sw.js
  Cache-Control: no-cache
/config.js
  Cache-Control: no-cache
""", encoding="utf-8")
print(f"Web assets staged: {len(files)} public files")
