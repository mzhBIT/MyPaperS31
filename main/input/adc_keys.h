#pragma once

#include "activities/Activity.h"

void adc_keys_init();
void adc_keys_update();
bool adc_keys_pop(AppKey *key);
