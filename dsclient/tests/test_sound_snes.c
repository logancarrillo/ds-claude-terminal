// The SNES instrument: the sample bank, the two PCM lanes, the blip
// scheduler, and the settings rows that choose all of it.
//
// The rest of the suite pins the chip voice (main() selects it before any
// test runs, so thousands of PSG checks keep meaning what they meant).  Every
// test here switches to SNES for itself and puts CHIP back on the way out.
#include <string.h>

#include "check.h"
#include "support.h"

#include "../source/chime.h"
#include "../source/settings.h"
#include "../source/sfx_assets.h"
#include "../source/sound.h"
#include "../source/wav.h"

static void snes_begin(void)
{
    sound_restore_defaults();
    sound_set_style(SOUND_STYLE_SNES);
    reset_sound();
}

static void snes_end(void)
{
    sound_restore_defaults();
}

// Tick the conductor once and collect a lane's change, if any.
static int tick_take(unsigned lane, sound_pcm_t *out)
{
    sound_voice_t v;
    (void)sound_tick_voice(&v);
    return sound_take_pcm(lane, out);
}

static void test_sfx_bank_is_well_formed(void)
{
    unsigned total = 0;
    for (unsigned i = 0; i < SFX_SAMPLE_COUNT; i++) {
        const sfx_sample_t *s = &SFX_SAMPLES[i];
        CHECK(s->data != NULL, "take %u has no data", i);
        CHECK(s->bytes >= 8 && (s->bytes % 4) == 0,
              "take %u is %u bytes; the hardware counts words", i,
              (unsigned)s->bytes);
        CHECK(s->hz == 16384, "take %u is %u Hz", i, s->hz);
        CHECK(s->frames >= 1 && s->frames <= 120,
              "take %u plays %u frames; a cue is not a song", i, s->frames);
        CHECK(s->gain >= 1 && s->gain <= 100, "take %u gain %u", i, s->gain);
        CHECK(s->format == SFX_FORMAT_ADPCM || s->format == SFX_FORMAT_PCM16,
              "take %u has format %u", i, s->format);
        CHECK(((uintptr_t)s->data % 4) == 0, "take %u is not word-aligned", i);
        total += s->bytes;
    }
    CHECK(total == SFX_TOTAL_BYTES, "bank totals %u, header says %u",
          total, (unsigned)SFX_TOTAL_BYTES);
    // Every bank a cue can ask for has at least one take, and the ranges tile
    // the sample table exactly.
    unsigned covered = 0;
    for (unsigned b = 0; b < SFX_BANK_COUNT; b++) {
        CHECK(SFX_BANKS[b].count >= 1, "bank %u is empty", b);
        CHECK(SFX_BANKS[b].first + SFX_BANKS[b].count <= SFX_SAMPLE_COUNT,
              "bank %u runs off the table", b);
        covered += SFX_BANKS[b].count;
    }
    CHECK(covered == SFX_SAMPLE_COUNT, "banks cover %u of %u takes",
          covered, (unsigned)SFX_SAMPLE_COUNT);
    // The small sounds of your hands are small: a key tick that rang for a
    // quarter second would talk over the next one.
    const unsigned tiny[] = { SFX_KEY, SFX_DELETE, SFX_CURSOR, SFX_BLIP };
    for (unsigned i = 0; i < sizeof(tiny) / sizeof(tiny[0]); i++) {
        const sfx_range_t r = SFX_BANKS[tiny[i]];
        for (unsigned t = 0; t < r.count; t++) {
            const sfx_sample_t *s = &SFX_SAMPLES[r.first + t];
            CHECK(s->frames <= 5, "bank %u take %u is %u frames long",
                  tiny[i], t, s->frames);
            CHECK(s->gain <= 40, "bank %u take %u is loud (%u)",
                  tiny[i], t, s->gain);
        }
    }
}

static int sample_in_bank(const sfx_sample_t *s, sfx_bank_t bank)
{
    const sfx_range_t r = SFX_BANKS[bank];
    return s >= &SFX_SAMPLES[r.first] && s < &SFX_SAMPLES[r.first + r.count];
}

