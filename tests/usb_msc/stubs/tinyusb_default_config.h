#pragma once
#include "tinyusb.h"
#define TINYUSB_DEFAULT_CONFIG(cb,arg) ((tinyusb_config_t){.event_cb=(cb),.event_arg=(arg)})
