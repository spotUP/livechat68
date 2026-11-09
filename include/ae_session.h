/*
 * ae_session - who is calling, and on what screen.
 *
 * Phase 1 of the C SDK. A door could already ask WHERE it was running
 * (ae_host.h); this is the other half of the question - who is on the other
 * end, how much room they have, and whether the line is still up.
 *
 * CALLER-OWNED STORAGE, on purpose. The existing C door keeps its BBS
 * message buffer in a `static` (examples/doorrepo-c/aedoor_amiga.c:199-204),
 * which means two subsystems in one door cannot both talk to the board. Here
 * the caller hands over the memory and the library keeps no globals of its
 * own, so a door may open a second session for a background task without the
 * two treading on each other.
 *
 * THE TRANSPORT IS A SEAM. On the Amiga a field is one AEDoor round trip; on
 * a host build it is a table a test filled in. Same accessors either way,
 * which is what lets phase 1 be tested at all without an emulator.
 */

#ifndef AE_SESSION_H
#define AE_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

/** The fields a door may ask for, with the AEDoor numbers they map to. */
typedef enum {
    AE_FIELD_USER_NAME      = 100,  /* DT_NAME */
    AE_FIELD_USER_LOCATION  = 102,  /* DT_LOCATION */
    AE_FIELD_USER_LEVEL     = 105,  /* DT_SECSTATUS */
    AE_FIELD_TIME_LEFT      = 115,  /* DT_TIMELIMIT */
    /*
     * 541, not the 123 the plan wrote down: 123 is ACTIVE_NODES, and a door
     * asking it would have got a node count and read it as "yes, ANSI".
     * Checked against the board's own table (xim/types.ts) and against
     * axcommon.e:230, which is where that table came from.
     */
    AE_FIELD_IS_ANSI        = 541,  /* DT_ISANSI */
    AE_FIELD_SCREEN_COLS    = 520,  /* BB_SCRWIDTH */
    AE_FIELD_SCREEN_ROWS    = 521,  /* BB_SCRHEIGHT */
    AE_FIELD_CONFERENCE     = 510,  /* BB_CONFNUM */
    /* What the conference is CALLED, for a masthead. Not in the message base:
     * a message base is headers and bodies and knows nothing about it. */
    AE_FIELD_CONF_NAME      = 126,  /* BB_CONFNAME */
    /*
     * WHERE THE CALLER'S CONFERENCE AND ITS MESSAGES LIVE ON DISK.
     *
     * These two are what let a door read a message base for itself rather
     * than asking the board to decode one for it - and they are ORDINARY
     * AmiExpress, not this port's inventions, which is the entire point. A
     * real AmiExpress answers no command that lists or reads a message; a
     * door that wants messages opens <conf>/MsgBase/HeaderFile, and these say
     * where that is. Doors/Conftop and Doors/Request already do exactly this.
     *
     * MSGBASE_LOC answers the conference's DEFAULT base. A conference with
     * several (MsgBases.info: NMSGBASES, NAME.n, LOCATION.n - express.e:2048
     * and ~2068) needs the door to read that file and resolve LOCATION.n
     * itself; CONF_LOCATION is the directory to read it from.
     *
     * Both answer in String, so they need no entry in the transport's
     * field_answers_in_data() - unlike the two pointers below.
     */
    AE_FIELD_CONF_LOCATION  = 127,  /* BB_CONFLOCAL */
    AE_FIELD_MSGBASE_LOC    = 604,  /* GET_MSGBASE_LOC */
    /*
     * THE CALLER'S READ POINTERS - express.e:3963-3966, both answered in
     * Data:
     *
     *     CASE LAST_READ    -> msg.data:=lastMsgReadConf
     *     CASE LAST_SCANNED -> msg.data:=lastNewReadConf
     *
     * A door asks the BOARD for these rather than parsing <conf>/Conf.DB,
     * whose 74-byte records are indexed by the caller's account SLOT - a
     * number the door interface does not expose at all. Asking also stays
     * right on a board that keeps its pointers somewhere else entirely.
     */
    AE_FIELD_LAST_READ      = 602,  /* LAST_READ */
    AE_FIELD_LAST_SCANNED   = 603,  /* LAST_SCANNED */
    /*
     * WHICH COMMAND NAME LAUNCHED THIS DOOR - express.e:3974-3975,
     * AstrCopy(msg.string, customMsgCmd, 200).
     *
     * The only way one binary can behave differently under two registrations,
     * because AmigaDOS gives a door ONE argument string and only argv[1]
     * survives (sdk/c/README.md:132-143) - and argv[1] is the node number.
     * SUBS uses it to tell its full screen from its three-line digest.
     */
    AE_FIELD_MENU_COMMAND   = 605,  /* GET_CUSTOM_MSGBASE_MENUCMD */
    /*
     * THE WHOLE LINE THAT LAUNCHED THE DOOR, command and parameters:
     * express.e:3794-3800 answers "<command> <params>", or the command alone.
     * MENU_COMMAND above is the name only; this is what carries what the
     * caller typed after it (the palette's starting filter).
     */
    AE_FIELD_MAIN_LINE      = 131,  /* BB_MAINLINE */
    /*
     * WHY THIS DOOR WAS LAUNCHED, when the launch was a conference handing
     * over its message base - express.e:3959-3962, both answered in Data:
     *
     *     CASE GET_CUSTOM_MSGBASE_PARAM1 -> msg.data:=customMsgParam1
     *     CASE GET_CUSTOM_MSGBASE_PARAM2 -> msg.data:=customMsgParam2
     *
     * A conference carrying the tooltype CUSTOM routes every message command
     * to one door (customMsgbaseCmd, express.e:12426-12437). PARAM1 says
     * WHICH - read, enter, scan, stats, comment, zoom - and PARAM2 says which
     * CONFERENCE, which during `MS` is not the one the caller is standing in.
     *
     * ZERO means "you were not launched as a message base", which is what a
     * board with no custom conference answers and what express.e's own globals
     * hold (:273-274). Branch on this, not on the command NAME: 605 carries
     * the command LINE on a real Amiga and only this port's extension makes it
     * a registration name.
     */
    AE_FIELD_MSGBASE_FUNC   = 600,  /* GET_CUSTOM_MSGBASE_PARAM1 */
    AE_FIELD_MSGBASE_CONF   = 601,  /* GET_CUSTOM_MSGBASE_PARAM2 */
    /*
     * THE BOARD'S CLOCK - express.e:3771-3772,
     * `StringF(tempstring,'\d',getSystemTime())`. NOT Unix seconds:
     * getSystemTime is the local wall clock counted from 1970 as if it were
     * UTC, plus 21600 (MiscFuncs.e:463-469) - the form a HeaderFile's
     * msgDate holds, which is what a door wants it for.
     *
     * A door that wrote a message header needs a date, and must not reach for
     * time() to get one: on AmigaOS the C library's epoch is not reliably the
     * Unix one, so a door built with the wrong toolchain would stamp every
     * message eight years out and nobody would notice until the list was
     * sorted. The board already knows, and it is the same clock express.e
     * stamps its own messages with.
     */
    AE_FIELD_CURR_TIME      = 145,  /* DT_CURR_TIME */
    /*
     * THIS BOARD'S OWN, above express.e's MAX_CMD (1003).
     *
     * A classic board answers an unknown command with nothing, which is the
     * signal to fall back - and a door should have checked AE_HOST first
     * anyway (ae_host.h).
     */
    AE_FIELD_THEME          = 10100, /* AEW_THEME */
    /*
     * ALSO THIS BOARD'S OWN: what KIND of screen the caller has.
     *
     * The SIZE has been askable for ever (AE_FIELD_SCREEN_COLS), and for a
     * long time that was the whole question, because every caller's screen
     * was the same kind of screen in two widths. It is not any more: a
     * PETSCII caller's bytes are transduced on the way out and a teletext
     * caller's terminal draws DECDHL headlines in a teletext face. Forty
     * columns is true of both, so a door that writes a teletext escape on
     * the strength of the width writes it into a C64's byte stream too.
     *
     * Answers `ansi`, `petscii`, `teletext` or `rip`. Read-only. A classic
     * board answers nothing, which ae_display_mode reports as `ansi` - the
     * mode every caller on a classic board has.
     */
    AE_FIELD_DISPLAY_MODE   = 10400, /* AEW_DISPLAYMODE */
    /*
     * ALSO THIS BOARD'S OWN: move the caller's read pointer.
     *
     * AE_FIELD_LAST_READ / _LAST_SCANNED above are READ-ONLY, and that is
     * true of a real AmiExpress too, not just of a board that has not got
     * round to it - express.e:3963-3966 only ever answers, while MSGBASE_LOC
     * three lines below shows the settable idiom the author used where a
     * field WAS meant to be written. The board owns the pointer and writes it
     * when the BOARD does the reading.
     *
     * A door that reads the message base itself therefore cannot mark
     * anything read, and SUBS is exactly that door. This is the additive
     * command that closes it. Written as "READ <n>" or "SCAN <n>".
     *
     * A board that does not have it ignores the write, the read-back does not
     * move, and ae_set_last_read answers 0 - so the door keeps working with
     * nothing marked, which is what it does on a classic board today.
     *
     * Proposed upstream: Documentation/4-Door-Developers/PROPOSED_SETPOINTER.md
     */
    AE_FIELD_SETPOINTER     = 10300, /* AEW_SETPOINTER */
    /*
     * ALSO THIS BOARD'S OWN: post a message through the board's own save, so
     * it gets everything saveNewMSG does - access, post count, callers log,
     * notifications, the board's copy of it. See ae_post_open. A board that
     * does not know it answers nothing, and the door writes the base itself.
     */
    AE_FIELD_POSTMSG        = 10500, /* AEW_POSTMSG */

    /*
     * ALSO THIS BOARD'S OWN: the W door's field conversation (AEW_USERFIELD).
     * A request in String, the answer in String - ae_field_exchange. See
     * ae_userfield.h for the verbs. A board that does not know it answers
     * nothing and the door leaves.
     */
    AE_FIELD_USERFIELD      = 10600, /* AEW_USERFIELD */
    /* 10700 is RESERVED for the J (join conference) door's AEW_CONFLIST; do not take it. */

    /*
     * ALSO THIS BOARD'S OWN: any C door's screen conversation (AEW_SCREEN) -
     * the caller's real size, board wipes around a screen, and mouse events.
     * A request in String, the answer in String. See ae_board_screen.h.
     */
    AE_FIELD_SCREEN         = 10800, /* AEW_SCREEN */

    /* The menu doors' catalogue and actions (AEW_MENU); the client is a later task. */
    AE_FIELD_MENU           = 10900, /* AEW_MENU */

    /* May this door animate? '1' yes, '0' the caller turned animations off
       (W row 23); a game door always reads '1'. Read-only. */
    AE_FIELD_ANIMATIONS     = 11000, /* AEW_ANIMATIONS */

    /* The caller's wire charset ("utf-8", "iso-8859-1", "cp437"). Writing
       "utf-8" declares this door's text bytes UTF-8 for the rest of the run;
       a door that never writes it is byte-identical to before. */
    AE_FIELD_CHARSET        = 11100, /* AEW_CHARSET */

    /* A sysop note about a user (AEW_USERNOTE); see ae_usernote.h. */
    AE_FIELD_USERNOTE       = 11200, /* AEW_USERNOTE */

    /* The caller's LiveChat prefs (AEW_CHATPREFS); see ae_chatprefs.h. */
    AE_FIELD_CHATPREFS      = 11300  /* AEW_CHATPREFS */
} ae_field;

