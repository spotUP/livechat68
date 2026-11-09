/*
 * ae_session_bbs - a session that talks to the real board.
 *
 * ae_session is deliberately transport-agnostic: a test hands it a table, a
 * door hands it this. Three lines of glue, kept in their own module so a
 * door that never opens a session does not link the AEDoor protocol - the
 * whole point of the layering (see tools/measure-link.sh).
 */

#include "ae_session.h"
#include "aedoor.h"

/** One field, read through the AEDoor round trip. */
static int bbs_read(void *context, ae_field field, char *out, int cap)
{
    (void)context;
    return ae_field_read((int)field, out, cap);
}

/** One field, written the same way. */
static int bbs_write(void *context, ae_field field, const char *value)
{
    (void)context;
    return ae_field_write((int)field, value);
}

/** One ACS question, through DT_QUERYBIT. */
static int bbs_query_bit(void *context, int acs)
{
    (void)context;
    return ae_query_bit(acs);
}

/**
 * Open a session on the board this door is running under.
 *
 * The caller still owns the storage, for the reason ae_session.h gives: a
 * library that kept its own would stop two subsystems in one door from both
 * talking to the BBS.
 */
int ae_open_bbs(ae_session *s, char *storage, long cap, int node)
{
    int rc = ae_open(s, bbs_read, 0, storage, cap, node);
    if (rc != 0) return rc;

    ae_set_writer(s, bbs_write);
    ae_set_security_query(s, bbs_query_bit);
    return 0;
}
