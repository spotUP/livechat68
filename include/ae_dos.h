/*
 * ae_dos - file access for a C door, on AmigaOS and on the host.
 *
 * The SDK had none. aedoor.h is the console and the field protocol; every
 * module above it either draws or talks to the board. That was enough while
 * doors asked the BOARD for their data, and it stops being enough the moment a
 * door has to read a message base: a real AmiExpress answers no XIM command
 * that lists or reads a message, so a door that wants messages opens
 * `<conf>/MsgBase/HeaderFile` itself, exactly as Doors/Conftop and
 * Doors/Request already do.
 *
 * WHY THIS EXISTS AT ALL, rather than dos.library calls inline in the door:
 * because then the door only builds for 68K, and every test of it needs an
 * emulator. This file is the same two-backend split the transport layer
 * already uses - ae_dos_amiga.c calls dos.library, ae_dos_native.c calls
 * stdio, and the Makefile picking one of them IS the platform mechanism.
 * There is no "#ifdef AMIGA" in any caller, and the message-base tests run on
 * a developer's machine in milliseconds.
 *
 * NOTHING ALLOCATES. Handles come from a fixed table of AE_DOS_MAX_OPEN
 * entries per backend; ae_dos_open returns a pointer into that table or NULL
 * when the table is full. A door that leaks handles runs out and is told so,
 * which is a better failure than an Amiga with no free memory.
 *
 * BYTE ORDER IS EXPLICIT. AmiExpress structures on disk are big-endian
 * because the machine that wrote them was. On 68K that happens to match the
 * CPU and on the host it does not, so nothing anywhere may cast a buffer to a
 * struct and read a LONG out of it - that is the single mistake that would
 * make this module produce right answers in the tests and wrong ones on the
 * board. ae_dos_get_long/word take bytes and shift. They are in ae_dos.c,
 * compiled on both backends, because they are arithmetic and have no platform.
 *
 * C89. ASCII only.
 */

#ifndef AE_DOS_H
#define AE_DOS_H

#ifdef __cplusplus
extern "C" {
#endif

/** How many files one door may hold open at once.
 *
 * Four is what the message reader actually needs at its peak: HeaderFile,
 * MailStats, one body, and MsgBases.info. It is a table size, not a policy -
 * raise it here if a door legitimately needs more, and never by leaking. */
#define AE_DOS_MAX_OPEN 4

/* Open modes. */
#define AE_DOS_READ   0   /* must exist */
#define AE_DOS_WRITE  1   /* created, or truncated if it exists */
#define AE_DOS_APPEND 2   /* created if missing, position starts at the end */

/* Seek origins, matching stdio's SEEK_* order rather than AmigaDOS's, because
 * a caller reads this header and not dos.library's autodocs. */
#define AE_DOS_SET 0
#define AE_DOS_CUR 1
#define AE_DOS_END 2

/** An open file. Opaque: the two backends hold entirely different things. */
typedef struct ae_file ae_file;

/**
 * Open `path` in one of the AE_DOS_* modes.
 *
 * Returns a handle, or NULL when the file cannot be opened OR when
 * AE_DOS_MAX_OPEN handles are already out. A caller cannot tell those apart
 * and does not need to: both mean "not today", and the door says so rather
 * than guessing at the contents.
 *
 * `path` is passed to the platform verbatim, so an Amiga path
 * ("BBS:Conf3/MsgBase/HeaderFile") works on the Amiga backend and a POSIX path
 * works on the host. Nothing here rewrites separators - a door gets its paths
 * from the board (XIM 604 / 127) and must not invent them.
 */
ae_file *ae_dos_open(const char *path, int mode);

/**
 * Read up to `len` bytes. Returns the count read, 0 at end of file, or -1.
 *
 * A SHORT READ IS NOT AN ERROR and not necessarily the end - callers that need
 * exactly N bytes must loop, which is why ae_dos_read_full exists below rather
 * than every caller reinventing that loop and half of them getting it wrong.
 */
long ae_dos_read(ae_file *f, void *buf, long len);

/**
 * Read exactly `len` bytes, looping over short reads.
 *
 * Returns 1 when all `len` bytes arrived, 0 at a clean end of file before any
 * byte was read, and -1 on a real error or a partial record. A 110-byte header
 * record that arrives 60 bytes long is a truncated file, not a header, and the
 * distinction matters enough to be in the return value.
 */
int ae_dos_read_full(ae_file *f, void *buf, long len);

/** Write `len` bytes. Returns the count written, or -1. */
long ae_dos_write(ae_file *f, const void *buf, long len);

/**
 * Move the read/write position. `whence` is AE_DOS_SET/CUR/END.
 * Returns 0 on success, -1 on failure.
 */
int ae_dos_seek(ae_file *f, long pos, int whence);

/** Current position, or -1. */
long ae_dos_tell(ae_file *f);

/**
 * Total size in bytes, or -1.
 *
 * Leaves the position where it found it. The message reader divides this by
 * 110 to count records before reading any, so a size call that quietly moved
 * the file pointer would make the first record read come out of the middle of
 * the file.
 */
long ae_dos_size(ae_file *f);

/** Close and release the table entry. NULL is accepted and ignored. */
void ae_dos_close(ae_file *f);

/** Non-zero when `path` exists. Cheaper than open/close and works on a drawer. */
int ae_dos_exists(const char *path);

/** Delete `path`. Returns 0 on success, -1 on failure. */
int ae_dos_delete(const char *path);

/**
 * Take an EXCLUSIVE lock on `path`, creating the file when it is missing -
 * express.e's lockMsgBase (express.e:11944-11964), which is
 * `Lock(<msgBase>MailLock, ACCESS_WRITE)` and `createFile` on IoErr 205.
 *
 * The file is only the thing being locked: nothing is written into it, and
 * nobody deletes it. Whether the base is busy is whether somebody holds the
 * handle, and AmigaOS drops a handle when its holder dies - so a killed door
 * cannot leave the base locked, and two writers never have to agree on a
 * clock (the timestamp this replaces did not: 2026-10-04 SUBS proposal,
 * conflict 1).
 *
 * Returns a non-zero handle, or 0 when somebody else holds it (or the file
 * cannot be made). One try: the caller owns the waiting.
 */
long ae_dos_lock(const char *path);

/** Release a lock ae_dos_lock gave. 0 is accepted and ignored. */
void ae_dos_unlock(long lock);

/* ---- big-endian accessors ---------------------------------------------
 *
 * These take a byte pointer, not a struct, and shift. See the byte-order note
 * at the top of this file: on 68K a struct cast would work and on the host it
 * would not, which is the worst possible combination because the tests would
 * pass.
 *
 * ae_dos_get_long returns a signed 32-bit value in a long, which is 32 bits on
 * the Amiga and 64 on the host - so it is sign-extended explicitly rather than
 * left to the width of the type. */
long ae_dos_get_long(const unsigned char *p);
int  ae_dos_get_word(const unsigned char *p);
void ae_dos_put_long(unsigned char *p, long value);
void ae_dos_put_word(unsigned char *p, int value);

#ifdef __cplusplus
}
#endif

#endif /* AE_DOS_H */
