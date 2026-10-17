#pragma once

// Transport-free constants of the dc_logtail contract, split from dc_logtail.h
// so code without esp_http_server (host tests, consumers' tooling) can use them.

#define DC_LOGTAIL_URI          "/api/v1/system/log"
// Fixed upper bound on payload bytes per response. One response is one
// coherent interval copied under the console spinlock into a buffer on the
// httpd task stack, so this bounds both lock hold time and stack use.
#define DC_LOGTAIL_MAX_PAYLOAD  1024
// Longest accepted boot identity, excluding the NUL.
#define DC_LOGTAIL_BOOT_ID_MAX  64
