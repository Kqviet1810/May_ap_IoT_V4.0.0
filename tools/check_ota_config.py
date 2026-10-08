from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "MAYAP_INDUSTRIAL_v1_0_0"
WORKFLOW = ROOT / ".github/workflows/build-firmware.yml"

# ArduinoOTA (LAN upload from the Arduino IDE) was removed on purpose: direct flashing is USB only and
# remote updates are the signed Cloudflare web OTA. Nothing may bring the listener or its secret back.
for gone in ("ota_update.h", "arduino_ota_window.h", "build_secrets.h", "build_secrets.local.h"):
    if (FW / gone).exists():
        raise SystemExit(f"OTA CONFIG FAIL: {gone} must not exist (ArduinoOTA removed)")

pattern = re.compile(r"ArduinoOTA|MAYAP_OTA_PASSWORD|\bOTA_PASSWORD\b|ESPmDNS|\bMDNS\.")
code_comment = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
for path in FW.iterdir():
    if path.suffix in {".h", ".ino", ".cpp"}:
        text = code_comment.sub("", path.read_text(encoding="utf-8", errors="ignore"))
        if pattern.search(text):
            raise SystemExit(f"OTA CONFIG FAIL: {path.name} references the removed ArduinoOTA/mDNS code")

workflow = WORKFLOW.read_text(encoding="utf-8")
if "MAYAP_OTA_PASSWORD" in workflow or "build_secrets" in workflow:
    raise SystemExit("OTA CONFIG FAIL: CI still handles an ArduinoOTA password")
if "ArduinoOTAClass::" not in workflow:
    raise SystemExit("OTA CONFIG FAIL: CI no longer checks that ArduinoOTA is absent from the ELF")
print("OTA config: ArduinoOTA removed, web OTA only OK")
