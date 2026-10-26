/*
 * Sound driver test: plays every sound ID through the original Z80 driver
 * and the C driver side by side (src/runtime/sound_verify.c compares the
 * Z80 RAM and every chip write after each call), over songs, with pause,
 * fade, the engine sound and the 68000's other inputs varied, to reach the
 * paths the recorded sessions don't.
 *
 * usage: sndtest ROM [TICKS [ROUND ID]]   (one run: ROUND, sound ID in hex)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#include "z80_rt.h"
#include "genesis.h"
#include "sound.h"

extern volatile int sound_verify_running;
extern int sound_verify_quiet;
long sound_verify_failed(void);
static int cur_id, cur_round;

/* A hang (or, in the recompiled original, runaway recursion that the Z80
   would just loop through) ends the run: skipped when it is the original. */
static void watchdog(int sig)
{
    fprintf(stderr, "sndtest: round %d sound %02X %s in the %s driver\n", cur_round, cur_id,
            sig == SIGALRM ? "hangs" : "crashes",
            sound_verify_running == 1 ? "original" : sound_verify_running == 2 ? "C" : "test");
    _exit(sound_verify_running == 1 ? 3 : 4);
}

static void watch(int seconds)
{
    static char altstack[65536];
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = watchdog;
        sa.sa_flags = SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGALRM, &sa, NULL);
    }
    alarm((unsigned)seconds);
}

/* no video or pads here */
void host_frame(const uint8_t *pixels) { (void)pixels; }
uint16_t host_pad(int port) { (void)port; return 0; }

/* The sounds the game asks for (the sound test's list, the race's), and
   the values the 68000 writes: RPM up to $1C0, turbo and rumble flags $80,
   a rival's distance up to $80. Other IDs can hang the original. */
static const uint8_t sounds[] = {
    0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A,
    0x90, 0x92, 0x93, 0x94, 0x95, 0x97, 0x9B, 0x9C, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAD, 0xB0, 0xB1, 0xB3, 0xB4,
};
#define NSOUNDS (int)(sizeof sounds / sizeof sounds[0])
#define FUZZ_RUNS 400

static uint32_t seed = 1;
static unsigned rnd(unsigned n)
{
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) % n;
}

static void driver_reset(void)
{
    memset(z80_ram + 0x1A24, 0, 0x2000 - 0x1A24);
    memset(&z80, 0, sizeof z80);
    z80.sp = 0x1FFD;
    sound_call(SND_STOP_ALL);
    sound_call(SND_FM_RESET);
    sound_call(SND_BANK);
}

/* One frame as the timer-driven driver runs it, with the 68000's inputs
   moved about. */
static void frame(int t)
{
    if (rnd(4) == 0) {
        uint16_t rpm = (uint16_t)rnd(0x1C0);
        z80_ram[0x1C31] = (uint8_t)rpm;
        z80_ram[0x1C32] = (uint8_t)(rpm >> 8);
    }
    if (rnd(200) == 0)
        z80_ram[0x1C36] ^= 0x80;
    if (rnd(50) == 0)
        z80_ram[0x1C38] = (uint8_t)rnd(0x81);
    if (rnd(150) == 0)
        z80_ram[0x1C39] ^= 0x80;
    if (rnd(300) == 0)                        /* (the game uses $1C03 instead) */
        z80_ram[0x1C04 + rnd(3)] = sounds[rnd(NSOUNDS)];
    sound_call(SND_REQUESTS);
    if (t % 7 == 0)
        sound_call(SND_TIMER_RESET);
    sound_call(SND_TICK);
}

/* One run: a sound, maybe over a song, with pause, more sounds and fades. */
static void play(int round, int id, int ticks)
{
    int t;
    cur_id = id;
    cur_round = round;
    watch(20);
    driver_reset();
    if (round == 2) {                      /* the VBlank-driven phase */
        z80_ram[0x1C18] = 2;
        z80_ram[0x1C03] = (uint8_t)id;
        for (t = 0; t < ticks / 2; t++)
            sound_call(SND_IRQ);
        return;
    }
    if (round == 1 || id >= 0x90) {        /* a song underneath */
        z80_ram[0x1C03] = (uint8_t)(0x81 + rnd(10));
        for (t = 0; t < 60 + (int)rnd(200); t++)
            frame(t);
    }
    z80_ram[0x1C03] = (uint8_t)id;
    for (t = 0; t < ticks; t++) {
        if (t == ticks / 3)
            z80_ram[0x1C08] = 1;           /* pause */
        if (t == ticks / 3 + 20)
            z80_ram[0x1C09] = 1;
        if (round == 1 && t == ticks / 2)
            z80_ram[0x1C03] = sounds[10 + rnd(NSOUNDS - 10)];
        if (round == 1 && t == 2 * ticks / 3)
            z80_ram[0x1C03] = rnd(2) ? 0xB0 : 0xB3;
        frame(t);
    }
}

