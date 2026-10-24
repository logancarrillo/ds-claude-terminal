// The PHOTO screen: the DSi's sensor as QUICK's PHOTO row.
//
// A live viewfinder on the bottom screen, a reduced preview streamed to the
// computer, and a still that crosses in chunks only after A.  camera.h owns
// every byte-level detail and the screen's states; what lives here is the
// hardware choreography (NDMA channel 1 into a ping-pong pair, a CAPTURE
// transfer for the still) and the pacing of what goes on the wire.
//
// Moved out of main.c whole.  It reaches main.c only through host.h, and the
// wire only through host_send_camera(), which takes nothing but the camera's
// own message types: this file can show pictures and send them, never type.
// main.c decides where the screen goes when the camera closes, and owns the
// composer a saved picture's caption opens in.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nds.h>

#include "camera.h"
#include "host.h"
#include "kbd.h"
#include "log.h"
#include "photo.h"
#include "proto.h"
#include "seal.h"
#include "settings.h"
#include "subscreen.h"

// Whether this card has a camera, from isDSiMode() at boot: QUICK's PHOTO
// row, the SETTINGS shutter row and the camera HELLO feature all key off it.
static int camera_available = 0;

// The buffers are TWL so the 4 MB the terminal's history fills never sees
// them; aligned because NDMA asks for it.  Touched only on a DSi.
#define CAMERA_NDMA 1
TWL_BSS static uint16_t cam_pv[2][CAMERA_SRC_W * CAMERA_SRC_H]
    __attribute__((aligned(32)));
TWL_BSS static uint8_t cam_yuyv[CAMERA_PHOTO_BYTES]
    __attribute__((aligned(32)));
TWL_BSS static uint8_t cam_packed[CAMERA_PHOTO_BYTES_MAX]
    __attribute__((aligned(32)));
TWL_BSS static uint16_t cam_still[CAMERA_SRC_W * CAMERA_SRC_H]
    __attribute__((aligned(32)));

static camera_ui_t cam_ui = CAMERA_UI_OFF;
static int camera_proved = 0;     // a CAMERA_READY landed on this link
static int camera_can_save = 0;   // its bit0: the computer can keep pictures
static int cam_which = PROTO_CAMERA_OUTER;    // remembered for this boot
static int cam_hw_up = 0;         // cameraInit() has run and Deinit has not
static int cam_front = 0;         // the cam_pv half the strips may draw
enum { CAM_T_NONE = 0, CAM_T_PREVIEW, CAM_T_CAPTURE };
static int cam_transfer = CAM_T_NONE;
static unsigned long cam_transfer_at = 0;
static int cam_transfer_retry = 0;
static int cam_frame_new = 0;     // a fresh frame awaits its blit
static int cam_still_new = 0;     // the review thumbnail awaits its blit
static int cam_capture_pending = 0;  // the shutter fired; drain, then capture
static int cam_still_owed = 0;    // REVIEW owes the wire one STILL preview
static camera_pacer_t cam_pacer;
static camera_upload_t cam_upload;
static camera_shutter_state_t cam_shutter;
static int cam_fmt = PROTO_PHOTO_FORMAT_FULL;
static uint32_t cam_crc = 0;
static unsigned long cam_sent_at = 0;
// When the upload last moved: a computer that stops taking chunks fails the
// send after SENDING_STALL_FRAMES rather than leaving "sending N%" up.
static unsigned long cam_progress_at = 0;
#define SENDING_STALL_FRAMES (20 * 60)
static unsigned long cam_saving_at = 0;
static unsigned cam_percent_shown = 0;
static char cam_fail[LOG_COLS + 1];
static int cam_touch_frames = 0;
static int cam_bench_done = 0;
static void cam_strips(void);

// The LED is the ARM7's I2C; the value crosses on a user FIFO channel and
// the handler there writes I2CREGPM_CAMLED.  DSi only.
static void camled(int on)
{
    if (isDSiMode()) fifoSendValue32(FIFO_USER_01, (uint32_t)(on ? 1 : 0));
}

// The overlay owns input whole, the way the composer does, and leaves the
// top screen to the terminal.  Its states are camera.h's; the choreography
// below is NDMA channel 1 into the ping-pong pair, a CAPTURE transfer for
// the still, and one paced preview frame on the wire at a time.

