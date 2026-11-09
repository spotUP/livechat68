/*
 * ae_field_shape - which board fields answer in the message's Data and
 * which in its String.
 *
 * The platform-free half of ae_transport_amiga's field read, split out the
 * same way ae_dos.c is split from ae_dos_amiga.c: the transport needs the NDK
 * and never builds on the host, so the list it decides by lived where no test
 * could reach it. It was wrong there for DT_ISANSI (see ae_field_shape.c).
 *
 * C89. ASCII only.
 */
#ifndef AE_FIELD_SHAPE_H
#define AE_FIELD_SHAPE_H

/** 1 when express.e answers `field` in msg.data and writes nothing into String. */
int ae_field_answers_in_data(int field);

#endif
