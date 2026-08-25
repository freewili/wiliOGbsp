/* The guided-walk screen: one persistent screen, re-labelled per step, plus
 * a summary screen shown at the end. Button wiring (Confirm/Retry/Skip on
 * the step screen, Restart on the summary screen) lives here too, since
 * sending a CONFIRM reply is what a button press IS in this app -- there is
 * no other place that would make sense to own it. */
#ifndef FWOG_IOTEST_UI_H
#define FWOG_IOTEST_UI_H
#include "proto/iotest_proto.h"

/* Builds both screens and the button groups. Call once, after
 * fwog_lvgl_init(). */
void iotest_ui_build(void);

/* Update the step screen from a decoded STEP_SHOW and switch to it if the
 * summary screen was showing. Does NOT send the STEP_ACK -- the caller
 * (display/main.c) does that once this returns, so the ack means "rendered"
 * rather than merely "received". */
void iotest_ui_show_step(const fwog_iotest_step_show_t *m);

/* Update the summary screen from a decoded SUMMARY and switch to it. */
void iotest_ui_show_summary(const fwog_iotest_summary_t *m);

#endif
