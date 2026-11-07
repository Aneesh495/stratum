#ifndef STRATUM_KERNEL_KEYBOARD_H
#define STRATUM_KERNEL_KEYBOARD_H

#include <kernel/types.h>

void keyboard_init(void);
bool keyboard_ready(void);
int keyboard_getc(void);

#endif
