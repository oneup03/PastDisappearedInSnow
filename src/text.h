#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "types.h"

/* The text path: languages, fonts and drawing. See src/text.c. */

/* Choose a language by its folder under translation/, or "ja" for the original. */
void TextSetLanguage(const char *language);

/* The machine has just loaded a part's image. Applies the language to it. */
void TextOnImageLoaded(int part);

/* The small capitals used in the game's graphics. */
enum { TEXT_TINY_COLUMNS = 3, TEXT_TINY_ROWS = 5 };
bool TextTinyPixel(char letter, int x, int y);

/* A file that ships beside the executable. */
void AssetPath(const char *leaf, char *path, size_t size);