static void test_snes_cue_plays_a_sample_not_a_tone(void)
{
    snes_begin();
    sound_cue(SOUND_DONE);
    sound_voice_t v;
    (void)sound_tick_voice(&v);
    CHECK(v.hz == 0, "SNES style still sounded the square wave at %u Hz", v.hz);
    sound_pcm_t pcm;
    CHECK(sound_take_pcm(SOUND_LANE_CUE, &pcm), "DONE started nothing");
    CHECK(pcm.sample && sample_in_bank(pcm.sample, SFX_DONE),
          "DONE played a take from another bank");
    CHECK(pcm.hz == pcm.sample->hz || pcm.hz == pcm.sample->hz * 1122u / 1000u ||
              pcm.hz == pcm.sample->hz * 891u / 1000u,
          "a cue played at %u Hz, not one of its three variations", pcm.hz);
    CHECK(pcm.volume > 0 && pcm.volume <= 127, "volume %u", pcm.volume);
    CHECK(!sound_take_pcm(SOUND_LANE_CUE, &pcm),
          "the same start was reported twice");
    CHECK(sound_playing(), "a playing sample does not count as playing");
    // It ends on its own clock, and nobody has to stop the hardware.
    const unsigned frames = SFX_SAMPLES[SFX_BANKS[SFX_DONE].first].frames;
    for (unsigned i = 0; i < 130; i++) (void)tick_take(SOUND_LANE_CUE, &pcm);
    CHECK(!sound_lane_busy(SOUND_LANE_CUE), "DONE never finished (%u frames)",
          frames);
    CHECK(sound_idle(), "the conductor is not idle after DONE");
    snes_end();
}

static void test_snes_stays_quiet_when_an_answer_starts(void)
{
    // A turn sounds SENT, THINK and DONE.  "The answer started" said nothing
    // between them, so the sampled set does not play it -- and does not fall
    // through to the square wave either -- while the chip voice keeps it.
    snes_begin();
    sound_cue(SOUND_OUTPUT);
    sound_voice_t v;
    (void)sound_tick_voice(&v);
    sound_pcm_t pcm;
    CHECK(v.hz == 0 && !sound_take_pcm(SOUND_LANE_CUE, &pcm),
          "the sampled set made a sound when an answer started");
    sound_set_style(SOUND_STYLE_CHIP);
    sound_cue(SOUND_OUTPUT);
    (void)sound_tick_voice(&v);
    CHECK(v.hz != 0, "the chip voice lost its output note");
    snes_end();
}

static void test_snes_lanes_keep_typing_and_cues_apart(void)
{
    snes_begin();
    sound_cue(SOUND_DONE);
    sound_pcm_t pcm;
    (void)tick_take(SOUND_LANE_CUE, &pcm);
    key_typed(0);
    sound_pcm_t key;
    CHECK(sound_take_pcm(SOUND_LANE_TICK, &key) && key.sample &&
              sample_in_bank(key.sample, SFX_KEY),
          "a key typed during DONE did not tick");
    CHECK(sound_lane_busy(SOUND_LANE_CUE), "typing cut off DONE");
    // Backspace has its own takes.
    for (int i = 0; i < 4; i++) (void)tick_take(SOUND_LANE_TICK, &key);
    key_typed(1);
    CHECK(sound_take_pcm(SOUND_LANE_TICK, &key) && key.sample &&
              sample_in_bank(key.sample, SFX_DELETE),
          "backspace did not use the delete takes");
    snes_end();
}

static void test_snes_priorities_match_the_chip(void)
{
    snes_begin();
    sound_pcm_t pcm;
    sound_cue(SOUND_SENT);
    (void)tick_take(SOUND_LANE_CUE, &pcm);
    sound_cue(SOUND_THINK);                 // lower: refused
    CHECK(!sound_take_pcm(SOUND_LANE_CUE, &pcm), "THINK talked over SENT");
    sound_cue(SOUND_DROP);                  // the link died: interrupts
    CHECK(sound_take_pcm(SOUND_LANE_CUE, &pcm) && pcm.sample &&
              sample_in_bank(pcm.sample, SFX_DROP),
          "DROP could not interrupt SENT");
    snes_end();
}

