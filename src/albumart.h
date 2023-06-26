#ifndef ALBUMART_H
#define ALBUMART_H
#include <stdbool.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/param.h>
#include "../include/imgtotxt/write_ascii.h"

extern char coverArtFilePath[MAXPATHLEN];

int displayAlbumArt(const char *filepath, int width, int height, bool coverBlocks, PixelData *brightPixel);

int calcIdealImgSize(int *width, int *height, const int equalizerHeight, const int metatagHeight, bool firstSong);

void cleanupCover();

#endif