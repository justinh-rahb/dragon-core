#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t dc_portal_dns_start(uint32_t redirect_ip);
void dc_portal_dns_stop(void);