/**
 * The message functions a conference's CUSTOM message base is called with -
 * `axenums.e:27`, read from AE_FIELD_MSGBASE_FUNC.
 *
 * There is no 5. The gap is AmiExpress's own, and closing it here would put
 * every door built against this SDK half a step out from a real board.
 */
enum {
    AE_MAIL_NONE          = 0,   /* not a message-base launch at all */
    AE_MAIL_READ          = 1,
    AE_MAIL_CREATE        = 2,
    AE_MAIL_SCAN          = 3,
    AE_MAIL_STATS         = 4,
    AE_MAIL_SYSOP_COMMENT = 6,
    AE_MAIL_ZOOM          = 7
};

/**
 * One round trip to the board.
 *
 * Returns the number of characters written to `out` (0 for a field with no
 * text), or -1 when the CARRIER IS GONE - which is the one failure a door
 * must notice, because everything after it is writing into a dropped line.
 */
typedef int (*ae_transport_fn)(void *context, ae_field field, char *out, int cap);

/**
 * Write one field back to the board.
 *
 * Optional: a transport that only reads leaves it NULL, and the one call
 * that needs it (ae_set_user_theme) answers "this host cannot" rather than
 * failing. Returns 0 when the board took it, -1 when the carrier is gone.
 */
typedef int (*ae_write_fn)(void *context, ae_field field, const char *value);