static void test_snes_never_repeats_a_take_back_to_back(void)
{
    snes_begin();
    const sfx_sample_t *last = NULL;
    int repeats = 0, distinct = 0;
    const sfx_sample_t *seen[16] = {0};
    for (int i = 0; i < 40; i++) {
        key_typed(0);
        sound_pcm_t pcm;
        if (!sound_take_pcm(SOUND_LANE_TICK, &pcm) || !pcm.sample) continue;
        if (pcm.sample == last) repeats++;
        last = pcm.sample;
        int known = 0;
        for (int k = 0; k < distinct; k++) known |= seen[k] == pcm.sample;
        if (!known && distinct < 16) seen[distinct++] = pcm.sample;
        // A variation (a whole tone either way) and four percent of jitter.
        const long lo = (long)pcm.sample->hz * 891 / 1000 * 958 / 1000;
        const long hi = (long)pcm.sample->hz * 1122 / 1000 * 1042 / 1000;
        CHECK(pcm.hz >= lo && pcm.hz <= hi, "key pitched to %u", pcm.hz);
        for (int f = 0; f < 3; f++) (void)tick_take(SOUND_LANE_TICK, &pcm);
    }
    CHECK(repeats == 0, "the same key take played twice in a row %d times",
          repeats);
    CHECK(distinct == (int)SFX_BANKS[SFX_KEY].count,
          "only %d of %u key takes were ever used", distinct,
          (unsigned)SFX_BANKS[SFX_KEY].count);
    snes_end();
}

static void test_snes_mute_and_style_silence_the_lanes(void)
{
    snes_begin();
    sound_pcm_t pcm;
    sound_cue(SOUND_LINK);
    (void)tick_take(SOUND_LANE_CUE, &pcm);
    sound_set_muted(1);
    CHECK(sound_take_pcm(SOUND_LANE_CUE, &pcm) && pcm.sample == NULL,
          "mute left a sample playing");
    sound_cue(SOUND_DONE);
    CHECK(!sound_take_pcm(SOUND_LANE_CUE, &pcm), "a muted card started DONE");
    sound_set_muted(0);
    sound_cue(SOUND_SWITCH);
    (void)tick_take(SOUND_LANE_CUE, &pcm);
    CHECK(sound_lane_busy(SOUND_LANE_CUE), "SWITCH did not start after unmute");
    sound_set_style(SOUND_STYLE_CHIP);
    CHECK(sound_take_pcm(SOUND_LANE_CUE, &pcm) && pcm.sample == NULL,
          "changing instrument left the old one playing");
    snes_end();
}

// The three variations: over many DONEs all three are heard, and never the
// same one twice in a row.
static void test_snes_three_variations_rotate(void)
{
    snes_begin();
    unsigned seen[3] = {0, 0, 0};
    unsigned last = 99, repeats = 0;
    for (int i = 0; i < 30; i++) {
        sound_cue(SOUND_DONE);
        sound_pcm_t pcm;
        if (!sound_take_pcm(SOUND_LANE_CUE, &pcm) || !pcm.sample) continue;
        const unsigned base = pcm.sample->hz;
        const unsigned v = pcm.hz == base ? 0 : pcm.hz > base ? 1 : 2;
        seen[v]++;
        if (v == last) repeats++;
        last = v;
        // Let it finish and clear the cooldown before the next one.
        for (int f = 0; f < 140; f++) (void)tick_take(SOUND_LANE_CUE, &pcm);
    }
    CHECK(seen[0] && seen[1] && seen[2],
          "the variations heard were %u normal, %u high, %u low",
          seen[0], seen[1], seen[2]);
    CHECK(repeats == 0, "a variation played twice running %u times", repeats);
    snes_end();
}

