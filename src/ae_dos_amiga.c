/* ae_dos_amiga.c - AmigaOS implementation of ae_dos, over dos.library.
 *
 * The twin of ae_dos_native.c. There is no "#ifdef AMIGA" here: the Makefile
 * compiling THIS file for the 68K target, and the other one for the host, is
 * the whole platform mechanism - the same arrangement ae_transport_amiga.c /
 * ae_transport_native.c already use.
 *
 * dos.library mapping, with the differences that actually bite:
 *
 *   ae_dos_open   -> Open() with MODE_OLDFILE (read; must exist),
 *                    MODE_NEWFILE (write; creates or truncates) or
 *                    MODE_READWRITE (append; creates, then Seek to the end).
 *                    MODE_READWRITE does NOT position at the end by itself -
 *                    that is the append bug waiting to happen, so the Seek is
 *                    done here and not left to callers.
 *   ae_dos_read   -> Read(), which returns -1 for an error and 0 at EOF, the
 *                    same three-way answer this API promises.
 *   ae_dos_seek   -> Seek(), whose origins are OFFSET_BEGINNING/CURRENT/END
 *                    (-1/0/1) and NOT stdio's 0/1/2, and which returns the
 *                    PREVIOUS position rather than the new one. Both of those
 *                    are translated here.
 *   ae_dos_size   -> Seek to the end, read the position, Seek back. Examine()
 *                    would also do it and needs a FileInfoBlock, which must be
 *                    longword-aligned and is 260 bytes of stack this door does
 *                    not need to spend.
 *   ae_dos_exists -> Lock()/UnLock(), which unlike the host's fopen() is TRUE
 *                    for a drawer as well as a file. Nothing in the SDK asks
 *                    about drawers; the difference is recorded in both files
 *                    rather than discovered later.
 *
 * IoErr() is deliberately not surfaced. A door cannot do anything useful with
 * ERROR_OBJECT_NOT_FOUND that it cannot do with NULL, and every extra return
 * code is one more thing a caller gets wrong.
 *
 * C89. ASCII only.
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include "ae_dos.h"

/* The handle table. Nothing allocates - see ae_dos.h. A zero `fh` is a free
 * slot; there is no separate in-use flag that could disagree with it. */
struct ae_file {
    BPTR fh;
};

static struct ae_file slots[AE_DOS_MAX_OPEN];

static struct ae_file *free_slot(void)
{
    int i;
    for (i = 0; i < AE_DOS_MAX_OPEN; i++) {
        if (slots[i].fh == 0) return &slots[i];
    }
    return 0;
}

ae_file *ae_dos_open(const char *path, int mode)
{
    struct ae_file *slot;
    LONG how;
    BPTR fh;

    if (path == 0 || path[0] == '\0') return 0;

    switch (mode) {
        case AE_DOS_READ:   how = MODE_OLDFILE;   break;
        case AE_DOS_WRITE:  how = MODE_NEWFILE;   break;
        case AE_DOS_APPEND: how = MODE_READWRITE; break;
        default: return 0;
    }

    slot = free_slot();
    if (slot == 0) return 0;

    fh = Open((STRPTR)path, how);
    if (fh == 0) return 0;

    /* MODE_READWRITE opens at the START. An append that skipped this would
     * overwrite the beginning of the file instead of extending it - on a
     * HeaderFile that is somebody's mail gone. */
    if (mode == AE_DOS_APPEND) {
        if (Seek(fh, 0, OFFSET_END) < 0) {
            Close(fh);
            return 0;
        }
    }

    slot->fh = fh;
    return slot;
}

long ae_dos_read(ae_file *f, void *buf, long len)
{
    LONG got;

    if (f == 0 || f->fh == 0 || buf == 0 || len < 0) return -1;
    if (len == 0) return 0;

    got = Read(f->fh, buf, (LONG)len);
    return (long)got;   /* -1 error, 0 EOF, else the count: already our contract */
}

long ae_dos_write(ae_file *f, const void *buf, long len)
{
    LONG put;

    if (f == 0 || f->fh == 0 || buf == 0 || len < 0) return -1;
    if (len == 0) return 0;

    put = Write(f->fh, (APTR)buf, (LONG)len);
    if (put != (LONG)len) return -1;
    return (long)put;
}

int ae_dos_seek(ae_file *f, long pos, int whence)
{
    LONG origin;

    if (f == 0 || f->fh == 0) return -1;

    switch (whence) {
        case AE_DOS_SET: origin = OFFSET_BEGINNING; break;
        case AE_DOS_CUR: origin = OFFSET_CURRENT;   break;
        case AE_DOS_END: origin = OFFSET_END;       break;
        default: return -1;
    }

    /* Seek() answers with the OLD position, and -1 for failure. A successful
     * seek to the very start of a file therefore answers 0, which is not an
     * error - testing "< 0" and not "!= 0" is the whole point. */
    if (Seek(f->fh, (LONG)pos, origin) < 0) return -1;
    return 0;
}

long ae_dos_tell(ae_file *f)
{
    LONG here;

    if (f == 0 || f->fh == 0) return -1;

    /* A zero-distance seek from the current position changes nothing and
     * returns where we were, which is the only way dos.library will say. */
    here = Seek(f->fh, 0, OFFSET_CURRENT);
    if (here < 0) return -1;
    return (long)here;
}

long ae_dos_size(ae_file *f)
{
    LONG here;
    LONG end;

    if (f == 0 || f->fh == 0) return -1;

    here = Seek(f->fh, 0, OFFSET_CURRENT);
    if (here < 0) return -1;

    if (Seek(f->fh, 0, OFFSET_END) < 0) return -1;
    end = Seek(f->fh, 0, OFFSET_CURRENT);

    /* Put the caller back where he was even if reading the end failed: asking
     * how big a file is has never been a request to be moved to the end of it. */
    if (Seek(f->fh, here, OFFSET_BEGINNING) < 0) return -1;
    if (end < 0) return -1;
    return (long)end;
}

void ae_dos_close(ae_file *f)
{
    if (f == 0 || f->fh == 0) return;
    Close(f->fh);
    f->fh = 0;
}

int ae_dos_exists(const char *path)
{
    BPTR lock;

    if (path == 0 || path[0] == '\0') return 0;

    lock = Lock((STRPTR)path, ACCESS_READ);
    if (lock == 0) return 0;
    UnLock(lock);
    return 1;
}

int ae_dos_delete(const char *path)
{
    if (path == 0 || path[0] == '\0') return -1;
    return DeleteFile((STRPTR)path) ? 0 : -1;
}

long ae_dos_lock(const char *path)
{
    BPTR lock;

    if (path == 0 || path[0] == '\0') return 0;

    lock = Lock((STRPTR)path, ACCESS_WRITE);
    if (lock == 0 && IoErr() == ERROR_OBJECT_NOT_FOUND) {
        /* express.e:11952 IF(error=205) THEN createFile(tempstr) */
        BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
        if (fh != 0) Close(fh);
        lock = Lock((STRPTR)path, ACCESS_WRITE);
    }
    return (long)lock;
}

void ae_dos_unlock(long lock)
{
    if (lock != 0) UnLock((BPTR)lock);
}
