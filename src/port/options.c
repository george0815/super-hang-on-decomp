/*
 * Options overlay for the native port (F1): sound mix and output filter.
 * Drawn straight into the 320x224 frame with a small built-in font, and
 * saved to settings.ini in the user's preferences folder.
 */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "options.h"
#include "sound.h"

/* 5x7 font, one byte per row, bit 4 = leftmost pixel */
static const struct { char c; uint8_t rows[7]; } font[] = {
    { ' ', { 0, 0, 0, 0, 0, 0, 0 } },
    { '%', { 0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13 } },
    { '-', { 0, 0, 0, 0x1f, 0, 0, 0 } },
    { ':', { 0, 0x04, 0x04, 0, 0x04, 0x04, 0 } },
    { '/', { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 } },
    { '<', { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 } },
    { '>', { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 } },
    { '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
    { '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { '2', { 0x0e, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1f } },
    { '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
    { '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
    { '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
    { '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
    { '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } },
    { '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
    { 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
    { 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
    { 'D', { 0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c } },
    { 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
    { 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } },
    { 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
    { 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
    { 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
    { 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
    { 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a } },
    { 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } },
};

enum { ROW_MASTER, ROW_MUSIC, ROW_ENGINE, ROW_EFFECTS, ROW_FILTER, ROWS };
static const char *row_name[ROWS] = { "MASTER", "MUSIC", "ENGINE", "EFFECTS", "FILTER" };
static const char *filter_name[FILTER_MODES] = { "OFF", "CONSOLE", "SOFT" };

static int open_, row;

static const uint8_t *glyph(char c)
{
    size_t i;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (i = 0; i < sizeof font / sizeof font[0]; i++)
        if (font[i].c == c) return font[i].rows;
    return font[0].rows;
}

static void text(uint32_t *px, int pitch, int x, int y, const char *s, uint32_t color)
{
    for (; *s; s++, x += 6) {
        const uint8_t *g = glyph(*s);
        int r, b;
        for (r = 0; r < 7; r++)
            for (b = 0; b < 5; b++)
                if (g[r] & (0x10 >> b))
                    px[(y + r) * pitch + x + b] = color;
    }
}

static int *value_of(int r)
{
    switch (r) {
    case ROW_MASTER:  return &sound_settings.master;
    case ROW_MUSIC:   return &sound_settings.group[SND_MUSIC];
    case ROW_ENGINE:  return &sound_settings.group[SND_ENGINE];
    case ROW_EFFECTS: return &sound_settings.group[SND_EFFECTS];
    default:          return &sound_settings.filter;
    }
}

static char *settings_path(void)
{
    static char path[1024];
    char *dir = SDL_GetPrefPath("SuperHangOnPort", "shangon");
    snprintf(path, sizeof path, "%ssettings.ini", dir ? dir : "");
    SDL_free(dir);
    return path;
}

void options_load(void)
{
    FILE *f = fopen(settings_path(), "r");
    char line[128];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char key[64];
        int v, r;
        if (sscanf(line, "%63[^=]=%d", key, &v) != 2) continue;
        for (r = 0; r < ROWS; r++) {
            char lower[16];
            int k;
            for (k = 0; row_name[r][k]; k++) lower[k] = (char)(row_name[r][k] | 0x20);
            lower[k] = 0;
            if (!strcmp(key, lower))
                *value_of(r) = r == ROW_FILTER ? (v % FILTER_MODES + FILTER_MODES) % FILTER_MODES
                                               : (v < 0 ? 0 : v > 100 ? 100 : v);
        }
    }
    fclose(f);
}

static void save(void)
{
    FILE *f = fopen(settings_path(), "w");
    if (!f) return;
    fprintf(f, "master=%d\nmusic=%d\nengine=%d\neffects=%d\nfilter=%d\n",
            sound_settings.master, sound_settings.group[SND_MUSIC],
            sound_settings.group[SND_ENGINE], sound_settings.group[SND_EFFECTS],
            sound_settings.filter);
    fclose(f);
}

int options_open(void) { return open_; }

int options_event(const SDL_Event *e)
{
    int *v;
    if (e->type != SDL_EVENT_KEY_DOWN)
        return 0;
    if (e->key.key == SDLK_F1) {
        open_ = !open_;
        if (!open_) save();
        return 1;
    }
    if (!open_)
        return 0;
    v = value_of(row);
    switch (e->key.key) {
    case SDLK_ESCAPE: open_ = 0; save(); break;
    case SDLK_UP:     row = (row + ROWS - 1) % ROWS; break;
    case SDLK_DOWN:   row = (row + 1) % ROWS; break;
    case SDLK_LEFT:
        if (row == ROW_FILTER) *v = (*v + FILTER_MODES - 1) % FILTER_MODES;
        else if ((*v -= 5) < 0) *v = 0;
        break;
    case SDLK_RIGHT:
        if (row == ROW_FILTER) *v = (*v + 1) % FILTER_MODES;
        else if ((*v += 5) > 100) *v = 100;
        break;
    }
    return 1;
}

void options_draw(uint32_t *px, int pitch)
{
    const int x0 = 70, y0 = 56, w = 180, h = 100;
    int x, y, r;
    if (!open_) return;
    for (y = y0; y < y0 + h; y++)        /* darken the game behind the panel */
        for (x = x0; x < x0 + w; x++)
            px[y * pitch + x] = (px[y * pitch + x] >> 2) & 0x3f3f3f;
    text(px, pitch, x0 + 46, y0 + 8, "SOUND OPTIONS", 0xffd040);
    for (r = 0; r < ROWS; r++) {
        char val[16];
        uint32_t c = r == row ? 0xffffff : 0x9090a0;
        if (r == ROW_FILTER)
            snprintf(val, sizeof val, "%s", filter_name[sound_settings.filter % FILTER_MODES]);
        else
            snprintf(val, sizeof val, "%d%%", *value_of(r));
        text(px, pitch, x0 + 16, y0 + 24 + r * 12, row_name[r], c);
        if (r == row) text(px, pitch, x0 + 100, y0 + 24 + r * 12, "<", c);
        text(px, pitch, x0 + 112, y0 + 24 + r * 12, val, c);
        if (r == row) text(px, pitch, x0 + 160, y0 + 24 + r * 12, ">", c);
    }
    text(px, pitch, x0 + 22, y0 + h - 12, "F1 OR ESC TO CLOSE", 0x708090);
}
