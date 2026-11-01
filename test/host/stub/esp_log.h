#pragma once
#include <cstdio>
#define ESP_LOGE(t, f, ...) fprintf(stderr, "E %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGW(t, f, ...) fprintf(stderr, "W %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGI(t, f, ...) ((void)0)
#define ESP_LOGD(t, f, ...) ((void)0)
