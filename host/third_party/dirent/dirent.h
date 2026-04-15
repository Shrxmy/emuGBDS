#ifndef EMUGBDS_DIRENT_COMPAT_H
#define EMUGBDS_DIRENT_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DIR DIR;

struct dirent {
    char d_name[260];
    unsigned char d_type;
};

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8

static inline DIR* opendir(const char* path) {
    (void)path;
    return (DIR*)0;
}

static inline struct dirent* readdir(DIR* dirp) {
    (void)dirp;
    return (struct dirent*)0;
}

static inline int closedir(DIR* dirp) {
    (void)dirp;
    return 0;
}

static inline void rewinddir(DIR* dirp) {
    (void)dirp;
}

#ifdef __cplusplus
}
#endif

#endif
