#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#ifndef DEBUG_H

#define DEBUG_H

#define FATAL 1
#define OKISH 0

#define ERROR_STRING "\n\n======EROARE======\nLINIA: %d, FISIERUL: %s\n\n"

#define CHECK(cond, msg, flag)                                  \
    do{                                                         \
        if ((cond)) {                                           \
            fprintf(stderr, ERROR_STRING, __LINE__, __FILE__);  \
            perror((msg));                                      \
            if ((flag) == FATAL) {                              \
                exit(1);                                        \
            }                                                   \
        }                                                       \
    } while (0);

#ifdef DEBUG


#define DWRITE(buf, size)                               \
    do{                                                 \
        fprintf(stderr, "-----MESSAGE-----\n");         \
        fprintf(stderr, "====DATA BEGIN====\n*\n");     \
        write(STDERR_FILENO, buf, size);                \
        fprintf(stderr, "\n*\n====DATA END====\n\n");   \
        fprintf(stderr, "====BYTES BEGIN====\n*\n");    \
        for (int i = 0; i < size; i++) {                \
            fprintf(stderr, "%.2x ", buf[i]);           \
            if (i % 20 == 0 && i > 0) {                 \
                fprintf(stderr, "\n");                  \
            }                                           \
        }                                               \
        fprintf(stderr, "\n*\n====BYTES END====\n");    \
        fprintf(stderr, "-----MESSAGE END-----\n\n\n"); \
    } while (0);

#else

#define DWRITE

#endif

#endif