static void test_snes_approve_and_question(void)
{
    snes_begin();
    sound_pcm_t pcm;
    sound_cue(SOUND_DONE);
    (void)tick_take(SOUND_LANE_CUE, &pcm);
    sound_cue(SOUND_APPROVE);
    CHECK(sound_take_pcm(SOUND_LANE_CUE, &pcm) && pcm.sample &&
              sample_in_bank(pcm.sample, SFX_APPROVE),
          "APPROVE could not interrupt DONE");
    sound_cue(SOUND_QUESTION);
    CHECK(!sound_take_pcm(SOUND_LANE_CUE, &pcm),
          "QUESTION talked over APPROVE");
    // Studio keeps both: being asked for something is consequential.
    for (int f = 0; f < 140; f++) (void)tick_take(SOUND_LANE_CUE, &pcm);
    sound_set_profile(SOUND_PROFILE_STUDIO);
    sound_cue(SOUND_QUESTION);
    CHECK(sound_take_pcm(SOUND_LANE_CUE, &pcm) && pcm.sample,
          "Studio silenced QUESTION");
    snes_end();
}

static void test_blips_are_opt_in_and_never_talk_over(void)
{
    snes_begin();
    sound_pcm_t pcm;
    CHECK(!sound_blip('a'), "a blip sounded with BLIPS off");
    sound_set_blips(1);
    CHECK(sound_blip('a'), "a blip did not sound with BLIPS on");
    CHECK(sound_take_pcm(SOUND_LANE_TICK, &pcm) && pcm.sample &&
              sample_in_bank(pcm.sample, SFX_BLIP),
          "the blip came from the wrong bank");
    for (int i = 0; i < 10; i++) (void)tick_take(SOUND_LANE_TICK, &pcm);
    sound_cue(SOUND_DONE);
    (void)tick_take(SOUND_LANE_CUE, &pcm);
    CHECK(!sound_blip('b'), "a blip talked over DONE");
    // Studio is the profile that promises only the consequential.
    for (int i = 0; i < 130; i++) (void)tick_take(SOUND_LANE_CUE, &pcm);
    sound_set_profile(SOUND_PROFILE_STUDIO);
    CHECK(!sound_blip('c'), "a blip sounded in Studio");
    snes_end();
}

// The scheduler: blips while an answer streams, irregular, and fewer the
// longer it runs.
// Blips counted over `frames` of steady streaming at WRITING `level`.
static unsigned blips_at_level(int level, unsigned frames)
{
    snes_begin();
    sound_set_blips(level);
    chime_init();
    chime_sent();
    unsigned blips = 0;
    for (unsigned f = 1; f < frames; f++) {
        chime_tick();
        if ((f % 4) == 0) chime_cells((unsigned char)('a' + f % 26));
        sound_pcm_t pcm;
        sound_voice_t v;
        (void)sound_tick_voice(&v);
        (void)sound_take_pcm(SOUND_LANE_CUE, &pcm);
        if (sound_take_pcm(SOUND_LANE_TICK, &pcm) && pcm.sample &&
            sample_in_bank(pcm.sample, SFX_BLIP)) {
            blips++;
        }
    }
    snes_end();
    return blips;
}

static void test_writing_levels_are_distinct(void)
{
    // OFF is silent; each level up writes more; even HIGH thins out on a
    // long answer rather than becoming a drone.
    const unsigned off = blips_at_level(SETTINGS_WRITING_OFF, 2400);
    const unsigned low = blips_at_level(SETTINGS_WRITING_LOW, 2400);
    const unsigned mid = blips_at_level(SETTINGS_WRITING_MID, 2400);
    const unsigned high = blips_at_level(SETTINGS_WRITING_HIGH, 2400);
    CHECK(off == 0, "WRITING OFF still blipped %u times", off);
    CHECK(low > 0 && low < mid && mid < high,
          "the levels are not ordered: low %u, mid %u, high %u", low, mid, high);
    const unsigned high_long = blips_at_level(SETTINGS_WRITING_HIGH, 7200);
    CHECK(high_long < high * 3,
          "HIGH did not thin out on a long answer (%u in 2 min vs %u in 40 s)",
          high_long, high);
}

