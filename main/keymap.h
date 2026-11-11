#pragma once
#include <stdbool.h>
#include <stdint.h>

/* modifier bits (byte 0 of keyboard report) */
#define MOD_LCTRL  0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT   0x04
#define MOD_LGUI   0x08
#define MOD_RCTRL  0x10
#define MOD_RSHIFT 0x20
#define MOD_RALT   0x40
#define MOD_RGUI   0x80

/* Resolve a key name ("A", "F5", "CTRL", "0x2C"...).
 * Returns true; *mod set for modifiers (usage=0), else *usage set. */
bool keymap_lookup(const char *name, uint8_t *usage, uint8_t *mod);

/* US layout: ASCII char -> usage + shift flag. false if unsupported. */
bool keymap_ascii(char c, uint8_t *usage, bool *shift);

/* Consumer usage by name ("VOLUME_UP"...); 0 if unknown. */
uint16_t keymap_media(const char *name);
