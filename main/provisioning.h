#pragma once

#include "app_config.h"

// Blocks forever: starts the setup SoftAP and web form, then reboots after save.
void provisioning_run(app_config_t *cfg);
