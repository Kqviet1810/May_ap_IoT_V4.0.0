#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]

def text(path):
    return (ROOT / path).read_text(encoding='utf-8')

def need(cond, message):
    if not cond:
        raise SystemExit('EEPROM HISTORY CHECK FAIL: ' + message)

cfg = text('MAYAP_INDUSTRIAL_v1_0_0/config.h')
hist = text('MAYAP_INDUSTRIAL_v1_0_0/history_store.h')
rt = text('MAYAP_INDUSTRIAL_v1_0_0/transaction_bridge.h')
mc = text('MAYAP_INDUSTRIAL_v1_0_0/machine_control.h')
app = text('app.js')
html = text('index.html')
worker = text('cloudflare/src/index.js')

def num(name, base=10):
    m = re.search(rf'{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)[UL]+', cfg)
    need(m, 'thieu ' + name)
    return int(m.group(1), 0)

need(num('EEPROM_CAPACITY_BYTES') == 65536, 'khong phai AT24C512 64KiB')
need(num('EEPROM_PAGE_SIZE') == 128, 'page AT24C512 phai 128B')
need(num('EEPROM_I2C_ADDRESS') == 0x50, 'dia chi EEPROM phai 0x50')
need(num('EEPROM_MAX_WRITE_CHUNK') <= 126, 'Wire TX overflow')
base = num('EEPROM_ADDR_TEMP_HISTORY')
slots = num('TEMP_HISTORY_SLOT_COUNT')
rec = num('TEMP_HISTORY_RECORD_BYTES')
interval = num('TEMP_HISTORY_SAMPLE_SEC')
need(base == 0x1000, 'history base phai 0x1000')
need(slots == 2016 and rec == 4 and interval == 300, 'layout 7 ngay/5phut sai')
need(base + slots * rec <= 65536, 'history vuot EEPROM')
need((base % 128) == 0 and (128 % rec) == 0, 'record phai can page')
need('lastSampleBucket' in hist and 'crc8' in hist, 'thieu anti-hotspot/CRC')
need('EEPROM_ADDR_TEMP_HISTORY +' in hist, 'history khong dung vung rieng')
need('mayapTemperatureHistorySample' in mc, 'MachineController chua sample history')
need('history/request' in rt and 'history/reported' in rt, 'realtime history contract thieu')
need('verifyAndDispatch("history/request"' in rt, 'history request khong HMAC')
need('mayap-control-grant:v2' in text('cloudflare/src/account-auth.js') and 'realtimeVerifyV2' in rt,
     'Worker/ESP chua ho tro phien HMAC V2 cho history/request')
need('temperatureChartCanvas' in html and html.count('id="batchLogList"') == 1, 'UI chart/log sai')
need('history/reported' in app and "signRealtimeWrite(device, 'history/request'" in app, 'web realtime history sai')
need('/api/device/history' not in app, 'web van phu thuoc Cloud history')
need('telemetry_history' not in worker, 'Worker runtime khong duoc luu telemetry')

# Wear worst-case: 4-byte record, page 32B => 8 writes/page/day. Datasheet minimum
# 1,000,000 page-write cycles @25C => >300 nam ly thuyet; chi check kien truc.
writes_per_page_day = (128 // rec) / 7
need(writes_per_page_day < 5, 'wear distribution khong nhu thiet ke')
print(f'EEPROM history checks: OK base=0x{base:04X} bytes={slots*rec} writes/page/day={writes_per_page_day}')
