/* ae_dos_native.c - stdio twin of ae_dos, so message-base code can be built
 * and tested on a developer's machine without an emulator.
 *
 * There is no "#ifdef AMIGA" in this file. The Makefile choosing THIS file
 * over ae_dos_amiga.c is the entire platform mechanism, exactly as
 * ae_transport_native.c is chosen over ae_transport_amiga.c.
 *
 * Mapping to the Amiga backend:
 *   ae_dos_open   -> fopen with "rb" / "wb" / "ab"
 *   ae_dos_read   -> fread   (short reads are normal; see ae_dos_read_full)
 *   ae_dos_write  -> fwrite
 *   ae_dos_seek   -> fseek, with AE_DOS_SET/CUR/END mapped to SEEK_*
 *   ae_dos_size   -> fseek to the end and back, position preserved
 *   ae_dos_exists -> fopen "rb", which unlike the Amiga's Lock() is FALSE for
 *                    a directory. Nothing in the SDK asks whether a drawer
 *                    exists, and a backend difference nobody exercises is a
 *                    trap left for later, so it is written down here instead
 *                    of pretended away.
 *
 * C89. ASCII only.
 */

#include <stdio.h>
#include <string.h>

#include "ae_dos.h"

/* The handle table. See ae_dos.h: nothing allocates, so a handle is a slot.
 * `fp == 0` means the slot is free - there is no separate in-use flag to fall
 * out of step with reality. */
struct ae_file {
    FILE *fp;
};

static struct ae_file slots[AE_DOS_MAX_OPEN];

static struct ae_file *free_slot(void)
{
    int i;
    for (i = 0; i < AE_DOS_MAX_OPEN; i++) {
        if (slots[i].fp == 0) return &slots[i];
    }
    return 0;
}

ae_file *ae_dos_open(const char *path, int mode)
{
    struct ae_file *slot;
    const char *how;
    FILE *fp;

    if (path == 0 || path[0] == '\0') return 0;

    switch (mode) {
        case AE_DOS_READ:   how = "rb"; break;
        case AE_DOS_WRITE:  how = "wb"; break;
        case AE_DOS_APPEND: how = "ab"; break;
        default: return 0;
    }

    /* Take the slot only once the file is really open, so a failed open does
     * not consume one of four. */
    slot = free_slot();
    if (slot == 0) return 0;

    fp = fopen(path, how);
    if (fp == 0) return 0;

    slot->fp = fp;
    return slot;
}

long ae_dos_read(ae_file *f, void *buf, long len)
{
    size_t got;

    if (f == 0 || f->fp == 0 || buf == 0 || len < 0) return -1;
    if (len == 0) return 0;

    got = fread(buf, 1, (size_t)len, f->fp);
    if (got == 0 && ferror(f->fp)) return -1;
    return (long)got;
}

long ae_dos_write(ae_file *f, const void *buf, long len)
{
    size_t put;

    if (f == 0 || f->fp == 0 || buf == 0 || len < 0) return -1;
    if (len == 0) return 0;

    put = fwrite(buf, 1, (size_t)len, f->fp);
    if (put != (size_t)len) return -1;
    return (long)put;
}

int ae_dos_seek(ae_file *f, long pos, int whence)
{
    int origin;

    if (f == 0 || f->fp == 0) return -1;

    switch (whence) {
        case AE_DOS_SET: origin = SEEK_SET; break;
        case AE_DOS_CUR: origin = SEEK_CUR; break;
        case AE_DOS_END: origin = SEEK_END; break;
        default: return -1;
    }
    return fseek(f->fp, pos, origin) == 0 ? 0 : -1;
}

long ae_dos_tell(ae_file *f)
{
    if (f == 0 || f->fp == 0) return -1;
    return ftell(f->fp);
}

long ae_dos_size(ae_file *f)
{
    long here;
    long end;

    if (f == 0 || f->fp == 0) return -1;

    here = ftell(f->fp);
    if (here < 0) return -1;
    if (fseek(f->fp, 0, SEEK_END) != 0) return -1;
    end = ftell(f->fp);
    /* Put the position back even if the size read failed: a caller that asks
     * how big a file is has not asked to be moved to the end of it. */
    if (fseek(f->fp, here, SEEK_SET) != 0) return -1;
    return end;
}

void ae_dos_close(ae_file *f)
{
    if (f == 0 || f->fp == 0) return;
    fclose(f->fp);
    f->fp = 0;
}

int ae_dos_exists(const char *path)
{
    FILE *fp;

    if (path == 0 || path[0] == '\0') return 0;
    fp = fopen(path, "rb");
    if (fp == 0) return 0;
    fclose(fp);
    return 1;
}

int ae_dos_delete(const char *path)
{
    if (path == 0 || path[0] == '\0') return -1;
    return remove(path) == 0 ? 0 : -1;
}

/*
 * Exclusive locks, the host's stand-in for AmigaDOS Lock(ACCESS_WRITE).
 *
 * A table of the paths held in this process: a second exclusive lock on a
 * held path is refused, as AmigaOS refuses it (ERROR_OBJECT_IN_USE) even to
 * the task that holds the first. That is what lets a test play "another node
 * is posting" with one call.
 */
#define AE_DOS_MAX_LOCKS 4
static char held_paths[AE_DOS_MAX_LOCKS][256];

long ae_dos_lock(const char *path)
{
    FILE *fp;
    int i;
    int free_at = -1;

    if (path == 0 || path[0] == '\0' || strlen(path) >= sizeof(held_paths[0])) return 0;

    for (i = 0; i < AE_DOS_MAX_LOCKS; i++) {
        if (held_paths[i][0] == '\0') { if (free_at < 0) free_at = i; continue; }
        if (strcmp(held_paths[i], path) == 0) return 0;
    }
    if (free_at < 0) return 0;

    /* createFile: made when missing, never truncated or written. */
    fp = fopen(path, "ab");
    if (fp == 0) return 0;
    fclose(fp);

    strcpy(held_paths[free_at], path);
    return (long)(free_at + 1);
}

void ae_dos_unlock(long lock)
{
    if (lock < 1 || lock > AE_DOS_MAX_LOCKS) return;
    held_paths[lock - 1][0] = '\0';
}