/**
 * Ask the board one ACS question (DT_QUERYBIT). Returns 1 or 0, or -1 when
 * the carrier is gone. Optional, like the writer.
 */
typedef int (*ae_query_bit_fn)(void *context, int acs);

/**
 * A session.
 *
 * The fields are visible so a caller can place one on its own stack; treat
 * them as private. `storage`/`cap` are the buffer the transport works in.
 */
typedef struct {
    ae_transport_fn transport;
    /** May be NULL: a read-only transport is a valid one. */
    ae_write_fn write;
    /** May be NULL: then ae_check_security answers "would not say". */
    ae_query_bit_fn query_bit;
    void *context;
    char *storage;
    long cap;
    int node;
    /** 0 once any round trip has reported the carrier gone. */
    int carrier;
} ae_session;

/**
 * Start a session on `node`, with a buffer the caller owns.
 *
 * The buffer must be at least AE_SESSION_MIN_STORAGE bytes - the AEDoor
 * message is 264 and a door that hands over less would corrupt the board's
 * reply. Returns 0 on success, -1 when the arguments cannot work, and does
 * not allocate.
 */
int ae_open(ae_session *s, ae_transport_fn transport, void *context,
            char *storage, long cap, int node);

/**
 * Give a session a write path. Optional, and separate from ae_open so the
 * dozens of read-only callers do not grow an argument they would pass NULL
 * to.
 */