static void test_blip_schedule_thins_out(void)
{
    snes_begin();
    sound_set_blips(SETTINGS_WRITING_MID);
    chime_init();
    chime_sent();
    unsigned blips = 0, last = 0, early_gaps = 0, late_gaps = 0;
    unsigned min_early = 100000, min_late = 100000;
    for (unsigned f = 1; f < 5400; f++) {
        chime_tick();
        // Output arrives every few frames, like a streaming answer.
        if ((f % 4) == 0) chime_cells((unsigned char)('a' + f % 26));
        sound_pcm_t pcm;
        sound_voice_t v;
        (void)sound_tick_voice(&v);
        (void)sound_take_pcm(SOUND_LANE_CUE, &pcm);
        if (sound_take_pcm(SOUND_LANE_TICK, &pcm) && pcm.sample &&
            sample_in_bank(pcm.sample, SFX_BLIP)) {
            if (blips) {
                const unsigned gap = f - last;
                if (f < 1200) { early_gaps++; if (gap < min_early) min_early = gap; }
                if (f > 3700) { late_gaps++; if (gap < min_late) min_late = gap; }
            }
            blips++;
            last = f;
        }
    }
    CHECK(blips > 10, "only %u blips in ninety seconds of streaming", blips);
    CHECK(early_gaps > 3 && min_early >= CHIME_BLIP_GAP_MIN,
          "early blips came %u frames apart", min_early);
    CHECK(late_gaps > 0 && min_late >= 3 * CHIME_BLIP_GAP_MIN,
          "after a minute blips still came %u frames apart", min_late);
    CHECK(late_gaps * 2 < early_gaps,
          "a long answer blipped as often as a short one (%u vs %u)",
          late_gaps, early_gaps);
    snes_end();
}

static void test_settings_sound_rows(void)
{
    settings_t s;
    settings_ui_t ui;
    settings_defaults(&s);
    settings_ui_init(&ui);
    CHECK(s.sound == SETTINGS_SOUND_CHIP && s.volume == SETTINGS_VOLUME_MID &&
              s.blips == SETTINGS_WRITING_MID,
          "sound defaults are not CHIP, MID volume, WRITING MID");
    const char *send = "x";
    CHECK(settings_press(&s, &ui, SETTINGS_ROW_SOUND, 0, &send) ==
              SETTINGS_PRESS_CHANGED && s.sound == SETTINGS_SOUND_SNES &&
              send == NULL,
          "SOUND > SOFT did not change the instrument");
    CHECK(settings_press(&s, &ui, SETTINGS_ROW_SOUND, 0, &send) ==
              SETTINGS_PRESS_NOTHING,
          "pressing the current instrument reported a change");
    CHECK(settings_press(&s, &ui, SETTINGS_ROW_VOLUME, 1, &send) ==
              SETTINGS_PRESS_CHANGED,
          "pressing the current volume should still preview it");
    CHECK(settings_press(&s, &ui, SETTINGS_ROW_BLIPS, SETTINGS_WRITING_LOW, &send) ==
              SETTINGS_PRESS_CHANGED && s.blips == SETTINGS_WRITING_LOW,
          "WRITING > LOW did not set the level");
    CHECK(settings_press(&s, &ui, SETTINGS_ROW_BLIPS, SETTINGS_WRITING_OFF, &send) ==
              SETTINGS_PRESS_CHANGED && s.blips == SETTINGS_WRITING_OFF,
          "WRITING > OFF did not turn them off");
    s.volume = SETTINGS_VOLUME_HIGH;
    s.sound = SETTINGS_SOUND_OFF;

    // The file round trip, and an old file loading with the defaults.
    char err[64];
    settings_t back;
    const char *v3 = "version=3\nsound=off\nvolume=high\nblips=on\n";
    CHECK(settings_parse(v3, &back, err, sizeof(err)) &&
              back.sound == SETTINGS_SOUND_OFF &&
              back.volume == SETTINGS_VOLUME_HIGH && back.blips == SETTINGS_WRITING_MID,
          "a version 3 file (blips=on) did not load as MID: %s", err);
    const char *v2 = "version=2\nmodel=opus\nboot=keys\ngod_mode=off\n";
    CHECK(settings_parse(v2, &back, err, sizeof(err)) &&
              back.sound == SETTINGS_SOUND_CHIP &&
              back.volume == SETTINGS_VOLUME_MID && back.blips == SETTINGS_WRITING_MID &&
              back.boot == SETTINGS_BOOT_KEYS,
          "a version 2 file did not load with sound defaults: %s", err);
    // An unknown value costs that one setting, never the file (rule 3).
    CHECK(settings_parse("version=3\nsound=loud\nvolume=high\n", &back, err,
                         sizeof(err)) &&
              back.sound == SETTINGS_SOUND_CHIP && back.volume == SETTINGS_VOLUME_HIGH,
          "an unknown sound value was not its default alone: %s", err);

    // Every row still finishes above the strip an extension stands on.
    int x, y, w, h;
    const settings_row_t last = (settings_row_t)(SETTINGS_ROW_COUNT - 1);
    const unsigned rows = settings_tab_rows(settings_row_tab(last));
    const unsigned scroll = rows > SETTINGS_VISIBLE_ROWS ? rows - SETTINGS_VISIBLE_ROWS : 0u;
    CHECK(settings_row_rect(last, scroll, &x, &y, &w, &h) && y + h <= 160,
          "the last settings row ends at y=%d", y + h);
}