// One framed camera message, counted like a keystroke; a failed send is the
// same dead-link evidence send_key_seq treats it as.
static int cam_send(const uint8_t *out, size_t n, unsigned long *tx)
{
    return host_send_camera(out, n, tx);
}

static int cam_send_state(unsigned long *tx)
{
    uint8_t out[PROTO_HEADER_SIZE + 1];
    return cam_send(out, proto_encode_camera(out, cam_which), tx);
}

// The strips: camera.h composes the words, the failure's reason replaces the
// hint, and a REVIEW on a computer that cannot save says so instead of
// offering an A that would fail anyway.
static void cam_strips(void)
{
    char top[LOG_COLS + 1], hint[LOG_COLS + 1];
    camera_ui_top(cam_ui, cam_which, top, sizeof(top));
    if (cam_ui == CAMERA_UI_FAILED && cam_fail[0]) {
        snprintf(hint, sizeof(hint), "%s", cam_fail);
    } else if (cam_ui == CAMERA_UI_REVIEW && !camera_can_save) {
        snprintf(hint, sizeof(hint), "this computer cannot save pictures");
    } else {
        camera_ui_hint(cam_ui,
                       camera_shutter_name(host_settings()->camera_shutter),
                       camera_upload_percent(&cam_upload),
                       hint, sizeof(hint));
    }
    subscreen_set_camera(cam_ui != CAMERA_UI_OFF, top, hint,
                         camera_ui_is_error(cam_ui));
    host_refresh_sub();
}

static void camera_fail(const char *why)
{
    host_status("camera: %s", why);
    snprintf(cam_fail, sizeof(cam_fail), "%s", why);
    cam_ui = camera_ui_failed(cam_ui);
    cam_strips();
}

// Stop a transfer on both sides.  cameraStopTransfer() only clears CAM_CNT's
// enable; an NDMA channel left armed ignores the next start's registers and
// carries on with the old transfer.
static void cam_stop_dma(void)
{
    cameraStopTransfer();
    REG_NDMA_CR(CAMERA_NDMA) = 0;
}

// One PREVIEW transfer running again, in the buffer the strips are not
// showing.  Shared by every path that returns the sensor to live view.
// The retry allowance is reset only by a frame that arrives (camera_tick),
// so a stream that keeps stalling reaches FAILED instead of restarting
// forever.
static int cam_start_preview(void)
{
    if (!cameraStartTransfer(cam_pv[1 - cam_front],
                             MCUREG_APT_SEQ_CMD_PREVIEW, CAMERA_NDMA)) {
        return 0;
    }
    cam_transfer = CAM_T_PREVIEW;
    cam_transfer_at = host_frame_count();
    return 1;
}

// SETTINGS -> LINK -> LIVE VIEW, as the wire's format byte.
static int cam_view_format(void)
{
    return host_settings()->liveview == SETTINGS_LIVEVIEW_COLOUR ? CAMERA_VIEW_COLOUR
                                                            : CAMERA_VIEW_GREY;
}