void ae_set_writer(ae_session *s, ae_write_fn write);

/** Give a session a way to ask ACS questions. Optional, as ae_set_writer. */
void ae_set_security_query(ae_session *s, ae_query_bit_fn query_bit);

/** The AEDoor message size, which is the floor for `cap`. */
#define AE_SESSION_MIN_STORAGE 264

/**
 * Open a session on the REAL board, using the AEDoor protocol.
 *
 * The convenience every door wants: ae_open with the transport already
 * wired, reads and writes both. Declared here, implemented in its own
 * module, so a door that never opens a session does not link the protocol.
 *
 * `node` is what AmiExpress passed as argv[1].
 */
int ae_open_bbs(ae_session *s, char *storage, long cap, int node);

/** End a session. Safe on one that never opened. */
void ae_close(ae_session *s);

/** Is the line still up? 0 once a round trip has said otherwise. */
int ae_carrier(const ae_session *s);

/** The node this door was started on (argv[1] on a real board). */
int ae_node(const ae_session *s);

/*
 * User identity. Each is one round trip; the caller owns `out`, and every
 * one of them leaves `out` a valid empty string when the answer is unknown,
 * so a door can print the result without checking first.
 */
int ae_user_name(ae_session *s, char *out, int n);
int ae_user_location(ae_session *s, char *out, int n);

/** -1 when the board did not answer, which is not a level a door should use. */
int ae_user_level(ae_session *s);
int ae_user_time_left(ae_session *s);

/** 1 when the caller's terminal takes ANSI, 0 when it does not. */
int ae_user_is_ansi(ae_session *s);

/*
 * Geometry. A door that never asks gets 80x25, the board's own default and
 * the size every classic door assumed.
 */
int ae_screen_cols(ae_session *s);
int ae_screen_rows(ae_session *s);
int ae_conference(ae_session *s);

/**
 * The current conference's directory, e.g. "BBS:Conf3/". "" when the board
 * does not answer.
 *
 * A door needs this to read the conference's own files - MsgBases.info above
 * all, which is the only place the names and locations of several message
 * bases in one conference are written down.
 */
int ae_conf_location(ae_session *s, char *out, int n);

/**
 * The current conference's name, e.g. "Coding". "" when the board does not
 * answer.
 *
 * It is NOT in the message base - a message base is headers and bodies and
 * knows nothing about what the conference is called - so a door that wants it
 * on a masthead asks the board.
 */
