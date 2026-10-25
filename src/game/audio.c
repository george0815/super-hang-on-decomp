/* Talking to the sound driver on the Z80. */

#include "game.h"

/*
 * Send a command byte to the sound driver ($6E8C): hold the Z80 bus, drop
 * the byte into the driver's mailbox, release the bus. Skipped while sound
 * is locked.
 */
void sound_command(uint8_t command)
{
    if (rd8(SOUND_LOCKED))
        return;
    do
        wr16(Z80_BUSREQ, 0x100);
    while (rd8(Z80_BUSREQ) & 1);
    wr8(Z80_SOUND_COMMAND, command);
    wr16(Z80_BUSREQ, 0);
}

void sound_command_68k(void)
{
    uint8_t locked = rd8(SOUND_LOCKED);
    sound_command((uint8_t)cpu.d[7]);
    if (locked)
        f_logic(1, locked);              /* tst.b */
    else
        f_logic(2, 0);                   /* move.w #0,... */
}

/*
 * Load the sound driver into Z80 RAM and restart the Z80 ($6E28): hold
 * the bus and reset, set the driver's region byte ($CB when $FFC892 is
 * set), copy the driver from ROM, then pulse reset and release the bus.
 */
#define ROM_Z80_DRIVER   0x07D7F4
#define Z80_DRIVER_SIZE  0x1A24
#define Z80_RESET        0xA11200
void sound_driver_load(void)
{
    int k;
    wr16(Z80_BUSREQ, 0x100);
    wr16(Z80_RESET, 0x100);
    wr8(0xA01C3C, rd16(0xFFC892) ? 0xCB : 0);
    for (k = 0; k < Z80_DRIVER_SIZE; k++)
        wr8(0xA00000 + k, rd8(ROM_Z80_DRIVER + k));
    wr16(Z80_RESET, 0);
    wr16(Z80_RESET, 0x100);
    wr16(Z80_BUSREQ, 0);
}

void sound_driver_load_68k(void) { sound_driver_load(); }
