#pragma once

#include "config.h"
#include "boot_diagnostic.h"
#include "firmware_update_guard.h"
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// Quay lai firmware TRUOC DO ma KHONG can nap lai tu dau qua USB/OTA - tan
// dung dung thiet ke OTA "2 vi tri" (A/B) co san cua chip ESP32-S3 (xem
// build-firmware.yml: Partition Scheme "default_8MB" co ota_0 va ota_1,
// moi vi tri ~3.1MB). Moi lan OTA that su (qua Update.h - xem ota_web_
// update.h) CHI GHI DE vao vi tri KHONG dang chay; vi tri
// DANG CHAY truoc do van con NGUYEN trong flash cho toi khi no bi mot lan
// OTA khac ghi de trong tuong lai. File nay chi lam 1 viec don gian va an
// toan: bao bootloader LAN SAU khoi dong hay doi sang vi tri con lai (KHONG
// ghi/xoa gi ca - chi doi 1 con tro nho trong vung "otadata"), roi khoi
// dong lai - dung khi ban vua nap co van de va can quay ve ban chay on
// dinh truoc do.
//
// GIOI HAN QUAN TRONG (phai noi ro voi nguoi dung): CHI giu duoc DUY NHAT 1
// ban truoc do - day la gioi han cua thiet ke phan cung "2 vi tri", khong
// phai "kho luu nhieu phien ban". Neu OTA 2 lan lien tiep ma khong quay lai
// giua chung, ban dau tien se bi ghi de vinh vien va khong con cach nao lay
// lai duoc nua.
namespace MayapOtaRollbackInternal {
static bool cachedTargetValid = false;
static volatile uint8_t requestFlag = 0U;
}  // namespace MayapOtaRollbackInternal

// Kiem tra vi tri OTA CON LAI (khong phai vi tri dang chay hien tai) co dang
// giu 1 anh firmware HOP LE hay khong, bang cach doc byte "magic" dau tien
// cua anh ESP32 (0xE9 - ESP_IMAGE_HEADER_MAGIC). Chi doc 1 byte (khong doc
// toan bo anh ~3MB), du de phan biet "co firmware that" voi "con trong/da
// xoa tu nha may hoac chua tung OTA lan nao" (flash trong mang gia tri
// 0xFF). KHONG cho phep quay lai vao vung nho rac - se lam thiet bi khong
// khoi dong duoc, phai chinh sua that.
inline bool mayapRollbackTargetValidUncached() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running) return false;
  const esp_partition_t *target = esp_ota_get_next_update_partition(running);
  if (!target) return false;
  uint8_t magic = 0U;
  if (esp_partition_read(target, 0, &magic, 1U) != ESP_OK) return false;
  return magic == 0xE9U;
}

// Goi 1 LAN trong setup() (xem .ino) - ket qua chi thay doi qua 1 lan
// OTA/rollback that su (luon di kem khoi dong lai), nen cache lai thay vi
// doc flash lai moi khi HMI can biet de an/hien muc menu.
inline void mayapOtaRollbackBegin() {
  MayapOtaRollbackInternal::cachedTargetValid = mayapRollbackTargetValidUncached();
}

// HMI dung ham nay de quyet dinh an/hien muc "Quay lai ban cu", va
// machine_control.h dung de tra loi ACK ro rang ngay lap tuc (khong phai
// doi otaTask xu ly xong moi biet co lam duoc hay khong).
inline bool mayapRollbackAvailable() {
  return MayapOtaRollbackInternal::cachedTargetValid;
}

// Chi dat co hieu - viec THAT SU doi vi tri khoi dong + restart nam trong
// mayapFirmwareRollbackUpdate(), goi tu otaTask (cung task voi moi thao tac
// dung den flash khac, xem .ino) de khong tranh chap voi OTA dang chay.
inline void mayapRequestFirmwareRollback() {
  __atomic_store_n(&MayapOtaRollbackInternal::requestFlag, 1U, __ATOMIC_RELEASE);
}

// Goi moi chu ky tu otaTask. Kiem tra lai lan nua (khong chi dua vao cache)
// truoc khi thuc su doi huong khoi dong - phong truong hop hiem gap giua
// luc nguoi dung xac nhan va luc otaTask xu ly, co 1 tien trinh OTA khac
// vua ghi de len vi tri do (vd 2 yeu cau OTA/rollback chen nhau).
inline void mayapFirmwareRollbackUpdate(uint32_t now) {
  (void)now;
  if (!__atomic_load_n(&MayapOtaRollbackInternal::requestFlag, __ATOMIC_ACQUIRE)) return;
  __atomic_store_n(&MayapOtaRollbackInternal::requestFlag, 0U, __ATOMIC_RELEASE);

  if (!mayapFirmwareMaintenanceReady() || mayapFirmwareMaintenanceActive()) {
    mayapSerialPrintf(false, "[ROLLBACK] MAY CHUA AN TOAN DE CAP NHAT - HUY\n");
    return;
  }
  MayapFirmwareMaintenance maintenance;

  if (!mayapRollbackTargetValidUncached()) {
    mayapSerialPrintf(false, "[ROLLBACK] KHONG CON ban truoc do hop le - HUY\n");
    return;
  }
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *target = esp_ota_get_next_update_partition(running);
  const esp_err_t err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    mayapSerialPrintf(false, "[ROLLBACK] esp_ota_set_boot_partition loi=%d\n",
                      static_cast<int>(err));
    return;
  }
  mayapSerialPrintf(false,
      "[ROLLBACK] Da chuyen huong khoi dong ve firmware truoc do - KHOI DONG LAI\n");
  // Danh dau day la khoi dong lai CO CHU DICH (xem config.h) - khong de
  // PowerManager tinh nham lan rollback thanh cong nay vao bo dem "reset bat
  // thuong", tranh bao gia "ABNORMAL RESET"/mat dien sau khi quay lai ban cu.
  mayapMarkIntentionalRestart();
  delay(300);
  mayapRestart(MayapBoot::RestartReason::Rollback, "operator rollback complete");
}
