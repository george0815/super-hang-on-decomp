#ifndef OPTIONS_H
#define OPTIONS_H

#include <stdint.h>
#include <SDL3/SDL.h>

void options_load(void);
int options_open(void);                       /* overlay visible (game paused) */
int options_event(const SDL_Event *e);        /* returns 1 if the event was used */
void options_draw(uint32_t *pixels, int pitch_in_pixels);

#endif