int ae_conf_name(ae_session *s, char *out, int n);

/**
 * The current conference's DEFAULT message base directory, e.g.
 * "BBS:Conf3/MsgBase/". "" when the board does not answer.
 *
 * HeaderFile, MailStats, MailLock and the numbered body files all live here.
 * On a conference with several bases this is base 1 and not necessarily the
 * one the caller has joined - resolve LOCATION.n out of MsgBases.info for
 * that, starting from ae_conf_location().
 */
int ae_msgbase_path(ae_session *s, char *out, int n);

/**
 * The highest message number this caller has READ in this conference
 * (express.e's lastMsgReadConf), or -1 when the board does not answer.
 *
 * -1 and not 0, because 0 is a real answer: it is what a caller who has never
 * read anything here has, and a door that could not ask must not silently
 * treat every message as new.
 */
int ae_last_read(ae_session *s);

/**
 * The highest message number this caller has been SCANNED up to
 * (lastNewReadConf), or -1 when the board does not answer. This is the one
 * that decides what counts as new; ae_last_read is where he stopped reading.
 */
int ae_last_scanned(ae_session *s);

/**
 * The MAIL_* function this door was launched for, or 0 when it was not
 * launched as a conference's message base (axenums.e:27):
 *
 *     1 READ   2 CREATE   3 SCAN   4 STATS   6 SYSOP_COMMENT   7 ZOOM
 *
 * There is no 5. The gap is AmiExpress's.
 */
int ae_msgbase_function(ae_session *s);

/**
 * The conference that function is about, or 0. NOT always the conference the
 * caller is in: `MS` walks every conference he may read and calls the door
 * once per conference without him leaving his own (express.e:25260-25268).
 */
int ae_msgbase_conference(ae_session *s);

/**
 * The board's clock in express.e's getSystemTime() form (see
 * AE_FIELD_CURR_TIME) - a message date, not Unix time - or 0 where it will
 * not say.
 *
 * 0 rather than a guess: a message stamped with a wrong date is worse than one
 * the door refuses to write, and the caller can decide which.
 */
long ae_now(ae_session *s);

/**
 * Tell the board the caller has read up to `msg_num`.
 *
 * For a door that reads the message base itself: without this nothing ever
 * advances the pointer, so every message stays new for ever and the caller
 * can never clear "N new for you".
 *
 * Returns 1 when the pointer is now at or past `msg_num`, 0 otherwise -
 * including on a board that does not implement the command, which is not an
 * error and must not stop the door. Requires a writer (ae_set_writer).
 *
 * Pointers move FORWARD ONLY, as express.e's own readit does
 * (`IF(msgNum>lastMsgReadConf)`, express.e:12370). Asking for a lower number
 * answers 1 without moving anything: the state asked for is already true.
 */
int ae_set_last_read(ae_session *s, int msg_num);

/**
 * As ae_set_last_read, for the new-scan pointer (express.e lastNewReadConf).
 *
 * That pointer is the NEXT number to scan, not the last one scanned: the
 * board's scan starts AT it and leaves it at msgNum+1 (express.e:5103 prints
 * it minus one). A door that read up to m passes m+1.
 */
int ae_set_last_scanned(ae_session *s, int msg_num);

/**
 * The caller opened message `msg_num`: the board marks it received when it is
 * addressed to him, by its own rule (readit, express.e:8943-8949) - the
 * sender's R then shows when it was read, and the board's scan stops offering
 * it. AEW_SETPOINTER "RECV <n>". Returns 1 when the request went out; a board
 * without the command ignores it, as it ignores the pointer verbs.
 */
int ae_set_received(ae_session *s, int msg_num);

/* ACS indices a door asks about, from axcommon.e:12-16's enum. */
#define AE_ACS_ENTER_MESSAGE   5
#define AE_ACS_SYSOP_READ     23
#define AE_ACS_EALL_MESSAGES  34
#define AE_ACS_READ_PRIV_EALL 81
#define AE_ACS_READ_PRIV_ALL  82

/**
 * Does the caller pass the board's ACS check `acs` (AE_ACS_*)? DT_QUERYBIT,
 * which a real AmiExpress answers too. Returns 1 or 0, or -1 when the board
 * would not say - which a door must treat as "no" for anything that shows
 * somebody else's mail.
 */
