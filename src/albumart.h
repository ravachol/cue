#ifndef ALBUMART_H
#define ALBUMART_H
#include <stdbool.h>
#include <dirent.h>
#include "../include/imgtotxt/write_ascii.h"
#include "albumart.h"

int displayAlbumArt(const char *filepath, int width, int height, bool coverBlocks, PixelData *brightPixel);

int calcIdealImgSize(int *width, int *height, const int equalizerHeight, const int metatagHeight, bool firstSong);

#endif