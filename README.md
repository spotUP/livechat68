# LiveChat68

A live chat door for AmiExpress, written in C for the 68000. It joins a board
to the shared Up Rough chat hub (doors.uprough.net): rooms, private messages,
group messages, themes and help. Callers on every board that runs it talk to
each other and to callers on the web board at bbs.uprough.net. The board makes
outgoing HTTPS connections only.

This is the source. The ready-to-run door is the `LIVECH68.LHA` archive on the
releases page; its `README.txt` covers install and setup on AmiExpress.

## Build

    make            # 68K door: build/amiga/livechat (vbcc, vasm, NDK 3.2, AmiSSL SDK)
    make test       # host tests (any C99 compiler, OpenSSL 3)

Tool paths are variables at the top of the `Makefile`.

## Porting to another BBS

See `PORTING.txt`. The chat client and the screen code are board-independent;
one small layer talks to the BBS.

## License

MIT, see `LICENSE`.
