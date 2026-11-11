#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
size_t hid_report_build(bool kbd, bool mouse, bool media, bool absp, bool gamepad, const uint8_t **out);