// The first open of a boot times the two costs that gate a camera session:
// sealing a preview frame, and building one.  A local seal on a dummy key --
// the live one's seq must never move for a benchmark.  Timed in scanlines
// (REG_VCOUNT wraps at 263), median of the runs, reported in ms.
static void camera_bench(void)
{
    static seal_t bench;
    // Header, frame and tag: seal_frame() needs room for the tag it appends.
    static uint8_t dummy[PROTO_HEADER_SIZE + PROTO_CAMERA_FRAME_SIZE + SEAL_TAG];
    static uint8_t px[CAMERA_VIEW_BYTES_MAX];
    const uint8_t key[SEAL_KEY] = {0};
    const uint8_t nonce[PROTO_NONCE_SIZE] = {0};
    // A real CAMERA_FRAME, in the view this card is set to: seal_frame()
    // refuses anything whose header does not describe it, and a refused
    // seal would time as nothing.
    memset(px, 0xa5, sizeof(px));
    const int view = cam_view_format();
    const size_t framed = proto_encode_camera_frame(dummy, view, 0, 0, px);
    memset(cam_pv[0], 0, sizeof(cam_pv[0]));

    unsigned lines[8];
    for (int i = 0; i < 8; i++) {
        seal_start(&bench, key, sizeof(key), nonce,
                   (const uint8_t *)"bench", 5);
        const int a = REG_VCOUNT;
        seal_frame(&bench, dummy, framed, sizeof(dummy));
        int d = (int)REG_VCOUNT - a;
        if (d < 0) d += 263;
        lines[i] = (unsigned)d;
    }
    unsigned s = 0;
    for (int i = 0; i < 4; i++) {
        const int a = REG_VCOUNT;
        (void)camera_view_downscale(view, cam_pv[0], px);
        int d = (int)REG_VCOUNT - a;
        if (d < 0) d += 263;
        s += (unsigned)d;
    }
    // Medians: the sorted middle of each run.
    for (int i = 1; i < 8; i++) {
        const unsigned v = lines[i];
        int j = i - 1;
        while (j >= 0 && lines[j] > v) { lines[j + 1] = lines[j]; j--; }
        lines[j + 1] = v;
    }
    const unsigned seal_med2 = lines[3] + lines[4];   // x2, in half-lines
    const unsigned scale_med = s / 4;
    host_status("seal: %u.%u ms/frame, scale: %u.%u ms (CPU %u MHz)",
           (unsigned)(seal_med2 * 6356u / 200000u),
           (unsigned)((seal_med2 * 6356u / 20000u) % 10u),
           (unsigned)(scale_med * 6356u / 100000u),
           (unsigned)((scale_med * 6356u / 10000u) % 10u),
           (REG_SCFG_CLK & SCFG_CLK_ARM9_TWL) ? 133u : 67u);
}

// From QUICK's PHOTO row.  A camera on a computer that never sent READY is
// not a camera at all; the row's note says why.
void photo_open(void)
{
    cam_ui = CAMERA_UI_STARTING;
    cam_fail[0] = '\0';
    cam_transfer = CAM_T_NONE;
    cam_frame_new = cam_still_new = 0;
    cam_capture_pending = 0;
    cam_still_owed = 0;
    cam_touch_frames = 0;
    cam_percent_shown = 0;
    camera_pacer_init(&cam_pacer);
    memset(&cam_upload, 0, sizeof(cam_upload));
    camera_shutter_reset(&cam_shutter);
    cam_strips();
    host_frame_yield();          // let STARTING's strip show before init's wait

    if (!cam_bench_done) {
        cam_bench_done = 1;
        camera_bench();
    }
    // The ARM7 sleeps 20 frames after the lid closes; a loop busy in a drain
    // or a stalled send would let it sleep with the sensor and LED on, and
    // on waking the lid is open again, so the lid's way out never runs.
    disableSleep();
    // Up before init: cameraInit() powers the rails before its chip check
    // and can still return false, and photo_close() powers down only what
    // it believes is up.
    cam_hw_up = 1;
    if (!cameraInit()) {
        camera_fail("the camera did not start");
        return;
    }
    if (!cameraSelect(cam_which == PROTO_CAMERA_OUTER ? CAMERA_OUTER
                                                     : CAMERA_INNER)) {
        camera_fail("the camera would not select");
        return;
    }
    camled(1);
    cam_front = 0;
    if (!cam_start_preview()) {
        camera_fail("the camera would not stream");
        return;
    }
    cam_ui = camera_ui_live(cam_ui);
    cam_send_state(NULL);
    cam_strips();
    // A Ctrl or Shift armed on the way here must not fold into the shutter's
    // first byte later, and the chord to leave starts disarmed.
    if (kbd_ctrl_armed()) kbd_ctrl_arm();
    kbd_shift_consume();
    camera_shutter_reset(&cam_shutter);
}

