// File: src/intf/keyboard_input.h

#pragma once
#include <stdint.h>
#include "keyboard.h"

char to_ascii(uint16_t code);
void easy_handleinput(struct KeyboardEvent event);
