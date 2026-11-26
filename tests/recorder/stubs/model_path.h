#pragma once

typedef struct srmodel_list_t srmodel_list_t;

srmodel_list_t *srmodel_load(const void *);
srmodel_list_t *get_static_srmodels(void);
char *esp_srmodel_filter(srmodel_list_t *, const char *, const char *);
void esp_srmodel_deinit(srmodel_list_t *);
