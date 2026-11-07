/* Self-playing tetris on a well lying on its side, pieces fall from the left and stack on the
 * right */

#define TETRIS_LINES ANIM_COLS
#define TETRIS_LANES ANIM_ROWS
#define TETRIS_FULL_LINE ((1 << TETRIS_LANES) - 1)
#define TETRIS_PIECES 7
#define TETRIS_FLASH TETRIS_PIECES
/* Lane that lets every rotation of every piece fit */
#define TETRIS_SPAWN_LANE 1

#define TETRIS_STEP_SLOW_MS 400
#define TETRIS_STEP_FAST_MS 70
#define TETRIS_FADE_IN_MS 60
#define TETRIS_FADE_OUT_MS 180
#define TETRIS_CLEAR_FLASH_MS 180
#define TETRIS_OVER_HOLD_MS 600
#define TETRIS_OVER_SWEEP_MS 60

enum { TETRIS_FALLING, TETRIS_CLEARING, TETRIS_OVER };

/* 4x4 boxes per rotation, the top bit is the lowest line of the first lane */
// clang-format off
static const uint16_t tetris_shapes[TETRIS_PIECES][4] = {
    {0x0F00, 0x2222, 0x00F0, 0x4444}, // I
    {0x44C0, 0x8E00, 0x6440, 0x0E20}, // J
    {0x4460, 0x0E80, 0xC440, 0x2E00}, // L
    {0xCC00, 0xCC00, 0xCC00, 0xCC00}, // O
    {0x06C0, 0x8C40, 0x6C00, 0x4620}, // S
    {0x0E40, 0x4C40, 0x4E00, 0x4640}, // T
    {0x0C60, 0x4C80, 0xC600, 0x2640}, // Z
};
// clang-format on

#define TETRIS_FOR_BLOCKS(shape, b)  \
    for (uint8_t b = 0; b < 16; b++) \
        if ((shape) & (0x8000 >> b))
#define TETRIS_BLOCK_LINE(line, b) ((line) + ((b) & 3))
#define TETRIS_BLOCK_LANE(lane, b) ((lane) + ((b) >> 2))

static uint32_t tetris_last_ms;
static uint8_t  tetris_well[TETRIS_LINES];                // one bit per lane
static uint8_t  tetris_color[TETRIS_LINES][TETRIS_LANES]; // kept while a cell fades out
static uint8_t  tetris_level[TETRIS_LINES][TETRIS_LANES];
static rgb_t    tetris_palette[TETRIS_PIECES + 1];

static uint8_t  tetris_state;
static uint16_t tetris_state_ms;
static uint16_t tetris_clearing; // one bit per line

static uint8_t tetris_piece, tetris_rot, tetris_target_rot;
static int8_t  tetris_line, tetris_lane, tetris_target_lane;

/* Lines above the well are open, pieces enter through them */
static bool tetris_fits(uint16_t shape, int8_t line, int8_t lane) {
    TETRIS_FOR_BLOCKS(shape, b) {
        int8_t l = TETRIS_BLOCK_LINE(line, b), r = TETRIS_BLOCK_LANE(lane, b);
        if (r < 0 || r >= TETRIS_LANES || l < 0) return false;
        if (l < TETRIS_LINES && (tetris_well[l] & (1 << r))) return false;
    }
    return true;
}

/* Rates the board a drop down a lane leaves behind, weights tuned by simulation for this well size */
static int32_t tetris_score(uint16_t shape, int8_t lane) {
    int8_t line = TETRIS_LINES;
    while (tetris_fits(shape, line - 1, lane))
        line--;

    uint8_t well[TETRIS_LINES];
    memcpy(well, tetris_well, sizeof(well));
    TETRIS_FOR_BLOCKS(shape, b) {
        int8_t l = TETRIS_BLOCK_LINE(line, b);
        if (l >= TETRIS_LINES) return INT32_MIN;
        well[l] |= 1 << TETRIS_BLOCK_LANE(lane, b);
    }

    uint8_t kept = 0;
    for (uint8_t l = 0; l < TETRIS_LINES; l++) {
        if (well[l] != TETRIS_FULL_LINE) well[kept++] = well[l];
    }
    int32_t cleared = TETRIS_LINES - kept;

    int32_t total = 0, holes = 0, bumps = 0, previous = 0;
    for (uint8_t r = 0; r < TETRIS_LANES; r++) {
        int32_t height = 0;
        for (uint8_t l = 0; l < kept; l++) {
            if (well[l] & (1 << r)) {
                holes += l - height;
                height = l + 1;
            }
        }
        total += height;
        if (r > 0) bumps += abs(height - previous);
        previous = height;
    }
    return 68 * cleared - 74 * total - 43 * holes - 56 * bumps;
}

