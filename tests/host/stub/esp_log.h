// Stub di esp_log.h per le prove sul PC: i messaggi del firmware si
// stampano solo con la variabile d'ambiente PROVE_LOG=1 (vedi host_rt.c).
#pragma once
void host_log(char level, const char *tag, const char *fmt, ...);
#define ESP_LOGE(tag, ...) host_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) host_log('D', tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) host_log('V', tag, __VA_ARGS__)
