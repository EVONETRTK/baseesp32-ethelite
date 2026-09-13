#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

// Consuma byte RTCM3 da rtcm_stream e li inoltra al caster EVONETRTK
// come sorgente NTRIP (SOURCE <password> /<mountpoint>). Si riconnette
// automaticamente in caso di errore. arg = StreamBufferHandle_t.
void ntrip_client_task(void *arg);
