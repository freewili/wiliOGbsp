/* Application-level link messages for breakout I/O direction control.
 *
 * Types start at 0x20 because the hardware record freezes 0x01-0x1F for the
 * bootloader's own protocol and 0x00 as invalid. A display bootloader
 * already shipped in the field speaks that framing, so the range cannot be
 * widened later.
 *
 * Every struct below carries a _Static_assert on its size because both CPUs
 * put them on the wire unserialized (the hardware record). Byte 0 of every payload
 * is the message type (the hardware record).
 *
 * The reference sends the direction word TWICE and rejects a mismatch
 * (rpProcessHostCommand.cpp:52-59). That is deliberately not carried over:
 * link_frame.c already CRCs every frame, on a channel measured at zero
 * corrupt bytes across 58 consecutive 256-value sweeps (the hardware record), so a
 * duplicate field would be redundant integrity checking.
 *
 * Structs go on the wire as-is, like bl_proto.h's. Both ends are
 * little-endian Cortex-M0+, and every multi-byte member below is either
 * naturally aligned or -- unlike bl_proto.h -- inside a struct marked
 * __attribute__((packed)), so there is no serialization step and no
 * endianness conversion. The _Static_asserts are what make that safe rather
 * than merely true today.
 *
 * bl_proto.h lays down a rule this file's callers do NOT follow: "callers
 * must memcpy into a local struct rather than casting a pointer into a
 * receive buffer, because fwog_link_rx_t.buf has no alignment guarantee."
 * gpio/breakout.c and display_cpu/io_expander/ioexp_link.c both cast
 * directly into the receive buffer instead. That is not an oversight of the
 * same rule -- it is safe for a reason bl_proto.h's structs don't have:
 * `packed` tells the compiler these members may be unaligned, so it emits
 * byte-wise loads/stores instead of a single unaligned word access. The
 * bl_proto.h rule exists because ITS structs are plain (natural alignment
 * assumed); this file's structs opt out of that assumption entirely, which
 * is what makes the cast legal here and nowhere else in this repo's link
 * code.
 *
 * Every request (CONFIG_ENABLE, SET_DIRS) and its ACK carry a one-byte
 * `seq`. The display echoes back the seq of the message it is acking; main
 * increments a counter per message sent and rejects an ack whose seq does
 * not match the one just sent. Without this, a stale ack from a timed-out
 * attempt can satisfy the wait for the NEXT attempt -- see breakout.c's
 * wait_ack() and the design note in gpio/breakout.h. */
#ifndef FWOG_IO_PROTO_H
#define FWOG_IO_PROTO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "common/io_cfg.h"

#define FWOG_IO_MSG_CONFIG_ENABLE 0x20u   /* main -> display */
#define FWOG_IO_MSG_SET_DIRS      0x21u   /* main -> display */
#define FWOG_IO_MSG_ACK           0x22u   /* display -> main */
/* 0x40-0x47 reserved for apps/iotest/proto/iotest_proto.h's own MAIN<->
   DISPLAY messages (free-form UI content, not direction state -- out of
   scope for this file). Do not reuse that range; pick above 0x47 if this
   file ever grows past 0x22. */

typedef struct __attribute__((packed)) {
    uint8_t type;      /* FWOG_IO_MSG_CONFIG_ENABLE */
    uint8_t seq;       /* echoed back in the ACK; see the file header */
    uint8_t enable;
} fwog_io_config_enable_t;
_Static_assert(sizeof(fwog_io_config_enable_t) == 3,
               "fwog_io_config_enable_t goes on the wire unserialized");

typedef struct __attribute__((packed)) {
    uint8_t  type;         /* FWOG_IO_MSG_SET_DIRS */
    uint8_t  seq;          /* echoed back in the ACK; see the file header */
    uint16_t dirword;      /* fwog_io_pack_dirword() */
    uint8_t  i2c_pullup;
    uint8_t  radio1_ant;   /* fwog_ant_t */
    uint8_t  radio2_ant;   /* fwog_ant_t */
} fwog_io_set_dirs_t;
_Static_assert(sizeof(fwog_io_set_dirs_t) == 7,
               "fwog_io_set_dirs_t goes on the wire unserialized");

typedef struct __attribute__((packed)) {
    uint8_t type;        /* FWOG_IO_MSG_ACK */
    uint8_t answering;   /* the type this answers */
    uint8_t seq;         /* echo of the request's seq; see the file header */
    uint8_t ok;
} fwog_io_ack_t;
_Static_assert(sizeof(fwog_io_ack_t) == 4,
               "fwog_io_ack_t goes on the wire unserialized");

/* Each returns the bytes written, or 0 if `cap` is too small (writing
 * nothing). `seq` is the sender's per-message counter for build_config_enable
 * and build_set_dirs; build_ack's `seq` must be the seq of the request being
 * answered, so the caller echoes it straight from the decoded message. */
size_t fwog_io_proto_build_config_enable(void *out, size_t cap, uint8_t seq,
                                         bool enable);
size_t fwog_io_proto_build_set_dirs(void *out, size_t cap, uint8_t seq,
                                    const fwog_io_cfg_t *cfg);
size_t fwog_io_proto_build_ack(void *out, size_t cap, uint8_t seq,
                               uint8_t answering, bool ok);

/* The message type if `payload` is one of ours and long enough for its own
 * struct, else 0. 0 is safe to return because the hardware record reserves it as
 * invalid. */
uint8_t fwog_io_proto_type(const void *payload, size_t len);

#endif
