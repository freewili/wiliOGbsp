/* Link messages for apps/iotest's own MAIN<->DISPLAY coordination.
 *
 * bsp/common/link/io_proto.h's 0x20-0x22 carries direction/pin-config
 * round-trips and is reused as-is (via bsp/main_cpu/gpio/breakout.h) for
 * actually driving pins. What's missing for a guided on-screen walk is a way
 * for MAIN to tell DISPLAY "show this instruction" and for DISPLAY to tell
 * MAIN "the operator pressed Confirm/Retry/Skip" -- free-form app content,
 * not direction state, so it doesn't belong in the shared BSP protocol.
 *
 * Types reserved at 0x40-0x47 (see the reservation note in
 * bsp/common/link/io_proto.h) -- comfortably clear of bl_proto.h's
 * 0x01-0x1F and io_proto.h's 0x20-0x22. Same wire conventions as
 * io_proto.h: packed structs, byte 0 is the type, every struct carries a
 * _Static_assert on its size because both CPUs put these on the wire
 * unserialized. */
#ifndef FWOG_IOTEST_PROTO_H
#define FWOG_IOTEST_PROTO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FWOG_IOTEST_MSG_STEP_SHOW  0x40u   /* MAIN -> DISPLAY: show this step */
#define FWOG_IOTEST_MSG_STEP_ACK   0x41u   /* DISPLAY -> MAIN: rendered */
#define FWOG_IOTEST_MSG_CONFIRM    0x42u   /* DISPLAY -> MAIN: operator action */
#define FWOG_IOTEST_MSG_SUMMARY    0x43u   /* MAIN -> DISPLAY: final report */

/* Longest generated instruction string is well under this; see
 * iotest_ui_build_step_text()'s _Static_assert-adjacent length check. */
#define FWOG_IOTEST_TEXT_LEN 48u

typedef enum {
    FWOG_IOTEST_ACTION_CONFIRM = 0,
    FWOG_IOTEST_ACTION_RETRY,
    FWOG_IOTEST_ACTION_SKIP,
    FWOG_IOTEST_ACTION_RESTART   /* only meaningful from the summary screen */
} fwog_iotest_action_t;

typedef struct __attribute__((packed)) {
    uint8_t type;                          /* FWOG_IOTEST_MSG_STEP_SHOW */
    uint8_t seq;
    uint8_t step_index;                    /* 0..step_count-1 */
    uint8_t step_count;
    uint8_t drive_hdr;                     /* 0 = none (e.g. control failure) */
    uint8_t sense_hdr;                     /* 0 = none (e.g. I2C step) */
    uint8_t last_result;                   /* fwog_iotest_result_t, of the
                                               step just completed, or
                                               PENDING if this is step 0 */
    char    text[FWOG_IOTEST_TEXT_LEN];    /* NUL-padded instruction */
} fwog_iotest_step_show_t;
_Static_assert(sizeof(fwog_iotest_step_show_t) == 7u + FWOG_IOTEST_TEXT_LEN,
               "fwog_iotest_step_show_t goes on the wire unserialized");

typedef struct __attribute__((packed)) {
    uint8_t type;   /* FWOG_IOTEST_MSG_STEP_ACK */
    uint8_t seq;    /* echo of the STEP_SHOW being answered */
} fwog_iotest_step_ack_t;
_Static_assert(sizeof(fwog_iotest_step_ack_t) == 2,
               "fwog_iotest_step_ack_t goes on the wire unserialized");

typedef struct __attribute__((packed)) {
    uint8_t type;    /* FWOG_IOTEST_MSG_CONFIRM */
    uint8_t seq;     /* echo of the STEP_SHOW being answered */
    uint8_t action;  /* fwog_iotest_action_t */
} fwog_iotest_confirm_t;
_Static_assert(sizeof(fwog_iotest_confirm_t) == 3,
               "fwog_iotest_confirm_t goes on the wire unserialized");

#define FWOG_IOTEST_MAX_STEPS 16u

typedef struct __attribute__((packed)) {
    uint8_t type;                            /* FWOG_IOTEST_MSG_SUMMARY */
    uint8_t seq;
    uint8_t step_count;
    uint8_t overall_pass;
    uint8_t results[FWOG_IOTEST_MAX_STEPS];  /* fwog_iotest_result_t, index-
                                                 aligned; entries at and past
                                                 step_count are unused */
} fwog_iotest_summary_t;
_Static_assert(sizeof(fwog_iotest_summary_t) == 4u + FWOG_IOTEST_MAX_STEPS,
               "fwog_iotest_summary_t goes on the wire unserialized");

/* Each returns bytes written, or 0 if `cap` is too small (writing nothing). */
size_t fwog_iotest_proto_build_step_show(void *out, size_t cap, uint8_t seq,
                                         uint8_t step_index, uint8_t step_count,
                                         uint8_t drive_hdr, uint8_t sense_hdr,
                                         uint8_t last_result, const char *text);
size_t fwog_iotest_proto_build_step_ack(void *out, size_t cap, uint8_t seq);
size_t fwog_iotest_proto_build_confirm(void *out, size_t cap, uint8_t seq,
                                       fwog_iotest_action_t action);
size_t fwog_iotest_proto_build_summary(void *out, size_t cap, uint8_t seq,
                                       const uint8_t *results,
                                       uint8_t step_count, bool overall_pass);

/* The message type if `payload` is one of ours and long enough for its own
 * struct, else 0 (0x00 is invalid on this link -- see link_frame.h). */
uint8_t fwog_iotest_proto_type(const void *payload, size_t len);

#endif
