// KallistiOS's newlib has no access(), which raylib's FileExists() needs.
// Back it with KOS's own fs_open so file checks (e.g. save slots) work.
#include <fcntl.h>
#include <kos/fs.h>

int access(const char *path, int mode)
{
    (void)mode;
    file_t f = fs_open(path, O_RDONLY);
    if (f == FILEHND_INVALID) return -1;
    fs_close(f);
    return 0;
}
