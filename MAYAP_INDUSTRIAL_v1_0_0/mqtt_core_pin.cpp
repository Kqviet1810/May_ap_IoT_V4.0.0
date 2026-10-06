// Pins esp-mqtt's task to core 0 when it is CREATED.
//
// Why: the arduino-esp32 libraries are prebuilt with CONFIG_MQTT_TASK_CORE_SELECTION_ENABLED
// unset, so esp_mqtt_client_start() calls xTaskCreate() (no affinity). On the machine that
// task started on core 1 and, boosted by priority inheritance during the TLS/WSS handshake,
// starved the control task (priority 5, core 1) for >500 ms -> supervisor HEARTBEAT trip.
// IDF FreeRTOS cannot change affinity after creation, so the creation call is wrapped.
//
// Active only when the link uses -Wl,--wrap=xTaskCreate (CI passes it through
// compiler.c.elf.extra_flags; Arduino IDE users add it to platform.local.txt, see
// doc/MVP_BRINGUP.md). Without the flag this file is inert and the build still links.
#include <Arduino.h>
#include <string.h>

extern "C" BaseType_t __real_xTaskCreate(TaskFunction_t task, const char *name, uint32_t stackDepth,
                                         void *parameter, UBaseType_t priority,
                                         TaskHandle_t *created) __attribute__((weak));

extern "C" BaseType_t __wrap_xTaskCreate(TaskFunction_t task, const char *name, uint32_t stackDepth,
                                         void *parameter, UBaseType_t priority,
                                         TaskHandle_t *created) {
  if (name && strcmp(name, "mqtt_task") == 0) {
    return xTaskCreatePinnedToCore(task, name, stackDepth, parameter, priority, created, 0);
  }
  return __real_xTaskCreate(task, name, stackDepth, parameter, priority, created);
}
