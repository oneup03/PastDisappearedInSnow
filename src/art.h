#pragma once

/* Read the language's art.txt, when a part's image loads. */
void ArtLoad(const char *language, int part);

/* Each frame: put the language's text over Japanese in the picture on screen. */
void ArtOnFrame(void);
