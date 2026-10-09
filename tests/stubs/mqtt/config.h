#pragma once
#include "mayap_stubs.h"
// The serial diagnostics are part of the firmware surface (the [MQTT-STAT]/[MQTT-TX]/[MQTT-ARB] lines are how a field problem is read): compile and exercise them.
#ifndef MAYAP_DIAGNOSTIC_SERIAL
#define MAYAP_DIAGNOSTIC_SERIAL 1
#endif
