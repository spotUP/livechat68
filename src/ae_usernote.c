/* ae_usernote.c - the client for AEW_USERNOTE (11200). C89. See ae_usernote.h. */
#include <string.h>
#include "aedoor.h"
#include "ae_session.h"
#include "ae_usernote.h"

int ae_user_note(const char *username, const char *text)
{
    char request[AE_MAX_LINE + 1];
    char reply[AE_MAX_LINE + 1];
    int n = 0, i, got;
    if (!username || !text) return 0;
    /* "NOTE" TAB username TAB text; a TAB or control inside a field would break the split, so it becomes a space. */
    memcpy(request, "NOTE\t", 5);
    n = 5;
    for (i = 0; username[i] && n < AE_MAX_LINE - 2; i++)
        request[n++] = ((unsigned char)username[i] < 0x20 || username[i] == 0x7f) ? ' ' : username[i];
    request[n++] = '\t';
    for (i = 0; text[i] && n < AE_MAX_LINE; i++)
        request[n++] = ((unsigned char)text[i] < 0x20 || text[i] == 0x7f) ? ' ' : text[i];
    request[n] = '\0';
    got = ae_field_exchange(AE_FIELD_USERNOTE, request, reply, (int)sizeof(reply));
    if (got <= 0 || !reply[0]) return -1;
    return strcmp(reply, "OK") == 0 ? 1 : 0;
}
