V3 LEGACY AUDIT — not the V4 runtime architecture.

Cac file .cpp trong thu muc nay CHI la artifact kiem toan/mo phong (SIMULATED evidence).
Chung KHONG phai production code, KHONG duoc include/bien dich vao firmware ESP32 1.1.0 va KHONG thay the regression trong tests/.

Cach chay lai:
  g++ -O2 -std=c++17 -o timing_model timing_model.cpp && ./timing_model
  g++ -O2 -std=c++17 -o fault_slots  fault_slots.cpp  && ./fault_slots

Ket qua lich su da ghi nhan:
  timing_model 14/14 PASS
  fault_slots  13/13 PASS

Neu simulated artifact mau thuan voi code production hien tai, uu tien code + tests/ + release-manifest.json.
Realtime hien tai la Cloudflare WebSocket/DeviceHub; cac artifact audit cu khong duoc dung de suy ra rang runtime con MQTT broker.
