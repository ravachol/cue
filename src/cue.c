/* cue - a command-line music player
Copyright (C) 2022 Ravachol

http://github.com/ravachol/cue

 $$$$$$$\ $$\   $$\  $$$$$$\
$$  _____|$$ |  $$ |$$  __$$\
$$ /      $$ |  $$ |$$$$$$$$ |
$$ |      $$ |  $$ |$$   ____|
\$$$$$$$\ \$$$$$$  |\$$$$$$$\
 \_______| \______/  \_______|

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <stdio.h>
#include <pwd.h>
#include <sys/param.h>
#include <fcntl.h>
#include <stdbool.h>
#include <time.h>
#include <poll.h>
#include <dirent.h>
#include <signal.h>
#include <bits/sigaction.h>
#include <unistd.h>
#include "soundgapless.h"
#include "stringfunc.h"
#include "settings.h"
#include "printfunc.h"
#include "playlist.h"
#include "events.h"
#include "file.h"
#include "visuals.h"
#include "albumart.h"
#include "player.h"
#include "cache.h"
#include "songloader.h"

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

typedef struct
{
    char filePath[MAXPATHLEN];
    SongData *songdataA;
    SongData *songdataB;
    bool loadA;
    pthread_mutex_t mutex;
} LoadingThreadData;

bool playingMainPlaylist = false;
bool doQuit = false;
bool usingSongDataA = true;
bool loadingFailed = false;
bool skipPrev = false;
bool skipping = false;

struct timespec current_time;
struct timespec start_time;
struct timespec pause_time;

double elapsedSeconds = 0.0;
double pauseSeconds = 0.0;
double totalPauseSeconds = 0.0;

volatile bool loadedNextSong = false;
volatile bool songLoading = false;

UserData userData;
LoadingThreadData loadingdata;

Node *nextSong = NULL;
Node *prevSong = NULL;

#define COOLDOWN_DURATION 200

static clock_t lastInputTime = 0;

bool isCooldownElapsed()
{
    time_t currentTime = time(NULL);
    double elapsedSeconds = difftime(currentTime, lastInputTime);
    return elapsedSeconds >= COOLDOWN_DURATION / 1000.0;
}

void updateLastInputTime()
{
    lastInputTime = clock();
}

struct Event processInput()
{
    struct Event event;
    event.type = EVENT_NONE;
    event.key = '\0';

    if (!isInputAvailable())
    {
        saveCursorPosition();
        return event;
    }
    else
        restoreCursorPosition();

    char currentInput = '\0';

    while (isInputAvailable())
    {
        currentInput = readInput();
        usleep(50000);
    }

    if (!isCooldownElapsed())
        return event;

    updateLastInputTime();

    event.type = EVENT_NONE;
    event.key = currentInput;

    switch (event.key)
    {
    case 'q':
        event.type = EVENT_QUIT;
        break;
    case 's':
        event.type = EVENT_SHUFFLE;
        break;
    case 'c':
        event.type = EVENT_TOGGLECOVERS;
        break;
    case 'e':
        event.type = EVENT_TOGGLEVISUALIZER;
        break;
    case 'b':
        event.type = EVENT_TOGGLEBLOCKS;
        break;
    case 'a':
        event.type = EVENT_ADDTOMAINPLAYLIST;
        break;
    case 'd':
        event.type = EVENT_DELETEFROMMAINPLAYLIST;
        break;
    case 'r':
        event.type = EVENT_TOGGLEREPEAT;
        break;
    case 'p':
        event.type = EVENT_EXPORTPLAYLIST;
        break;
    case 'A': // Up arrow
        event.type = EVENT_VOLUME_UP;
        break;
    case 'B': // Down arrow
        event.type = EVENT_VOLUME_DOWN;
        break;
    case 'C': // Right arrow
        event.type = EVENT_NEXT;
        break;
    case 'D': // Left arrow
        event.type = EVENT_PREV;
        break;
    case ' ':
        event.type = EVENT_PLAY_PAUSE;
        break;
    case 'P': // F1
        refresh = true;
        printInfo = !printInfo;
        break;
    default:
        break;
    }
    return event;
}

void cleanup()
{
    cleanupPlaybackDevice();
    unloadSongData(&loadingdata.songdataA);
    unloadSongData(&loadingdata.songdataB);
    clearRestOfScreen();
}

void doShuffle()
{
    stopPlayListDurationCount();
    usleep(100000);
    playlist.totalDuration = 0.0;
    shufflePlaylistStartingFromSong(&playlist, currentSong);
    calculatePlayListDuration(&playlist);
    usleep(100000);
    loadedNextSong = false;
    refresh = true;
    nextSong = NULL;
}

void addToPlaylist()
{
    if (!playingMainPlaylist)
    {
        addToList(mainPlaylist, currentSong->song);
    }
}

void toggleBlocks()
{
    coverAnsi = !coverAnsi;
    strcpy(settings.coverAnsi, coverAnsi ? "1" : "0");
    if (coverEnabled)
        refresh = true;
}

void toggleCovers()
{
    coverEnabled = !coverEnabled;
    strcpy(settings.coverEnabled, coverEnabled ? "1" : "0");
    refresh = true;
}

void toggleRepeat()
{
    repeatEnabled = !repeatEnabled;
}

void toggleVisualizer()
{
    visualizerEnabled = !visualizerEnabled;
    strcpy(settings.visualizerEnabled, visualizerEnabled ? "1" : "0");
    restoreCursorPosition();
    refresh = true;
}

void togglePause(double *totalPauseSeconds, double pauseSeconds, struct timespec *pause_time)
{
    pausePlayback();
    if (isPaused())
        clock_gettime(CLOCK_MONOTONIC, pause_time);
    else
        *totalPauseSeconds += pauseSeconds;
}

void quit()
{
    doQuit = true;
}

void freeAudioBuffer()
{
    if (g_audioBuffer != NULL)
    {
        free(g_audioBuffer);
        g_audioBuffer = NULL;
    }
}

void resize()
{
    alarm(1); // Timer
    setCursorPosition(1, 1);
    clearRestOfScreen();
    while (resizeFlag)
    {
        usleep(100000);
    }
    alarm(0); // Cancel timer
    refresh = true;
    printf("\033[1;1H");
    clearRestOfScreen();
}

void assignLoadedData()
{
    if (usingSongDataA)
    {
        if (loadingdata.songdataB != NULL)
            userData.pcmFileB.filename = loadingdata.songdataB->pcmFilePath;
        else
            userData.pcmFileB.filename = NULL;
        userData.pcmFileB.file = NULL;
    }
    else
    {
        if (loadingdata.songdataA != NULL)
            userData.pcmFileA.filename = loadingdata.songdataA->pcmFilePath;
        else
            userData.pcmFileA.filename = NULL;
        userData.pcmFileA.file = NULL;
    }
}

void *songDataReaderThread(void *arg)
{
    LoadingThreadData *loadingdata = (LoadingThreadData *)arg;

    // Acquire the mutex lock
    pthread_mutex_lock(&(loadingdata->mutex));

    char filepath[MAXPATHLEN];
    strcpy(filepath, loadingdata->filePath);
    SongData *songdata = NULL;

    if (filepath[0] != '\0')
        songdata = loadSongData(filepath);
    else
        songdata = NULL;

    if (loadingdata->loadA)
    {
        unloadSongData(&loadingdata->songdataA);
        loadingdata->songdataA = songdata;
    }
    else
    {
        unloadSongData(&loadingdata->songdataB);
        loadingdata->songdataB = songdata;
    }

    assignLoadedData();

    loadedNextSong = true;
    skipping = false;
    songLoading = false;

    // Release the mutex lock
    pthread_mutex_unlock(&(loadingdata->mutex));

    return NULL;
}

void loadSong(Node *song, LoadingThreadData *loadingdata)
{
    if (song == NULL)
    {
        loadingFailed = true;
        loadedNextSong = true;
        skipping = false;
        songLoading = false;
        return;
    }

    strcpy(loadingdata->filePath, song->song.filePath);

    pthread_t loadingThread;
    pthread_create(&loadingThread, NULL, songDataReaderThread, (void *)loadingdata);
}

void loadNext(LoadingThreadData *loadingdata)
{
    nextSong = getListNext(currentSong);

    if (nextSong == NULL)
    {
        strcpy(loadingdata->filePath, "");
    }
    else
    {
        strcpy(loadingdata->filePath, nextSong->song.filePath);
    }

    pthread_t loadingThread;
    pthread_create(&loadingThread, NULL, songDataReaderThread, (void *)loadingdata);
}

bool isPlaybackOfListDone()
{
    return (userData.endOfListReached == 1);
}

void setPlaybackOfListDone()
{
    userData.endOfListReached = 1;
}

void prepareNextSong()
{
    if (songLoading)
        return;

    if (!skipPrev && !repeatEnabled)
        currentSong = currentSong->next;
    else
        skipPrev = false;

    if (currentSong == NULL)
    {
        setPlaybackOfListDone();
        quit();
        return;
    }

    elapsedSeconds = 0.0;
    pauseSeconds = 0.0;
    totalPauseSeconds = 0.0;

    loadedNextSong = false;
    nextSong = NULL;

    refresh = true;

    if (!repeatEnabled)
    {
        if (usingSongDataA)
        {
            unloadSongData(&loadingdata.songdataA);
            userData.pcmFileA.file = NULL;
            userData.pcmFileA.filename = NULL;
        }
        else
        {
            unloadSongData(&loadingdata.songdataB);
            userData.pcmFileB.file = NULL;
            userData.pcmFileB.filename = NULL;
        }
        usingSongDataA = !usingSongDataA;
    }
    clock_gettime(CLOCK_MONOTONIC, &start_time);
}

void skipToNextSong()
{
    if (songLoading || !loadedNextSong)
        return;

    if (currentSong->next == NULL)
    {
        return;
    }
    if (skipping)
        return;
    skipping = true;
    skip();
}

void skipToPrevSong()
{
    if (songLoading || !loadedNextSong || skipping)
        return;

    if (currentSong->prev == NULL)
    {
        return;
    }

    currentSong = currentSong->prev;
    skipping = true;
    skipPrev = true;
    loadedNextSong = false;
    songLoading = true;

    if (usingSongDataA)
    {
        loadingdata.loadA = false;
        unloadSongData(&loadingdata.songdataB);
    }
    else
    {
        loadingdata.loadA = true;
        unloadSongData(&loadingdata.songdataA);
    }
    loadSong(currentSong, &loadingdata);

    while (!loadedNextSong && !loadingFailed)
    {
        usleep(10000);
    }
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    skip();
}

void calcElapsedTime()
{
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    if (!isPaused())
    {
        elapsedSeconds = (double)(current_time.tv_sec - start_time.tv_sec) +
                         (double)(current_time.tv_nsec - start_time.tv_nsec) / 1e9;
        elapsedSeconds -= totalPauseSeconds;
    }
    else
    {
        pauseSeconds = (double)(current_time.tv_sec - pause_time.tv_sec) +
                       (double)(current_time.tv_nsec - pause_time.tv_nsec) / 1e9;
    }
}

void refreshPlayer()
{
    if (usingSongDataA)
        printPlayer(loadingdata.songdataA, elapsedSeconds, &playlist);
    else
        printPlayer(loadingdata.songdataB, elapsedSeconds, &playlist);
}

void loadAudioData()
{
    if (nextSong == NULL && !songLoading)
    {
        songLoading = true;
        loadingdata.loadA = !usingSongDataA;
        loadNext(&loadingdata);
    }
}

void handleInput()
{
    struct Event event = processInput();

    switch (event.type)
    {
    case EVENT_PLAY_PAUSE:
        togglePause(&totalPauseSeconds, pauseSeconds, &pause_time);
        break;
    case EVENT_TOGGLEVISUALIZER:
        toggleVisualizer();
        break;
    case EVENT_TOGGLEREPEAT:
        toggleRepeat();
        break;
    case EVENT_TOGGLECOVERS:
        toggleCovers();
        break;
    case EVENT_TOGGLEBLOCKS:
        toggleBlocks();
        break;
    case EVENT_SHUFFLE:
        doShuffle();
        break;
    case EVENT_QUIT:
        quit();
        break;
    case EVENT_VOLUME_UP:
        adjustVolumePercent(2);
        break;
    case EVENT_VOLUME_DOWN:
        adjustVolumePercent(-2);
        break;
    case EVENT_NEXT:
        skipToNextSong();
        break;
    case EVENT_PREV:
        skipToPrevSong();
        break;
    case EVENT_ADDTOMAINPLAYLIST:
        addToPlaylist();
        break;
    case EVENT_DELETEFROMMAINPLAYLIST:
        // FIXME implement this
        break;
    case EVENT_EXPORTPLAYLIST:
        savePlaylist();
        break;
    default:
        break;
    }
}

void updatePlayer()
{
    if (resizeFlag)
        resize();
    else
        refreshPlayer();
}

void play(Node *currentSong)
{
    processInput(); // Ignore any input that's already accumulated
    freeAudioBuffer();
    pthread_mutex_init(&(loadingdata.mutex), NULL);
    usingSongDataA = true;
    loadingdata.loadA = true;
    loadSong(currentSong, &loadingdata);
    int i = 0;
    while (!loadedNextSong)
    {
        usleep(10000);
        if (i % 100 == 0)
            printf(".");
        i++;
        fflush(stdout);
    }
    userData.currentFileIndex = 0;
    userData.currentPCMFrame = 0;
    userData.pcmFileA.filename = loadingdata.songdataA->pcmFilePath;

    createAudioDevice(&userData);

    loadedNextSong = false;
    nextSong = NULL;
    refresh = true;

    clock_gettime(CLOCK_MONOTONIC, &start_time);
    calculatePlayListDuration(&playlist);

    while (true)
    {
        calcElapsedTime();
        handleInput();
        updatePlayer();

        if (!loadedNextSong)
            loadAudioData();

        if (isPlaybackDone())
        {
            while (!loadedNextSong)
            {
                usleep(10000);
            }
            prepareNextSong();
        }

        if (doQuit || isPlaybackOfListDone() || loadingFailed)
        {
            break;
        }
        usleep(100000);
    }
    return;
}

int run()
{
    if (playlist.head == NULL)
    {
        showCursor();
        restoreTerminalMode();
        enableInputBuffering();
        return -1;
    }
    if (currentSong == NULL)
        currentSong = playlist.head;
    play(currentSong);
    cleanup();
    restoreTerminalMode();
    enableInputBuffering();
    setConfig();
    saveMainPlaylist(settings.path, playingMainPlaylist);
    freeAudioBuffer();
    deleteCache(tempCache);
    deleteTempDir();
    deletePlaylist(&playlist);
    deletePlaylist(mainPlaylist);
    free(mainPlaylist);
    showCursor();
    printf("\n");

    return 0;
}

void initResize()
{
    signal(SIGWINCH, handleResize);

    struct sigaction sa;
    sa.sa_handler = resetResizeFlag;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);
}

void init()
{
    disableInputBuffering();
    srand(time(NULL));
    freopen("/dev/null", "w", stderr);
    initResize();
    enableScrolling();
    setNonblockingMode();
    srand(time(NULL));
    tempCache = createCache();
    strcpy(loadingdata.filePath, "");
    loadingdata.songdataA = NULL;
    loadingdata.songdataB = NULL;
    loadingdata.loadA = false;
}

void playMainPlaylist()
{
    if (mainPlaylist->count == 0)
    {
        printf("Couldn't find any songs in the main playlist. Add a song by pressing 'a' while it's playing. \n");
        exit(0);
    }
    playingMainPlaylist = true;
    playlist = deepCopyPlayList(mainPlaylist);
    shufflePlaylist(&playlist);
    init();
    run();
}

void playAll(int argc, char **argv)
{
    init();
    makePlaylist(argc, argv);
    if (playlist.count == 0)
    {
        printf("Please make sure the path is set correctly. \n");
        printf("To set it type: cue path \"/path/to/Music\". \n");
    }
    run();
}

int main(int argc, char *argv[])
{
    getConfig();
    loadMainPlaylist(settings.path);

    if (argc == 1)
    {
        playAll(argc, argv);
    }
    else if (strcmp(argv[1], ".") == 0)
    {
        playMainPlaylist();
    }
    else if ((argc == 2 && ((strcmp(argv[1], "--help") == 0) || (strcmp(argv[1], "-h") == 0) || (strcmp(argv[1], "-?") == 0))))
    {
        showHelp();
    }
    else if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0))
    {
        printAboutDefaultColors();
    }
    else if (argc == 3 && (strcmp(argv[1], "path") == 0))
    {
        strcpy(settings.path, argv[2]);
        setConfig();
    }
    else if (argc >= 2)
    {
        init();
        makePlaylist(argc, argv);
        run();
    }
    return 0;
}