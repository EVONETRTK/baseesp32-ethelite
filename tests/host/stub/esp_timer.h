// Stub di esp_timer.h: tempo finto governato dalle prove (host_rt.c).
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
// Solo prove: tempo restituito dalla prossima chiamata (vedi host_rt.c).
void host_time_set_us(int64_t us);
void host_time_advance_us(int64_t us);
