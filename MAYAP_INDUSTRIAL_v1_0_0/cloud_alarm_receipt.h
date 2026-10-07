#pragma once
#include <ArduinoJson.h>
// HTTP status is insufficient: only a matching durable receipt releases an event.
inline bool mayapDurableAlarmReceipt(const char *body,const char *eventId) {
  JsonDocument receipt;
  return !deserializeJson(receipt,body) && receipt["success"]==true &&
      receipt["durable"]==true && strcmp(receipt["event_id"] | "",eventId)==0;
}
// Batch response {"success":true,"results":[{event_id,durable,...},...]}: bit i of the result is set only
// when results[] holds a durable receipt whose event_id equals ids[i]. Anything else stays queued.
inline uint8_t mayapDurableAlarmBatchReceipt(const char *body,const char (*ids)[40],uint8_t count) {
  JsonDocument receipt;
  if (count > 8U || deserializeJson(receipt,body) || !(receipt["success"]==true)) return 0U;
  uint8_t mask=0U;
  for (JsonObjectConst result : receipt["results"].as<JsonArrayConst>()) {
    if (!(result["durable"]==true)) continue;
    const char *id=result["event_id"] | "";
    for (uint8_t i=0U;i<count;++i) if (strcmp(id,ids[i])==0) mask|=static_cast<uint8_t>(1U<<i);
  }
  return mask;
}
