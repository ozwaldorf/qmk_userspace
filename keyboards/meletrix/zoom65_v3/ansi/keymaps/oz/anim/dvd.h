/* Bouncing DVD oval with a soft edge. A corner hit grows it over the board, then the fill fades
 * back to the original oval */

/* Positions are fixed point with 1 key unit = 1 << 16 */
#define DVD_FP(units) ((int32_t)(units) << 12)

#define DVD_RADIUS_X DVD_FP(3 * ANIM_UNIT / 2)
#define DVD_RADIUS_Y DVD_FP(5 * ANIM_UNIT / 4)
#define DVD_EDGE DVD_FP(ANIM_UNIT)
#define DVD_BOARD_W DVD_FP(16 * ANIM_UNIT)
#define DVD_BOARD_H DVD_FP(5 * ANIM_UNIT)

/* Key units per second are speed * 1000 / 65536, about 0.6 to 3 */
#define DVD_SPEED_MIN 40
#define DVD_SPEED_RANGE 160
/* Y velocity over X, as n / 256. Chosen with the radii, a near whole ratio of bounce periods
 * clusters corner hits with long gaps between */
#define DVD_VY_RATIO 185

#define DVD_HUE_STEP 48
#define DVD_HUE_EASE_MS 250

/* A bounce counts as a corner hit when the other wall is this close and being approached */
#define DVD_CORNER_TOLERANCE DVD_FP(ANIM_UNIT / 8)

/* Steady growth, a faster start would pass the board height before the curve can be seen */
#define DVD_WAVE_EXPAND_MS 1000
#define DVD_WAVE_FADE_MS 800
/* X radius that covers the board from any corner */
#define DVD_WAVE_RADIUS DVD_FP(19 * ANIM_UNIT)

static uint32_t dvd_last_ms;
static int32_t  dvd_x, dvd_y;
static int8_t   dvd_dir_x, dvd_dir_y;
static uint16_t dvd_hue, dvd_hue_target; // 8.8 offset from the configured hue
static rgb_t    dvd_rgb;

static bool     dvd_wave_active;
static uint16_t dvd_wave_ms;
static int32_t  dvd_wave_radius;
static uint8_t  dvd_wave_level;

/* Move along one axis, mirroring any overshoot past a wall. True on a bounce */
static bool dvd_advance(int32_t *pos, int8_t *dir, int32_t step, int32_t min, int32_t max) {
    *pos += *dir * step;
    if (*pos < min) {
        *pos = 2 * min - *pos;
        *dir = 1;
    } else if (*pos > max) {
        *pos = 2 * max - *pos;
        *dir = -1;
    } else {
        return false;
    }
    return true;
}

/* Whether the oval is closing in on a wall it has not reached yet */
static inline bool dvd_closing_on_wall(int32_t pos, int8_t dir, int32_t min, int32_t max) {
    return dir > 0 ? max - pos <= DVD_CORNER_TOLERANCE : pos - min <= DVD_CORNER_TOLERANCE;
}

static uint32_t dvd_isqrt(uint32_t value) {
    uint32_t root = 0;
    for (uint32_t bit = 1UL << 30; bit > 0; bit >>= 2) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
    }
    return root;
}

/* Distance from a key center to the oval center along its X axis, in 1/256 of a key unit. Y counts
 * for more by the ratio of the radii, so a constant distance traces the oval */
static int32_t dvd_key_distance(anim_key_t key) {
    int32_t dx = (DVD_FP(key.x) - dvd_x) >> 8;
    int32_t dy = ((DVD_FP(ANIM_KEY_Y(key.row)) - dvd_y) >> 8) * DVD_RADIUS_X / DVD_RADIUS_Y;
    return dvd_isqrt(dx * dx + dy * dy);
}

/* Level of a key inside the oval of an X radius, full within it and soft across the edge */
static uint8_t dvd_oval_level(int32_t distance, int32_t radius) {
    int32_t edge   = DVD_EDGE >> 8;
    int32_t inside = (radius >> 8) + edge / 2 - distance;
    return inside <= 0 ? 0 : inside >= edge ? 255 : inside * 255 / edge;
}