// ------------------------------------------------------------------ WAV ---

static size_t put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    return 4;
}

static size_t put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    return 2;
}

// A WAV with an optional LIST chunk before the data, the way editors write.
static size_t make_wav(uint8_t *out, uint16_t format, uint16_t channels,
                       uint32_t hz, uint16_t bits, const uint8_t *data,
                       uint32_t data_len, int with_list)
{
    size_t n = 0;
    memcpy(out + n, "RIFF", 4); n += 4;
    const size_t size_at = n; n += 4;
    memcpy(out + n, "WAVE", 4); n += 4;
    memcpy(out + n, "fmt ", 4); n += 4;
    n += put32(out + n, 16);
    n += put16(out + n, format);
    n += put16(out + n, channels);
    n += put32(out + n, hz);
    n += put32(out + n, hz * channels * (bits / 8u));
    n += put16(out + n, (uint16_t)(channels * (bits / 8u)));
    n += put16(out + n, bits);
    if (with_list) {
        memcpy(out + n, "LIST", 4); n += 4;
        n += put32(out + n, 5);          // odd: padded to 6
        memcpy(out + n, "INFOx\0", 6); n += 6;
    }
    memcpy(out + n, "data", 4); n += 4;
    n += put32(out + n, data_len);
    memcpy(out + n, data, data_len); n += data_len;
    put32(out + size_at, (uint32_t)(n - 8));
    return n;
}

static void test_wav_reader(void)
{
    static uint8_t file[4096];
    int16_t pcm[1024];
    char why[64];

    // 16-bit mono with a LIST chunk in front of the data.
    const uint8_t mono16[] = { 0x00, 0x10, 0x00, 0xF0, 0x34, 0x12 };
    size_t n = make_wav(file, 1, 1, 22050, 16, mono16, sizeof(mono16), 1);
    wav_t w = { pcm, 0, 0 };
    CHECK(wav_decode(file, n, &w, 1024, why, sizeof(why)) && w.hz == 22050 &&
              w.samples == 4 && pcm[0] == 0x1000 && pcm[1] == (int16_t)0xF000 &&
              pcm[2] == 0x1234 && pcm[3] == 0,
          "16-bit mono decoded wrong (%s)", why);

    // 8-bit stereo: unsigned, mixed down.
    const uint8_t st8[] = { 128, 128, 255, 255, 0, 0, 255, 1 };
    n = make_wav(file, 1, 2, 11025, 8, st8, sizeof(st8), 0);
    CHECK(wav_decode(file, n, &w, 1024, why, sizeof(why)) && w.samples == 4 &&
              pcm[0] == 0 && pcm[1] == 127 * 256 && pcm[2] == -128 * 256 &&
              pcm[3] == 0,
          "8-bit stereo decoded wrong (%s)", why);

    // Refusals, each with a reason a person can act on.
    n = make_wav(file, 3, 1, 22050, 32, mono16, 4, 0);       // float
    CHECK(!wav_decode(file, n, &w, 1024, why, sizeof(why)) && strstr(why, "PCM"),
          "a float WAV was accepted (%s)", why);
    n = make_wav(file, 1, 1, 22050, 24, mono16, 6, 0);
    CHECK(!wav_decode(file, n, &w, 1024, why, sizeof(why)), "24-bit was accepted");
    n = make_wav(file, 1, 1, 96000, 16, mono16, 6, 0);
    CHECK(!wav_decode(file, n, &w, 1024, why, sizeof(why)), "96 kHz was accepted");
    n = make_wav(file, 1, 1, 22050, 16, mono16, sizeof(mono16), 0);
    put32(file + n - sizeof(mono16) - 4, 4000);              // lie about length
    CHECK(!wav_decode(file, n, &w, 1024, why, sizeof(why)) &&
              strstr(why, "truncated"),
          "a WAV that lies about its size was accepted (%s)", why);
    memcpy(file, "RIFX", 4);
    CHECK(!wav_decode(file, n, &w, 1024, why, sizeof(why)), "not-RIFF was accepted");
    uint32_t hz;
    static uint8_t big[16 + 44 + 4 * 4000 * 2];
    static uint8_t silence[4 * 4000 * 2];
    n = make_wav(big, 1, 1, 4000, 16, silence, sizeof(silence), 0);
    CHECK(wav_measure(big, n, &hz, why, sizeof(why)) == 0 &&
              strstr(why, "3 seconds"),
          "a four-second WAV was accepted (%s)", why);
    n = make_wav(file, 1, 1, 22050, 16, mono16, sizeof(mono16), 0);
    CHECK(!wav_decode(file, n, &w, 2, why, sizeof(why)),
          "a WAV was written past the end of a small buffer");
}

