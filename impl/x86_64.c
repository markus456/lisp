#include "x86_64.h"
#include <string.h>

// Patches the jump point
void PATCH_JMP32(uint8_t* ptr, uint32_t off)
{
    memcpy(ptr - sizeof(off), &off, sizeof(off));
}
