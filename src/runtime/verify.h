/*
 * Side-by-side verification of handwritten functions against the
 * recompiled originals, inside a running game (see tests/replay.c).
 *
 * For each call: snapshot CPU and RAM, run the original while logging its
 * hardware accesses, restore the snapshot and run the handwritten version
 * with those reads replayed and its writes captured, then compare RAM, the
 * function's output registers (from tools/flow.py), the stack pointer and
 * the hardware access sequence. The game continues with the original's
 * results either way.
 */
#ifndef VERIFY_H
#define VERIFY_H

#include <stdint.h>

enum { VERIFY_OFF, VERIFY_ORIGINAL, VERIFY_SHADOW };
extern int verify_mode;
extern int verify_enabled;   /* 0: verify builds just run the originals */

void verify_call(uint32_t addr, const char *name, void (*original)(void),
                 void (*handwritten)(void), uint64_t outputs);

/* hardware access hooks used by genesis.c while verifying */
int verify_hw_read(int width, uint32_t addr, uint32_t *value);   /* 1 = handled */
void verify_hw_log_read(int width, uint32_t addr, uint32_t value);
int verify_hw_write(int width, uint32_t addr, uint32_t value);   /* 1 = handled */
void verify_interrupt(int level);      /* an interrupt is being taken */
void verify_interrupt_end(int level);  /* and has returned */

void verify_report(void);
extern long verify_failures;

#endif