static void test_custom_sounds_replace_a_bank(void)
{
    snes_begin();
    static const int16_t tone[64] __attribute__((aligned(4))) = { 1000 };
    sfx_sample_t mine[2] = {
        { (const uint8_t *)tone, sizeof(tone), 22050, 2, 88, SFX_FORMAT_PCM16 },
        { (const uint8_t *)tone, sizeof(tone), 22050, 2, 88, SFX_FORMAT_PCM16 },
    };
    CHECK(sound_set_custom(SFX_DONE, mine, 2) && sound_custom_banks() == 1,
          "a custom DONE was not taken");
    sound_cue(SOUND_DONE);
    sound_pcm_t pcm;
    CHECK(sound_take_pcm(SOUND_LANE_CUE, &pcm) &&
              (pcm.sample == &mine[0] || pcm.sample == &mine[1]),
          "DONE did not play the card's own take");
    CHECK(!sound_set_custom(SFX_DONE, mine, SOUND_CUSTOM_TAKES + 1),
          "more takes than a bank holds were accepted");
    CHECK(!sound_set_custom(SFX_BANK_COUNT, mine, 1), "a bad bank was accepted");
    CHECK(sound_set_custom(SFX_DONE, NULL, 0) && sound_custom_banks() == 0,
          "the built-in DONE could not be restored");
    for (int f = 0; f < 140; f++) (void)tick_take(SOUND_LANE_CUE, &pcm);
    sound_cue(SOUND_DONE);
    CHECK(sound_take_pcm(SOUND_LANE_CUE, &pcm) && pcm.sample &&
              sample_in_bank(pcm.sample, SFX_DONE),
          "after a restore DONE did not play the built-in take");
    // Every bank has a name a file can be called by.
    for (unsigned b = 0; b < SFX_BANK_COUNT; b++) {
        CHECK(SFX_BANK_NAMES[b] && SFX_BANK_NAMES[b][0] &&
                  strlen(SFX_BANK_NAMES[b]) < 16,
              "bank %u has no usable file name", b);
    }
    snes_end();
}

void test_sound_snes_all(void)
{
    printf("== sound: SNES ==\n");
    test_sfx_bank_is_well_formed();
    test_snes_cue_plays_a_sample_not_a_tone();
    test_snes_stays_quiet_when_an_answer_starts();
    test_snes_lanes_keep_typing_and_cues_apart();
    test_snes_priorities_match_the_chip();
    test_snes_never_repeats_a_take_back_to_back();
    test_snes_mute_and_style_silence_the_lanes();
    test_snes_three_variations_rotate();
    test_snes_approve_and_question();
    test_blips_are_opt_in_and_never_talk_over();
    test_blip_schedule_thins_out();
    test_writing_levels_are_distinct();
    test_settings_sound_rows();
    test_wav_reader();
    test_custom_sounds_replace_a_bank();
}