/* Picks the best placement for the new piece */
static void tetris_choose_target(void) {
    int32_t best_score = INT32_MIN;
    for (uint8_t rot = 0; rot < 4; rot++) {
        uint16_t shape = tetris_shapes[tetris_piece][rot];
        for (int8_t lane = -2; lane < TETRIS_LANES; lane++) {
            if (!tetris_fits(shape, TETRIS_LINES, lane)) continue;

            int32_t score = tetris_score(shape, lane);
            if (score >= best_score) {
                best_score         = score;
                tetris_target_rot  = rot;
                tetris_target_lane = lane;
            }
        }
    }
}

static void tetris_spawn(void) {
    tetris_piece    = rand() % TETRIS_PIECES;
    tetris_rot      = 0;
    tetris_line     = TETRIS_LINES;
    tetris_lane     = TETRIS_SPAWN_LANE;
    tetris_state_ms = 0;
    tetris_clearing = 0;
    tetris_state    = TETRIS_FALLING;
    tetris_choose_target();
}

static void tetris_reset(void) {
    memset(tetris_well, 0, sizeof(tetris_well));
    tetris_spawn();
}

/* One rotation or one sidestep toward the target, a blocked move waits for the next step */
static void tetris_steer(void) {
    if (tetris_rot != tetris_target_rot) {
        uint8_t rot = (tetris_rot + 1) & 3;
        if (tetris_fits(tetris_shapes[tetris_piece][rot], tetris_line, tetris_lane)) tetris_rot = rot;
    } else if (tetris_lane != tetris_target_lane) {
        int8_t lane = tetris_lane + (tetris_target_lane > tetris_lane ? 1 : -1);
        if (tetris_fits(tetris_shapes[tetris_piece][tetris_rot], tetris_line, lane)) tetris_lane = lane;
    }
}

static void tetris_lock(void) {
    bool topped_out = false;
    TETRIS_FOR_BLOCKS(tetris_shapes[tetris_piece][tetris_rot], b) {
        int8_t l = TETRIS_BLOCK_LINE(tetris_line, b), r = TETRIS_BLOCK_LANE(tetris_lane, b);
        if (l >= TETRIS_LINES) {
            topped_out = true;
        } else {
            tetris_well[l] |= 1 << r;
            tetris_color[l][r] = tetris_piece;
        }
    }

    tetris_state_ms = 0;
    if (topped_out) {
        tetris_state = TETRIS_OVER;
        return;
    }

    for (uint8_t l = 0; l < TETRIS_LINES; l++) {
        if (tetris_well[l] == TETRIS_FULL_LINE) {
            tetris_clearing |= 1 << l;
            memset(tetris_color[l], TETRIS_FLASH, TETRIS_LANES);
        }
    }
    if (tetris_clearing) {
        tetris_state = TETRIS_CLEARING;
    } else {
        tetris_spawn();
    }
}

/* Drops the cleared lines. An empty cell keeps its color, it may still be fading out */
static void tetris_collapse(void) {
    uint8_t kept = 0;
    for (uint8_t l = 0; l < TETRIS_LINES; l++) {
        if (tetris_clearing & (1 << l)) continue;
        tetris_well[kept] = tetris_well[l];
        for (uint8_t r = 0; r < TETRIS_LANES; r++) {
            if (tetris_well[l] & (1 << r)) tetris_color[kept][r] = tetris_color[l][r];
        }
        kept++;
    }
    memset(tetris_well + kept, 0, TETRIS_LINES - kept);
}

