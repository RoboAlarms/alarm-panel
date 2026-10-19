/*
 * link_console.c - see link_console.h. Win32 threads on Windows (no extra runtime DLL), POSIX
 * threads elsewhere.
 */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <time.h>
#endif

#include "link_console.h"

#include <stdio.h>
#include <string.h>

#define QUEUE 16u

static struct {
#if defined(_WIN32)
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    char line[QUEUE][LINK_CONSOLE_LINE];
    unsigned head, count;
    bool eof;
} C;

static void lock(void)
{
#if defined(_WIN32)
    EnterCriticalSection(&C.lock);
#else
    pthread_mutex_lock(&C.lock);
#endif
}

static void unlock(void)
{
#if defined(_WIN32)
    LeaveCriticalSection(&C.lock);
#else
    pthread_mutex_unlock(&C.lock);
#endif
}

static void push(const char *s)
{
    for (;;) {
        lock();
        if (C.count < QUEUE) {
            char *dst = C.line[(C.head + C.count) % QUEUE];
            snprintf(dst, LINK_CONSOLE_LINE, "%s", s);
            C.count++;
            unlock();
            return;
        }
        unlock();
        /* The main loop takes a line every 50 ms at most: wait for room. */
#if defined(_WIN32)
        Sleep(20);
#else
        const struct timespec ts = {0, 20000000L};
        nanosleep(&ts, NULL);
#endif
    }
}

static void reader(void)
{
    char buf[LINK_CONSOLE_LINE];
    while (fgets(buf, (int)sizeof(buf), stdin) != NULL) {
        size_t n = strlen(buf);
        while (n > 0u && (buf[n - 1u] == '\n' || buf[n - 1u] == '\r')) {
            buf[--n] = '\0';
        }
        push(buf);
    }
    lock();
    C.eof = true;
    unlock();
}

#if defined(_WIN32)
static DWORD WINAPI reader_thread(LPVOID arg)
{
    (void)arg;
    reader();
    return 0;
}
#else
static void *reader_thread(void *arg)
{
    (void)arg;
    reader();
    return NULL;
}
#endif

bool link_console_start(void)
{
    memset(C.line, 0, sizeof(C.line));
    C.head = 0u;
    C.count = 0u;
    C.eof = false;
#if defined(_WIN32)
    InitializeCriticalSection(&C.lock);
    HANDLE t = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
    if (t == NULL) {
        return false;
    }
    CloseHandle(t);
    return true;
#else
    if (pthread_mutex_init(&C.lock, NULL) != 0) {
        return false;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, reader_thread, NULL) != 0) {
        return false;
    }
    pthread_detach(t);
    return true;
#endif
}

bool link_console_line(char *out, size_t cap)
{
    bool got = false;
    lock();
    if (C.count > 0u) {
        snprintf(out, cap, "%s", C.line[C.head]);
        C.head = (C.head + 1u) % QUEUE;
        C.count--;
        got = true;
    }
    unlock();
    return got;
}

bool link_console_closed(void)
{
    lock();
    const bool closed = C.eof && C.count == 0u;
    unlock();
    return closed;
}