int ae_check_security(ae_session *s, int acs);

/* ae_post_open / ae_post_send answers. */
#define AE_POST_READY      1   /* the board will take this post */
#define AE_POST_REFUSED    0   /* the board said no; `why` says why */
#define AE_POST_NO_BOARD  -1   /* the board does not post for doors: write it yourself */
#define AE_POST_GONE      -2   /* the carrier dropped */

/**
 * Start a post through the board (AEW_POSTMSG): may this caller post to `to`
 * (EALL or ALL) here? `status` is 'P' or 'R'. Asked BEFORE the caller types,
 * as express.e asks at the To: prompt. The board applies its own access
 * (ACS_ENTER_MESSAGE, ACS_EALL_MESSAGES); a refusal's sentence lands in
 * `why`.
 */
int ae_post_open(ae_session *s, char status, const char *to, char *why, int why_cap);

/**
 * Send the post ae_post_open started. Returns the message number (>= 1), or
 * AE_POST_REFUSED with `why` filled (a base another node holds locked says
 * so), AE_POST_NO_BOARD, or AE_POST_GONE. The body is the caller's bytes,
 * LF between lines; the board stores it in its own encoding.
 */
long ae_post_send(ae_session *s, const char *subject, const char *body,
                  unsigned long body_len, char *why, int why_cap);

/**
 * The caller's theme id ("classic", "uprough-neon", ...), or "" where the
 * board does not answer - a classic AmiExpress, which has no themes.
 */
int ae_user_theme(ae_session *s, char *out, int n);

/**
 * The KIND of screen the caller has: `ansi`, `petscii`, `teletext`, `rip`.
 *
 * Ask before writing anything only one kind of terminal understands - a
 * teletext door's double-height headlines, say. A board that does not know
 * the question answers nothing and this reports `ansi`, so a door written
 * against it still runs on a classic AmiExpress.
 *
 * Returns 0 and fills `out` on success, -1 when the board said nothing.
 */
int ae_display_mode(ae_session *s, char *out, int n);

/**
 * Whether the caller wants animations (W row 23). A game door always gets 1.
 *
 * ae_open asks once and hands the answer to ui_effects_allowed, so a door
 * that gates its effects there needs no call of its own. A board that does
 * not know the question answers nothing and this reports 1: animations are
 * what a classic board always had.
 */
int ae_animations_allowed(ae_session *s);

/**
 * The caller's wire charset: "utf-8", "iso-8859-1" or "cp437". A classic
 * board that does not know the question answers nothing and this fills
 * "iso-8859-1", the Amiga's own. Returns 0.
 */
int ae_wire_charset(ae_session *s, char *out, int n);

/**
 * Declares this door's text output charset. Only "utf-8" changes anything:
 * the board then decodes the door's bytes as UTF-8 (a stray latin1 byte
 * still passes as latin1). Call it only after ae_wire_charset said
 * "utf-8". Returns 1 when sent, 0 when there is no board to tell.
 */
int ae_set_output_charset(ae_session *s, const char *charset);

/**
 * The command name the caller typed to get here, or "" where the board does
 * not say.
 *
 * One binary can then be registered twice and behave differently under each
 * name - which is the only mechanism available, since AmigaDOS hands a door a
 * single argument string and argv[1] is already the node number.
 */
int ae_menu_command(ae_session *s, char *out, int n);

/**
 * The full command line that launched the door, e.g. "PALETTE mail", or ""
 * where the board does not say. Returns its length.
 */
int ae_main_line(ae_session *s, char *out, int n);

/**
 * Change the caller's theme, and have the board remember it.
 *
 * Returns 1 when the board took it, 0 when this host has no such command -
 * which is not an error, it is a classic board. A door should say so rather
 * than pretending the choice was saved.
 *
 * Requires a write, so it is the one call here that needs the transport to
 * support direction; see ae_transport_fn.
 */
int ae_set_user_theme(ae_session *s, const char *theme_id);

/** What a door falls back to. */
#define AE_DEFAULT_COLS 80
#define AE_DEFAULT_ROWS 25

#ifdef __cplusplus
}
#endif

#endif /* AE_SESSION_H */