static void tetris_advance(uint16_t dt) {
    tetris_state_ms += dt;
    if (tetris_state == TETRIS_FALLING) {
        uint16_t step_ms = TETRIS_STEP_SLOW_MS - rgb_matrix_config.speed * (TETRIS_STEP_SLOW_MS - TETRIS_STEP_FAST_MS) / 255;
        if (tetris_rot == tetris_target_rot && tetris_lane == tetris_target_lane) step_ms /= 2;
        if (tetris_state_ms >= step_ms) {
            tetris_state_ms = 0;
            tetris_steer();
            if (tetris_fits(tetris_shapes[tetris_piece][tetris_rot], tetris_line - 1, tetris_lane)) {
                tetris_line--;
            } else {
                tetris_lock();
            }
        }
    } else if (tetris_state == TETRIS_CLEARING) {
        if (tetris_state_ms >= TETRIS_CLEAR_FLASH_MS + TETRIS_FADE_OUT_MS) {
            tetris_collapse();
            tetris_spawn();
        }
    } else if (tetris_state_ms >= TETRIS_OVER_HOLD_MS + TETRIS_LINES * TETRIS_OVER_SWEEP_MS + TETRIS_FADE_OUT_MS) {
        tetris_reset();
    }
}

/* Whether a settled cell stays lit, clears and the game over sweep put them out */
static bool tetris_settled_lit(uint8_t line, uint8_t lane) {
    if (!(tetris_well[line] & (1 << lane))) return false;
    if (tetris_state == TETRIS_CLEARING) return !(tetris_clearing & (1 << line)) || tetris_state_ms < TETRIS_CLEAR_FLASH_MS;
    if (tetris_state == TETRIS_OVER && tetris_state_ms >= TETRIS_OVER_HOLD_MS) {
        return (tetris_state_ms - TETRIS_OVER_HOLD_MS) / TETRIS_OVER_SWEEP_MS < TETRIS_LINES - line;
    }
    return true;
}

/* Every cell eases to its state, which gives falling, rotating and collapsing the same soft motion */
static void tetris_compute_levels(uint16_t dt) {
    uint16_t up = dt * 255 / TETRIS_FADE_IN_MS, down = dt * 255 / TETRIS_FADE_OUT_MS;
    uint8_t  piece[TETRIS_LINES] = {0};

    if (tetris_state == TETRIS_FALLING) {
        TETRIS_FOR_BLOCKS(tetris_shapes[tetris_piece][tetris_rot], b) {
            int8_t l = TETRIS_BLOCK_LINE(tetris_line, b), r = TETRIS_BLOCK_LANE(tetris_lane, b);
            if (l < TETRIS_LINES) {
                piece[l] |= 1 << r;
                tetris_color[l][r] = tetris_piece;
            }
        }
    }

    for (uint8_t l = 0; l < TETRIS_LINES; l++) {
        for (uint8_t r = 0; r < TETRIS_LANES; r++) {
            anim_ease(&tetris_level[l][r], (piece[l] & (1 << r)) || tetris_settled_lit(l, r), up, down);
        }
    }
}

/* Pieces spread evenly around the hue wheel from the configured hue, clears flash white */
static void tetris_compute_palette(void) {
    hsv_t hsv = rgb_matrix_config.hsv;
    for (uint8_t p = 0; p < TETRIS_PIECES; p++) {
        hsv.h             = rgb_matrix_config.hsv.h + p * 256 / TETRIS_PIECES;
        tetris_palette[p] = rgb_matrix_hsv_to_rgb(hsv);
    }
    hsv.s                        = 0;
    tetris_palette[TETRIS_FLASH] = rgb_matrix_hsv_to_rgb(hsv);
}

static void tetris_update(effect_params_t *params) {
    if (params->init) {
        srand(g_rgb_timer);
        tetris_last_ms = g_rgb_timer;
        memset(tetris_level, 0, sizeof(tetris_level));
        tetris_reset();
    }

    uint16_t dt = anim_frame_dt(&tetris_last_ms);
    tetris_advance(dt);
    tetris_compute_levels(dt);
    tetris_compute_palette();
}

static bool AUTO_TETRIS(effect_params_t *params) {
    RGB_MATRIX_USE_LIMITS(led_min, led_max);

    if (params->iter == 0) tetris_update(params);

    for (uint8_t i = led_min; i < led_max; i++) {
        RGB_MATRIX_TEST_LED_FLAGS();
        anim_paint_cells(i, tetris_level, tetris_color, tetris_palette);
    }

    return rgb_matrix_check_finished_leds(led_max);
}
