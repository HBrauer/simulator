#ifndef UI_TEXT_H
#define UI_TEXT_H

#include <SDL2/SDL.h>

/* Minimal embedded 5x7 bitmap font for toolbar widgets (digits, uppercase letters,
 * and basic punctuation; lowercase maps to uppercase). No external font dependency. */

#define UI_TEXT_GLYPH_W 5
#define UI_TEXT_GLYPH_H 7
#define UI_TEXT_ADVANCE 6 /* glyph width + 1 column spacing, in font pixels */

/* Rendered text width in screen pixels. */
int ui_text_width(int scale, const char *text);

/* Rendered text height in screen pixels. */
int ui_text_height(int scale);

/* Draw text at (x, y) using the current render draw color. */
void ui_text_draw(SDL_Renderer *renderer, int x, int y, int scale, const char *text);

#endif
