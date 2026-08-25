#include "iotest_proto.h"
#include <string.h>

size_t fwog_iotest_proto_build_step_show(void *out, size_t cap, uint8_t seq,
                                         uint8_t step_index, uint8_t step_count,
                                         uint8_t drive_hdr, uint8_t sense_hdr,
                                         uint8_t last_result, const char *text) {
    if (cap < sizeof(fwog_iotest_step_show_t)) return 0u;
    fwog_iotest_step_show_t m;
    m.type        = FWOG_IOTEST_MSG_STEP_SHOW;
    m.seq         = seq;
    m.step_index  = step_index;
    m.step_count  = step_count;
    m.drive_hdr   = drive_hdr;
    m.sense_hdr   = sense_hdr;
    m.last_result = last_result;
    memset(m.text, 0, sizeof m.text);
    if (text) strncpy(m.text, text, sizeof(m.text) - 1u);
    memcpy(out, &m, sizeof m);
    return sizeof m;
}

size_t fwog_iotest_proto_build_step_ack(void *out, size_t cap, uint8_t seq) {
    if (cap < sizeof(fwog_iotest_step_ack_t)) return 0u;
    fwog_iotest_step_ack_t m;
    m.type = FWOG_IOTEST_MSG_STEP_ACK;
    m.seq  = seq;
    memcpy(out, &m, sizeof m);
    return sizeof m;
}

size_t fwog_iotest_proto_build_confirm(void *out, size_t cap, uint8_t seq,
                                       fwog_iotest_action_t action) {
    if (cap < sizeof(fwog_iotest_confirm_t)) return 0u;
    fwog_iotest_confirm_t m;
    m.type   = FWOG_IOTEST_MSG_CONFIRM;
    m.seq    = seq;
    m.action = (uint8_t)action;
    memcpy(out, &m, sizeof m);
    return sizeof m;
}

size_t fwog_iotest_proto_build_summary(void *out, size_t cap, uint8_t seq,
                                       const uint8_t *results,
                                       uint8_t step_count, bool overall_pass) {
    if (cap < sizeof(fwog_iotest_summary_t)) return 0u;
    fwog_iotest_summary_t m;
    m.type         = FWOG_IOTEST_MSG_SUMMARY;
    m.seq          = seq;
    m.step_count   = step_count;
    m.overall_pass = overall_pass ? 1u : 0u;
    memset(m.results, 0, sizeof m.results);
    const uint8_t n = step_count < FWOG_IOTEST_MAX_STEPS
                       ? step_count : FWOG_IOTEST_MAX_STEPS;
    if (results) memcpy(m.results, results, n);
    memcpy(out, &m, sizeof m);
    return sizeof m;
}

uint8_t fwog_iotest_proto_type(const void *payload, size_t len) {
    if (len == 0u) return 0u;
    uint8_t t = ((const uint8_t *)payload)[0];
    switch (t) {
    case FWOG_IOTEST_MSG_STEP_SHOW:
        return len >= sizeof(fwog_iotest_step_show_t) ? t : 0u;
    case FWOG_IOTEST_MSG_STEP_ACK:
        return len >= sizeof(fwog_iotest_step_ack_t) ? t : 0u;
    case FWOG_IOTEST_MSG_CONFIRM:
        return len >= sizeof(fwog_iotest_confirm_t) ? t : 0u;
    case FWOG_IOTEST_MSG_SUMMARY:
        return len >= sizeof(fwog_iotest_summary_t) ? t : 0u;
    default:
        return 0u;
    }
}