// Every way out, fault or not.  `announce` sends CAMERA 0 -- only ever on a
// live, proved link, so connection_forget() passes 0 (a dead socket is told
// nothing, and the send path's own failure must not recurse into here).
void photo_close(int announce)
{
    if (cam_hw_up) {
        // Up to ~100 ms for a frame in flight to land, then force it.
        for (int i = 0; i < 6 && ndmaBusy(CAMERA_NDMA); i++) host_frame_yield();
        cam_stop_dma();
        cameraDeinit();
        cam_hw_up = 0;
        camled(0);
    }
    enableSleep();
    cam_transfer = CAM_T_NONE;
    cam_ui = CAMERA_UI_OFF;
    if (announce && camera_proved && host_link_up()) {
        uint8_t out[PROTO_HEADER_SIZE + 1];
        const size_t n = proto_encode_camera(out, PROTO_CAMERA_OFF);
        (void)host_send_camera(out, n, NULL);
    }
    camera_pacer_init(&cam_pacer);
    memset(&cam_upload, 0, sizeof(cam_upload));
    camera_shutter_reset(&cam_shutter);
    cam_frame_new = cam_still_new = 0;
    cam_capture_pending = 0;
    cam_still_owed = 0;
    cam_touch_frames = 0;
    if (camera_available) {
        memset(cam_pv, 0, sizeof(cam_pv));
        memset(cam_yuyv, 0, sizeof(cam_yuyv));
        memset(cam_packed, 0, sizeof(cam_packed));
        memset(cam_still, 0, sizeof(cam_still));
    }
    subscreen_set_camera(0, NULL, NULL, 0);
    host_refresh_sub();
    host_overlay_sync();
}

// The other sensor, in LIVE or REVIEW only: drain the running transfer so no
// half-written frame is read, then restart the stream on it.
static void camera_flip(void)
{
    for (int i = 0; i < 120 && ndmaBusy(CAMERA_NDMA); i++) host_frame_yield();
    cam_stop_dma();
    cam_transfer = CAM_T_NONE;
    cam_which = cam_which == PROTO_CAMERA_OUTER ? PROTO_CAMERA_INNER
                                                : PROTO_CAMERA_OUTER;
    cameraSelect(cam_which == PROTO_CAMERA_OUTER ? CAMERA_OUTER
                                                : CAMERA_INNER);
    camled(1);
    if (!cam_start_preview()) {
        camera_fail("the camera would not stream");
        return;
    }
    cam_ui = CAMERA_UI_LIVE;
    cam_still_owed = 0;
    cam_capture_pending = 0;
    camera_pacer_init(&cam_pacer);
    cam_send_state(NULL);
    cam_strips();
}

// FAILED's A or B: the camera may still be running (a save that went wrong)
// or never have come up (an init that failed); either way LIVE is re-entered
// through the same door.
static void camera_resume_live(void)
{
    if (!cam_hw_up) {
        cam_hw_up = 1;           // before init, as in camera_open()
        if (!cameraInit() ||
            !cameraSelect(cam_which == PROTO_CAMERA_OUTER ? CAMERA_OUTER
                                                         : CAMERA_INNER)) {
            camera_fail("the camera did not start");
            return;
        }
        camled(1);
    }
    if (cam_transfer != CAM_T_NONE) {
        cam_stop_dma();
        cam_transfer = CAM_T_NONE;
    }
    if (!cam_start_preview()) {
        camera_fail("the camera would not stream");
        return;
    }
    cam_ui = CAMERA_UI_LIVE;
    cam_still_owed = 0;
    cam_capture_pending = 0;
    camera_pacer_init(&cam_pacer);
    cam_send_state(NULL);
    cam_strips();
}

// REVIEW's A: pack the capture for the wire, checksum it once, and let the
// tick drive the windowed chunks.
static void camera_send_photo(void)
{
    cam_fmt = host_settings()->picture == SETTINGS_PICTURE_SMALL
                  ? PROTO_PHOTO_FORMAT_SMALL : PROTO_PHOTO_FORMAT_FULL;
    camera_photo_pack(cam_yuyv, cam_fmt, cam_packed);
    cam_crc = camera_crc32_update(0, cam_packed, camera_photo_bytes(cam_fmt));
    camera_upload_init(&cam_upload, cam_fmt);
    cam_sent_at = host_frame_count();
    cam_progress_at = host_frame_count();
    cam_percent_shown = 0;
    cam_ui = camera_ui_send(cam_ui);
    cam_strips();
}

// What PHOTO_SAVED's failure statuses say, in the plain words the strip
// shows.  The detail the computer logged is on the computer.
static const char *cam_saved_reason(unsigned status)
{
    switch (status) {
    case PROTO_SAVED_BUSY:      return "the computer is busy; try again";
    case PROTO_SAVED_INTEGRITY: return "it arrived damaged; try again";
    case PROTO_SAVED_DISK:      return "the computer could not save it";
    case PROTO_SAVED_TOO_SOON:  return "wait a moment, then try again";
    case PROTO_SAVED_BAD_NAME:
        return "the photo folder's name is not plain ASCII";
    default:                    return "the computer refused it";
    }
}

