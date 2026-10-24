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
