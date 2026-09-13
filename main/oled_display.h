#pragma once

// Inizializza (se i pin SDA/SCL sono configurati in settings, vedi
// settings.h) un display OLED SSD1306 128x64 via I2C e avvia un task che
// alterna a rotazione tre schermate diagnostiche: stato dispositivo,
// satelliti GNSS per costellazione, segnale cellulare/WiFi. Non fa nulla
// se i pin non sono impostati (-1).
void oled_display_start(void);