// One main-loop pass of the camera, while the overlay owns the screen.
void photo_tick(unsigned long *tx)
{
    // --- the transfers -------------------------------------------------
    if (cam_transfer != CAM_T_NONE) {
        // Done is the NDMA having moved the whole frame.  CAM_CNT's transfer
        // enable is the software's own switch and does not clear itself
        // (libnds: "call cameraStopTransfer() after the NDMA transfer has
        // ended"), so it cannot be part of the test.
        if (!ndmaBusy(CAMERA_NDMA)) {
            const int was = cam_transfer;
            cam_stop_dma();
            cam_transfer = CAM_T_NONE;
            cam_transfer_retry = 0;          // a frame arrived
            if (was == CAM_T_PREVIEW) {
                // The finished half becomes the shown one; NDMA fills the
                // other.  The cache holds stale lines, so invalidate before
                // the CPU reads a pixel.
                DC_InvalidateRange(cam_pv[1 - cam_front],
                                   sizeof(cam_pv[0]));
                cam_front = 1 - cam_front;
                cam_frame_new = 1;
                if (cam_capture_pending && cam_ui == CAMERA_UI_LIVE) {
                    cam_capture_pending = 0;
                    if (!cameraStartTransfer((uint16_t *)cam_yuyv,
                                             MCUREG_APT_SEQ_CMD_CAPTURE,
                                             CAMERA_NDMA)) {
                        camera_fail("the camera would not capture");
                    } else {
                        cam_transfer = CAM_T_CAPTURE;
                        cam_transfer_at = host_frame_count();
                    }
                } else if (cam_ui == CAMERA_UI_LIVE) {
                    (void)cam_start_preview();
                }
            } else {
                // The still: invalidate the 640x480 YUYV before the CPU
                // touches it, thumbnail it for the strip, and owe the wire
                // a STILL frame so the computer sees what was taken.
                DC_InvalidateRange(cam_yuyv, sizeof(cam_yuyv));
                camera_photo_thumbnail(cam_yuyv, cam_still);
                cam_still_new = 1;
                cam_still_owed = 1;
                host_key_tick(0);
                cam_ui = camera_ui_shot(cam_ui);
                cam_strips();
            }
        } else {
            const unsigned long waited = host_frame_count() - cam_transfer_at;
            const unsigned long limit =
                cam_transfer == CAM_T_CAPTURE ? 120 : 60;
            if (waited > limit) {
                if (cam_transfer == CAM_T_PREVIEW && !cam_transfer_retry) {
                    // One restart is allowed -- a frame can be dropped.
                    cam_stop_dma();
                    cam_transfer = CAM_T_NONE;
                    if (cam_start_preview()) cam_transfer_retry = 1;
                    else camera_fail("the camera stopped answering");
                } else {
                    // Stopped and cleared first: FAILED keeps ticking, and a
                    // transfer still marked running would fail every frame.
                    const int was = cam_transfer;
                    cam_stop_dma();
                    cam_transfer = CAM_T_NONE;
                    camera_fail(was == CAM_T_CAPTURE
                                    ? "the camera did not answer"
                                    : "the camera stopped answering");
                }
            }
        }
    }

    // --- the preview on the wire ---------------------------------------
    // In LIVE: a downscale of the frame just shown, paced to one in flight
    // every CAMERA_PREVIEW_INTERVAL_FRAMES frames.  The seq the frame
    // carries is the one the pacer assigns at sent -- the ACK must match it.
    if (cam_ui == CAMERA_UI_LIVE &&
        (cam_transfer == CAM_T_PREVIEW || cam_transfer == CAM_T_NONE) &&
        camera_pacer_ready(&cam_pacer, (uint32_t)host_frame_count())) {
        static uint8_t px[CAMERA_VIEW_BYTES_MAX];
        static uint8_t out[PROTO_HEADER_SIZE + PROTO_CAMERA_FRAME_SIZE];
        const int view = cam_view_format();
        (void)camera_view_downscale(view, cam_pv[cam_front], px);
        camera_pacer_sent(&cam_pacer, (uint32_t)host_frame_count());
        const size_t n = proto_encode_camera_frame(out, view, 0, cam_pacer.seq, px);
        if (!cam_send(out, n, tx)) return;
    }

    // REVIEW's one STILL preview: the same downscale over the captured
    // frame's thumbnail, built when the pacer next has room.  The back
    // buffer is free while REVIEW runs, so it is the scratch.
    if (cam_still_owed && cam_ui == CAMERA_UI_REVIEW &&
        camera_pacer_ready(&cam_pacer, (uint32_t)host_frame_count())) {
        static uint8_t px[CAMERA_VIEW_BYTES_MAX];
        static uint8_t out[PROTO_HEADER_SIZE + PROTO_CAMERA_FRAME_SIZE];
        const int view = cam_view_format();
        (void)camera_photo_preview(cam_yuyv, cam_pv[1 - cam_front], view, px);
        camera_pacer_sent(&cam_pacer, (uint32_t)host_frame_count());
        const size_t n = proto_encode_camera_frame(out, view,
                                                   PROTO_PREVIEW_FLAG_STILL,
                                                   cam_pacer.seq, px);
        if (!cam_send(out, n, tx)) return;
        cam_still_owed = 0;
    }

    // --- the upload -----------------------------------------------------
    if (cam_ui == CAMERA_UI_SENDING) {
        uint32_t off = 0, len = 0;
        while (camera_upload_next(&cam_upload, &off, &len)) {
            static uint8_t out[PROTO_HEADER_SIZE + 4 + PROTO_PHOTO_CHUNK_MAX];
            const size_t n = proto_encode_photo_chunk(
                out, off, cam_packed + off, (uint16_t)len);
            if (!cam_send(out, n, tx)) return;
        }
        const unsigned pc = camera_upload_percent(&cam_upload);
        if (pc != cam_percent_shown) {
            cam_percent_shown = pc;
            cam_progress_at = host_frame_count();
            cam_strips();
        } else if (host_frame_count() - cam_progress_at > SENDING_STALL_FRAMES) {
            camera_fail("the computer stopped taking the picture");
            return;
        }
        if (camera_upload_all_sent(&cam_upload)) {
            static uint8_t out[PROTO_HEADER_SIZE + 10];
            const size_t n = proto_encode_photo_end(
                out, (uint8_t)cam_fmt, (uint8_t)cam_which, cam_crc);
            if (!cam_send(out, n, tx)) return;
            cam_ui = camera_ui_sending(cam_ui);
            cam_saving_at = host_frame_count();
            const unsigned long took = host_frame_count() - cam_sent_at;
            const unsigned long kb = camera_photo_bytes(cam_fmt) / 1024;
            host_status("picture sent: %lu KB in %lu.%lu s (%lu KB/s)",
                   kb, took / 60, took % 60 * 10 / 60,
                   took ? kb * 60 / took : kb);
            cam_strips();
        }
    }

    // The computer answers or it does not: half a minute is long enough.
    if (cam_ui == CAMERA_UI_SAVING && host_frame_count() - cam_saving_at > 30 * 60) {
        camera_fail("the computer did not answer");
    }
}

