#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init(void);
void wifi_deinit(void);
bool wifi_connect(const char* ssid, const char* password);
bool wifi_disconnect();

#ifdef __cplusplus
}
#endif