static void dvd_wave_update(int32_t dt) {
    if (!dvd_wave_active) return;

    dvd_wave_ms += dt;
    if (dvd_wave_ms >= DVD_WAVE_EXPAND_MS + DVD_WAVE_FADE_MS) {
        dvd_wave_active = false;
    } else if (dvd_wave_ms >= DVD_WAVE_EXPAND_MS) {
        dvd_wave_radius = DVD_WAVE_RADIUS;
        dvd_wave_level  = 255 - (uint32_t)(dvd_wave_ms - DVD_WAVE_EXPAND_MS) * 255 / DVD_WAVE_FADE_MS;
    } else {
        dvd_wave_radius = DVD_RADIUS_X + (DVD_WAVE_RADIUS - DVD_RADIUS_X) / DVD_WAVE_EXPAND_MS * dvd_wave_ms;
        dvd_wave_level  = 255;
    }
}

static void dvd_update(effect_params_t *params) {
    if (params->init) {
        dvd_last_ms     = g_rgb_timer;
        dvd_x           = DVD_BOARD_W / 2;
        dvd_y           = DVD_BOARD_H / 2;
        dvd_dir_x       = 1;
        dvd_dir_y       = 1;
        dvd_hue         = 0;
        dvd_hue_target  = 0;
        dvd_wave_active = false;
    }

    int32_t dt    = anim_frame_dt(&dvd_last_ms);
    int32_t speed = DVD_SPEED_MIN + rgb_matrix_config.speed * DVD_SPEED_RANGE / 255;
    int32_t step  = speed * dt;

    bool hit_x = dvd_advance(&dvd_x, &dvd_dir_x, step, DVD_RADIUS_X, DVD_BOARD_W - DVD_RADIUS_X);
    bool hit_y = dvd_advance(&dvd_y, &dvd_dir_y, step * DVD_VY_RATIO / 256, DVD_RADIUS_Y, DVD_BOARD_H - DVD_RADIUS_Y);

    if (hit_x) dvd_hue_target += DVD_HUE_STEP << 8;
    if (hit_y) dvd_hue_target += DVD_HUE_STEP << 8;

    dvd_wave_update(dt);

    bool closing_x = dvd_closing_on_wall(dvd_x, dvd_dir_x, DVD_RADIUS_X, DVD_BOARD_W - DVD_RADIUS_X);
    bool closing_y = dvd_closing_on_wall(dvd_y, dvd_dir_y, DVD_RADIUS_Y, DVD_BOARD_H - DVD_RADIUS_Y);
    if (!dvd_wave_active && ((hit_x && hit_y) || (hit_x && closing_y) || (hit_y && closing_x))) {
        dvd_wave_active = true;
        dvd_wave_ms     = 0;
        dvd_wave_radius = DVD_RADIUS_X;
        dvd_wave_level  = 255;
    }

    dvd_hue += (int16_t)(dvd_hue_target - dvd_hue) * dt / (dt + DVD_HUE_EASE_MS);

    hsv_t hsv = rgb_matrix_config.hsv;
    hsv.h += dvd_hue >> 8;
    dvd_rgb = rgb_matrix_hsv_to_rgb(hsv);
}

static bool DVD_LOGO(effect_params_t *params) {
    RGB_MATRIX_USE_LIMITS(led_min, led_max);

    if (params->iter == 0) dvd_update(params);

    for (uint8_t i = led_min; i < led_max; i++) {
        RGB_MATRIX_TEST_LED_FLAGS();
        int32_t distance = dvd_key_distance(anim_keys[i]);
        uint8_t level    = dvd_oval_level(distance, DVD_RADIUS_X);

        if (dvd_wave_active) level = MAX(level, scale8(dvd_oval_level(distance, dvd_wave_radius), dvd_wave_level));

        anim_set_scaled(i, dvd_rgb, level);
    }

    return rgb_matrix_check_finished_leds(led_max);
}
