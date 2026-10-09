// Stub di esp_app_desc.h per le prove sul PC.
#pragma once
typedef struct {
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
