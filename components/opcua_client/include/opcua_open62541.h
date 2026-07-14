#pragma once

/* Support both a normal open62541 installation and its amalgamated form. */
#if __has_include(<open62541/client.h>)
#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>
#elif __has_include(<open62541.h>)
#include <open62541.h>
#else
#error "open62541 headers were not found. Add the open62541 ESP-IDF component."
#endif