// A tap on the FLIP tile, tracked the way poll_touch tracks a key: the first
// frame's coordinate is garbage, the second's is real.
static void camera_touch(void)
{
    if (!host_touch_held()) {
        cam_touch_frames = 0;
        return;
    }
    if (++cam_touch_frames != 2) return;
    int x = 0, y = 0;
    host_touch_point(&x, &y);
    if (camera_flip_hit(x, y) &&
        (cam_ui == CAMERA_UI_LIVE || cam_ui == CAMERA_UI_REVIEW)) {
        camera_flip();
    }
}

photo_exit_t photo_input(unsigned raw, unsigned edge)
{
    // Every byte the camera sends goes out of photo_tick.
    // Closed over open: the lid, the leave chord, X and SELECT all leave --
    // and leaving while SENDING or SAVING is the cancel, which camera_close
    // reports as CAMERA 0 so the computer drops the half picture.
    if (keysHeld() & KEY_LID) {
        return PHOTO_EXIT_QUICK;
    }
    if (camera_lr_chord(raw, edge)) {
        return PHOTO_EXIT_QUICK;
    }
    if (edge & KEY_X) {
        return PHOTO_EXIT_CONTROL;
    }
    if (edge & KEY_SELECT) {
        return PHOTO_EXIT_NEXT;
    }

    switch (cam_ui) {
    case CAMERA_UI_LIVE:
        if (edge & (KEY_B | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
            return PHOTO_EXIT_QUICK;
        }
        if (edge & KEY_Y) {
            camera_flip();
            return PHOTO_STAY;
        }
        if (camera_shutter_input(&cam_shutter,
                                 camera_shutter_key_mask(host_settings()->camera_shutter),
                                 camera_shutter_other_mask(host_settings()->camera_shutter),
                                 raw, edge)) {
            // The running transfer finishes first; the next pass of the tick
            // turns the flag into the capture.
            cam_capture_pending = 1;
        }
        break;
    case CAMERA_UI_REVIEW:
        if (edge & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
            return PHOTO_EXIT_QUICK;
        }
        if (edge & KEY_Y) {
            camera_flip();
            return PHOTO_STAY;
        }
        if (edge & KEY_B) {
            // Retake: the stream restarts and the computer hears the state.
            camera_resume_live();
            return PHOTO_STAY;
        }
        if (edge & KEY_A && camera_can_save) {
            camera_send_photo();
        }
        break;
    case CAMERA_UI_FAILED:
        if (edge & (KEY_A | KEY_B)) {
            camera_resume_live();
            return PHOTO_STAY;
        }
        if (edge & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
            return PHOTO_EXIT_QUICK;
        }
        break;
    case CAMERA_UI_SENDING:
    case CAMERA_UI_SAVING:
        // B or the d-pad cancels the upload: the close sends CAMERA 0 and
        // the server's assembler resets on the camera going off.  A does
        // nothing here -- a second impatient A, or A configured as the
        // shutter, must never throw a picture's upload away.
        if (edge & (KEY_B | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
            return PHOTO_EXIT_QUICK;
        }
        break;
    default:
        break;
    }
    camera_touch();
    return PHOTO_STAY;
}


// A PHOTO_SAVED the SAVING state was waiting for.  A refusal is FAILED with
// the computer's reason, in the strip; OK leaves the camera up for the
// caller, which records the picture's id before closing it (photo_close)
// and composing the caption -- in that order, so a close whose send fails
// and reconnects forgets the id with the rest of the connection.
int photo_saved_ok(const photo_saved_t *saved)
{
    if (saved->status != PROTO_SAVED_OK) {
        camera_fail(cam_saved_reason(saved->status));
        return 0;
    }
    return 1;
}

// -- what main.c asks ----------------------------------------------------

void photo_boot(int available)
{
    camera_available = available;
}

int photo_available(void)
{
    return camera_available;
}

int photo_is_open(void)
{
    return cam_ui != CAMERA_UI_OFF;
}

int photo_uploading(void)
{
    return cam_ui == CAMERA_UI_SENDING;
}

int photo_saving(void)
{
    return cam_ui == CAMERA_UI_SAVING;
}

int photo_proved(void)
{
    return camera_proved;
}

int photo_can_save(void)
{
    return camera_can_save;
}

void photo_ready(int can_save)
{
    camera_proved = 1;
    camera_can_save = can_save;
}

int photo_ack(uint8_t kind, uint32_t value)
{
    if (kind == PROTO_ACK_PREVIEW) {
        camera_pacer_ack(&cam_pacer, (uint8_t)value);
    } else if (kind == PROTO_ACK_PHOTO &&
               (cam_ui == CAMERA_UI_SENDING || cam_ui == CAMERA_UI_SAVING)) {
        // SAVING counts too: the last chunks' credits keep landing after
        // PHOTO_END.  Outside an upload an ack is stale, not a fault.
        if (!camera_upload_ack(&cam_upload, value)) return 0;
    }
    return 1;
}

void photo_forget(void)
{
    if (cam_ui != CAMERA_UI_OFF) photo_close(0);
    camera_proved = 0;
    camera_can_save = 0;
}

void photo_blit(void)
{
    if (cam_frame_new) {
        cam_frame_new = 0;
        subscreen_camera_blit(cam_pv[cam_front]);
    }
    if (cam_still_new) {
        cam_still_new = 0;
        // Written by the CPU, copied by DMA: flush, or a few stale cache
        // lines show in the review frame.
        DC_FlushRange(cam_still, sizeof(cam_still));
        subscreen_camera_blit(cam_still);
    }
}
