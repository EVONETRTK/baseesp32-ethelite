// Stub di esp_system.h per le prove sul PC.
#pragma once
#include "esp_err.h"
// Sul PC non riavvia: conta le chiamate (host_rt.c).
void esp_restart(void);
extern int host_restart_count;
