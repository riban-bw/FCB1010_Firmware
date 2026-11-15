// Simulator test program for host_8051.c: calls the switch for slot 5.
#include <stdint.h>

void fcb_onerom_switch(uint8_t slot) __naked;

void main(void)
{
    fcb_onerom_switch(5);
}
