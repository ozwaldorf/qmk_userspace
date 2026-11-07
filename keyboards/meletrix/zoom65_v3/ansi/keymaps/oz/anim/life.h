/* Conway's Game of Life on the key grid, wrapping at the edges. The board starts empty, key presses
 * spawn cells and so does idle time. Generations are simulated ahead and shown through a cubic
 * B-spline over four of them, so no cell changes direction sharply when a generation ends */

#define LIFE_WINDOW 4
#define LIFE_AGES 8
/* Palette entries an aging cell moves through per generation, each a small step of hue */
#define LIFE_COLOR_STEPS 4
#define LIFE_COLOR_HUE 3
#define LIFE_COLORS ((LIFE_AGES - 1) * LIFE_COLOR_STEPS + 1)
/* How much faster a cell returns to the young end of the palette when it is reborn */
#define LIFE_COLOR_RETURN 4

#define LIFE_STEP_SLOW_MS 1200
#define LIFE_STEP_FAST_MS 150
#define LIFE_IDLE_SPAWN_MS 1000

static uint32_t life_last_ms;
static uint16_t life_states[LIFE_WINDOW][ANIM_ROWS]; // oldest first, one bit per column
static uint8_t  life_age[ANIM_COLS][ANIM_ROWS];      // kept while a cell fades out
static uint8_t  life_color[ANIM_COLS][ANIM_ROWS];    // palette entry shown, follows the age
static uint8_t  life_level[ANIM_COLS][ANIM_ROWS];
static rgb_t    life_palette[LIFE_COLORS];
static uint16_t life_step_ms;
static uint16_t life_idle_ms;
static uint16_t life_color_ms;

static inline bool life_alive(const uint16_t *state, int8_t col, int8_t row) {
    return state[(row + ANIM_ROWS) % ANIM_ROWS] >> ((col + ANIM_COLS) % ANIM_COLS) & 1;
}

static void life_next(const uint16_t *state, uint16_t *next) {
    memset(next, 0, sizeof(life_states[0]));
    for (uint8_t col = 0; col < ANIM_COLS; col++) {
        for (uint8_t row = 0; row < ANIM_ROWS; row++) {
            uint8_t neighbors = 0;
            for (int8_t dc = -1; dc <= 1; dc++) {
                for (int8_t dr = -1; dr <= 1; dr++) {
                    if ((dc || dr) && life_alive(state, col + dc, row + dr)) neighbors++;
                }
            }
            if (neighbors == 3 || (neighbors == 2 && life_alive(state, col, row))) next[row] |= 1 << col;
        }
    }
}

/* Moves the window on a generation and ages the cells of the new one */
static void life_advance(void) {
    memmove(life_states[0], life_states[1], sizeof(life_states[0]) * (LIFE_WINDOW - 1));
    life_next(life_states[LIFE_WINDOW - 2], life_states[LIFE_WINDOW - 1]);
    for (uint8_t col = 0; col < ANIM_COLS; col++) {
        for (uint8_t row = 0; row < ANIM_ROWS; row++) {
            if (life_alive(life_states[LIFE_WINDOW - 1], col, row)) {
                life_age[col][row] = life_alive(life_states[LIFE_WINDOW - 2], col, row) ? MIN(life_age[col][row] + 1, LIFE_AGES - 1) : 0;
            }
        }
    }
}

/* Called from the keymap on key down. Goes in the generation fading in now, so it shows at once */
void life_key_pressed(uint8_t matrix_row, uint8_t matrix_col) {
    life_idle_ms = 0;
    if (matrix_row >= MATRIX_ROWS || matrix_col >= MATRIX_COLS) return;

    uint8_t led = g_led_config.matrix_co[matrix_row][matrix_col];
    if (led >= ANIM_LEDS) return;

    anim_key_t key = anim_keys[led];
    for (uint8_t c = anim_key_first_cell(key); c < anim_key_end_cell(key); c++) {
        uint8_t col = ANIM_COLS - 1 - c;
        if (!life_alive(life_states[LIFE_WINDOW - 2], col, key.row)) life_age[col][key.row] = 0;
        life_states[LIFE_WINDOW - 2][key.row] |= 1 << col;
    }
    life_next(life_states[LIFE_WINDOW - 2], life_states[LIFE_WINDOW - 1]);
}