/* Fuzzing for the commands and paths the game's data never uses: under a
   song, some tracks read random bytes from free RAM at $1A30, with random
   settings. */
static void fuzz(int run, int ticks)
{
    int t, k;
    cur_id = run;
    cur_round = 3;
    watch(5);
    driver_reset();
    z80_ram[0x1C03] = (uint8_t)(0x81 + rnd(10));
    for (t = 0; t < 30; t++)
        frame(t);
    for (k = 0; k < 0x1C0; k++) {
        unsigned r = rnd(10);
        z80_ram[0x1A30 + k] = (uint8_t)(r < 4 ? 0x80 + rnd(0x60) : r < 7 ? 1 + rnd(0x20) : 0xE0 + rnd(0x20));
    }
    for (k = 0; k < 14; k++) {
        uint16_t tr = (uint16_t)(0x1C60 + 0x30 * k);
        if (rnd(3))
            continue;
        z80_ram[tr] = (uint8_t)(0x80 | (rnd(4) ? 0 : rnd(0x80)));
        z80_ram[tr + 3] = (uint8_t)(0x30 + rnd(0x100));
        z80_ram[tr + 4] = 0x1A;
        z80_ram[tr + 5] = (uint8_t)rnd(0x10);
        z80_ram[tr + 6] = (uint8_t)rnd(4);
        z80_ram[tr + 7] = (uint8_t)rnd(4);
        z80_ram[tr + 10] = 1;
        z80_ram[tr + 11] = 0;
        z80_ram[tr + 17] = (uint8_t)rnd(0x100);
        z80_ram[tr + 18] = (uint8_t)rnd(0x100);
    }
    for (t = 0; t < ticks; t++)
        frame(t);
}

int main(int argc, char **argv)
{
    static uint8_t rom[0x80000];
    FILE *f;
    int ticks = argc > 2 ? atoi(argv[2]) : 600, id, round, k, failures = 0, hangs = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: sndtest ROM [TICKS]\n");
        return 1;
    }
    f = fopen(argv[1], "rb");
    if (!f || fread(rom, 1, sizeof rom, f) != sizeof rom) {
        perror(argv[1]);
        return 1;
    }
    fclose(f);
    gen_init(rom, sizeof rom);
    memcpy(z80_ram, rom + 0x7D7F4, 0x1A24);

    if (argc > 4) {                            /* one run: ROUND ID */
        round = atoi(argv[3]);
        id = (int)strtol(argv[4], NULL, 16);
        seed = (uint32_t)(round * 256 + id);
        if (round == 3)
            fuzz(id, ticks);
        else
            play(round, id, ticks);
        return 0;
    }
    sound_verify_quiet = 1;
    for (round = 0; round < 3; round++)
        for (k = 0; k < NSOUNDS; k++) {
            pid_t pid;
            id = sounds[k];
            int status;
            fflush(stdout);
            pid = fork();
            if (pid == 0) {
                seed = (uint32_t)(round * 256 + id);
                play(round, id, ticks);
                fflush(stdout);
                _exit(sound_verify_failed() ? 1 : 0);
            }
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 3)
                hangs++;
            else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                printf("round %d sound %02X: FAILED\n", round, id);
                failures++;
            }
        }
    for (k = 0; k < FUZZ_RUNS; k++) {
        pid_t pid;
        int status;
        fflush(stdout);
        pid = fork();
        if (pid == 0) {
            seed = (uint32_t)(3 * 256 + k);
            fuzz(k, ticks / 2);
            fflush(stdout);
            _exit(sound_verify_failed() ? 1 : 0);
        }
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 3)
            hangs++;
        else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("fuzz run %d (round 3 id %x): FAILED\n", k, k);
            failures++;
        }
    }
    printf("%d runs, %d failed, %d skipped (the original hangs or crashes)\n", 3 * NSOUNDS + FUZZ_RUNS,
           failures, hangs);
    return failures != 0;
}
