/* ae_field_shape.c - which board fields answer in Data, not String.
 *
 * express.e's CASE over the field numbers (express.e:3800-3970) answers most
 * fields with StringF into the message's String - including the ones that look
 * numeric (BB_CONFNUM is express.e:3832's StringF(tempstring,'\d',
 * currentConf-1), a string). A handful answer in msg.data instead and write
 * nothing into String. A door that reads String for one of those reads an
 * empty answer and takes its fallback, silently.
 *
 * AN EXPLICIT LIST, not a range and not "String came back empty". A range is
 * wrong at the first field it meets (the 5xx band holds both shapes). And
 * "empty String means read Data" is worse than wrong - a String field that
 * legitimately answers with nothing (an empty location) leaves Data holding
 * the 1 the door put there as the read flag, so the door would be handed the
 * string "1" as the caller's home town.
 *
 * The cost of a missing entry was measured twice:
 * - BB_SCRWIDTH: ae_screen_cols() answered 80 on every board and every caller,
 *   so every C door's 40-column tier was dead code.
 * - DT_ISANSI: ae_user_is_ansi() answered 0 for every caller, so every C door
 *   believed colour was off. ui_screen then turned on caret selection, and the
 *   menu door drew its selected row as ">>" in selection ink on the ground with
 *   no selection bar (sysop, 2026-10-17: "the selected rows have no bg color").
 *
 * C89. ASCII only.
 */

#include "ae_field_shape.h"

int ae_field_answers_in_data(int field)
{
    switch (field) {
        /* express.e:3861-3868: CASE BB_SCRLEFT/BB_SCRTOP/BB_SCRWIDTH/BB_SCRHEIGHT -> msg.data:=screen.* */
        case 518:   /* BB_SCRLEFT */
        case 519:   /* BB_SCRTOP */
        case 520:   /* BB_SCRWIDTH */
        case 521:   /* BB_SCRHEIGHT */
        /* express.e:3907-3908: CASE DT_ISANSI -> IF ansiColour THEN msg.data:=1 ELSE msg.data:=0 */
        case 541:   /* DT_ISANSI */
        /* express.e:3959-3962: msg.data:=customMsgParam1 / customMsgParam2 */
        case 600:   /* GET_CUSTOM_MSGBASE_PARAM1 */
        case 601:   /* GET_CUSTOM_MSGBASE_PARAM2 */
        /* express.e:3963-3966: msg.data:=lastMsgReadConf / lastNewReadConf */
        case 602:   /* LAST_READ */
        case 603:   /* LAST_SCANNED */
            return 1;
        default:
            return 0;
    }
}