static void life_update(effect_params_t *params) {
    if (params->init) {
        srand(g_rgb_timer);
        life_last_ms  = g_rgb_timer;
        life_step_ms  = 0;
        life_idle_ms  = 0;
        life_color_ms = 0;
        memset(life_states, 0, sizeof(life_states));
    }

    uint16_t dt      = anim_frame_dt(&life_last_ms);
    uint16_t step_ms = LIFE_STEP_SLOW_MS - rgb_matrix_config.speed * (LIFE_STEP_SLOW_MS - LIFE_STEP_FAST_MS) / 255;

    life_idle_ms += dt;
    life_step_ms += dt;
    life_color_ms += dt;
    bool color_tick = life_color_ms >= step_ms / LIFE_COLOR_STEPS;
    if (color_tick) life_color_ms -= step_ms / LIFE_COLOR_STEPS;

    if (life_step_ms >= step_ms) {
        life_step_ms %= step_ms;
        life_advance();
        // The newest generation has no weight yet, so this fades in from nothing
        if (life_idle_ms >= LIFE_IDLE_SPAWN_MS) {
            life_idle_ms       = 0;
            uint8_t col        = rand() % ANIM_COLS;
            uint8_t row        = rand() % ANIM_ROWS;
            life_age[col][row] = 0;
            life_states[LIFE_WINDOW - 1][row] |= 1 << col;
        }
    }

    // B-spline weights for progress through the generation, 8 bit
    int32_t t = (uint32_t)life_step_ms * 256 / step_ms, u = 256 - t;
    int32_t weights[LIFE_WINDOW] = {u * u * u >> 8, (3 * t * t * t - 1536 * t * t + 67108864) >> 8, (-3 * t * t * t + 768 * t * t + 196608 * t + 16777216) >> 8, t * t * t >> 8};
    int32_t total                = weights[0] + weights[1] + weights[2] + weights[3];

    for (uint8_t col = 0; col < ANIM_COLS; col++) {
        for (uint8_t row = 0; row < ANIM_ROWS; row++) {
            int32_t sum = 0;
            for (uint8_t w = 0; w < LIFE_WINDOW; w++) {
                if (life_alive(life_states[w], col, row)) sum += weights[w];
            }
            // The curve runs in PWM duty, so crossfading cells hold the board's light steady
            life_level[col][row] = sqrt16((uint16_t)(sum * 255 / total) << 8);

            // Colors glide to the age too, only a cell that cannot be seen may jump
            uint8_t *color  = &life_color[col][row];
            uint8_t  target = life_age[col][row] * LIFE_COLOR_STEPS;
            if (life_level[col][row] == 0) {
                *color = target;
            } else if (color_tick && *color < target) {
                (*color)++;
            } else if (color_tick && *color > target) {
                *color = MAX(*color - LIFE_COLOR_RETURN, target);
            }
        }
    }

    hsv_t hsv = rgb_matrix_config.hsv;
    for (uint8_t color = 0; color < LIFE_COLORS; color++) {
        life_palette[color] = rgb_matrix_hsv_to_rgb(hsv);
        hsv.h += LIFE_COLOR_HUE;
    }
}

static bool AUTO_LIFE(effect_params_t *params) {
    RGB_MATRIX_USE_LIMITS(led_min, led_max);

    if (params->iter == 0) life_update(params);

    for (uint8_t i = led_min; i < led_max; i++) {
        RGB_MATRIX_TEST_LED_FLAGS();
        anim_paint_cells(i, life_level, life_color, life_palette);
    }

    return rgb_matrix_check_finished_leds(led_max);
}
