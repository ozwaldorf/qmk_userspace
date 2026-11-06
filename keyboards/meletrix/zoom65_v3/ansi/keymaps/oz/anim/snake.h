/* Self-playing snake that walks between physically adjacent keys, easing from key to key */

#define SNAKE_MAX_DEGREE 7
/* Keys in adjacent rows connect when they overlap by this much, excludes 1/4u stagger contact */
#define SNAKE_MIN_OVERLAP (3 * ANIM_UNIT / 8)

#define SNAKE_START_LENGTH 3
/* Head on H with the body trailing over G and F, LED indices rise leftward on this row */
#define SNAKE_START_LED 36
/* Chance per move of a careless lunge at the food, out of 1024 per body segment. Makes long games rare but possible */
#define SNAKE_LUNGE_PER_SEGMENT 8
/* Moves without food before the snake stops playing safe, so it cannot circle forever */
#define SNAKE_MAX_HUNGER (2 * ANIM_LEDS)

#define SNAKE_STEP_SLOW_MS 660
#define SNAKE_STEP_FAST_MS 110
#define SNAKE_FADE_IN_MS 400
#define SNAKE_FOOD_IN_MS 400
#define SNAKE_DEATH_TINT_MS 150
#define SNAKE_DEATH_HOLD_MS 500
#define SNAKE_DEATH_FADE_MS 900

static uint8_t snake_adj[ANIM_LEDS][SNAKE_MAX_DEGREE];
static uint8_t snake_degree[ANIM_LEDS];
static bool    snake_graph_ready;

static uint32_t snake_last_ms;
static uint8_t  snake_body[ANIM_LEDS]; // head first
static bool     snake_occupied[ANIM_LEDS];
static uint8_t  snake_len;
static uint8_t  snake_ghost; // cell the tail is leaving
static uint8_t  snake_food;
static uint8_t  snake_eaten; // food cell being consumed by the head
static uint8_t  snake_eaten_from;
static uint16_t snake_hunger;
static uint32_t snake_phase; // 16.16 progress through the current step
static uint16_t snake_age_ms;
static uint16_t snake_food_ms;
static bool     snake_dying;
static uint16_t snake_dying_ms;

static uint8_t snake_level[ANIM_LEDS]; // PWM duty
static uint8_t snake_food_level, snake_eaten_level;
static rgb_t   snake_body_rgb, snake_accent_rgb;

static void snake_link(uint8_t a, uint8_t b) {
    snake_adj[a][snake_degree[a]++] = b;
    snake_adj[b][snake_degree[b]++] = a;
}

static void snake_build_graph(void) {
    for (uint8_t a = 0; a < ANIM_LEDS; a++) {
        anim_key_t ka    = anim_keys[a];
        uint8_t    right = ANIM_NONE;
        for (uint8_t b = 0; b < ANIM_LEDS; b++) {
            anim_key_t kb = anim_keys[b];
            if (kb.row == ka.row && kb.x > ka.x && (right == ANIM_NONE || kb.x < anim_keys[right].x)) {
                right = b;
            } else if (kb.row == ka.row + 1) {
                int16_t overlap = MIN(ka.x + ka.w / 2, kb.x + kb.w / 2) - MAX(ka.x - ka.w / 2, kb.x - kb.w / 2);
                if (overlap >= SNAKE_MIN_OVERLAP) snake_link(a, b);
            }
        }
        if (right != ANIM_NONE) snake_link(a, right);
    }
}

/* Whether a cell is taken on the next step, the tail moves out of the way */
static inline bool snake_blocked(uint8_t led) {
    return snake_occupied[led] && led != snake_body[snake_len - 1];
}

/* Breadth first distances from a cell over free cells, returns the number reached */
static uint8_t snake_flood(uint8_t start, uint8_t *dist) {
    uint8_t queue[ANIM_LEDS];
    uint8_t read = 0, write = 0;

    memset(dist, ANIM_NONE, ANIM_LEDS);
    dist[start]    = 0;
    queue[write++] = start;
    while (read < write) {
        uint8_t cell = queue[read++];
        for (uint8_t n = 0; n < snake_degree[cell]; n++) {
            uint8_t next = snake_adj[cell][n];
            if (dist[next] == ANIM_NONE && !snake_blocked(next)) {
                dist[next]     = dist[cell] + 1;
                queue[write++] = next;
            }
        }
    }
    return write;
}

/* Prefers moves that can still reach the tail, then the shortest way to the food, then room */
static uint8_t snake_choose_move(void) {
    uint8_t to_food[ANIM_LEDS], from_move[ANIM_LEDS];
    uint8_t head = snake_body[0], tail = snake_body[snake_len - 1];
    uint8_t best       = ANIM_NONE;
    int32_t best_score = -1;

    snake_flood(snake_food, to_food);
    for (uint8_t n = 0; n < snake_degree[head]; n++) {
        uint8_t next = snake_adj[head][n];
        if (snake_blocked(next)) continue;

        uint8_t room  = snake_flood(next, from_move);
        int32_t score = room;
        if (to_food[next] != ANIM_NONE) score += (int32_t)(ANIM_LEDS + 1 - to_food[next]) << 8;
        if (from_move[tail] != ANIM_NONE && snake_hunger < SNAKE_MAX_HUNGER) score += (int32_t)1 << 16;

        if (score > best_score || (score == best_score && (rand() & 1))) {
            best_score = score;
            best       = next;
        }
    }
    return best;
}

/* Neighbor nearest the food in a straight line, looking only at the keys and not at the body */
static uint8_t snake_lunge_move(void) {
    uint8_t    head = snake_body[0], best = ANIM_NONE;
    uint32_t   best_dist = UINT32_MAX;
    anim_key_t food      = anim_keys[snake_food];

    for (uint8_t n = 0; n < snake_degree[head]; n++) {
        uint8_t next = snake_adj[head][n];
        if (next == snake_body[1]) continue;

        anim_key_t key  = anim_keys[next];
        int32_t    dx   = (int32_t)key.x - food.x;
        int32_t    dy   = ((int32_t)key.row - food.row) * ANIM_UNIT;
        uint32_t   dist = dx * dx + dy * dy;
        if (dist < best_dist) {
            best_dist = dist;
            best      = next;
        }
    }
    return best;
}

/* Random free cell, or ANIM_NONE when the board is full */
static uint8_t snake_pick_food(void) {
    if (snake_len == ANIM_LEDS) return ANIM_NONE;

    uint8_t skip = rand() % (ANIM_LEDS - snake_len);
    for (uint8_t i = 0; i < ANIM_LEDS; i++) {
        if (!snake_occupied[i] && skip-- == 0) return i;
    }
    return ANIM_NONE;
}

static void snake_reset(void) {
    memset(snake_occupied, 0, sizeof(snake_occupied));
    snake_len = SNAKE_START_LENGTH;
    for (uint8_t i = 0; i < snake_len; i++) {
        snake_body[i]                 = SNAKE_START_LED + i;
        snake_occupied[snake_body[i]] = true;
    }
    snake_ghost = snake_eaten = ANIM_NONE;
    snake_food                = snake_pick_food();
    snake_hunger              = 0;
    snake_phase               = 0;
    snake_age_ms              = 0;
    snake_food_ms             = 0;
    snake_dying               = false;
    snake_dying_ms            = 0;
}

static void snake_die(void) {
    snake_dying = true;
    snake_ghost = snake_eaten = ANIM_NONE;
    snake_phase               = 0xFFFF;
}

static void snake_step(void) {
    uint8_t next = ANIM_NONE;
    if (snake_food != ANIM_NONE) {
        next = rand() % 1024 < snake_len * SNAKE_LUNGE_PER_SEGMENT ? snake_lunge_move() : snake_choose_move();
    }
    if (next == ANIM_NONE || snake_blocked(next)) {
        snake_die();
        return;
    }

    bool eating = next == snake_food;
    if (eating) {
        snake_len++;
        snake_ghost  = ANIM_NONE;
        snake_hunger = 0;
    } else {
        snake_ghost                 = snake_body[snake_len - 1];
        snake_occupied[snake_ghost] = false;
        snake_hunger++;
    }

    memmove(snake_body + 1, snake_body, snake_len - 1);
    snake_body[0]        = next;
    snake_occupied[next] = true;

    snake_eaten      = eating ? next : ANIM_NONE;
    snake_eaten_from = snake_food_level;
    if (eating) {
        snake_food    = snake_pick_food();
        snake_food_ms = 0;
    }
}

/* Smoothstep over 0 to 255 */
static uint8_t snake_ease(uint8_t t) {
    return (uint32_t)t * t * (765 - 2 * t) / 65025;
}

/* Duty falls linearly from full behind the head to zero one step past the tail, positions are 8.8 */
static uint8_t snake_segment_duty(uint16_t behind, uint16_t length) {
    return behind >= length ? 0 : (uint32_t)(length - behind) * 255 / length;
}

static inline void snake_raise(uint8_t led, uint8_t level) {
    if (level > snake_level[led]) snake_level[led] = level;
}

/* Overall level of the scene, fading a new game in and a finished one out */
static uint8_t snake_scene_level(void) {
    if (!snake_dying) return snake_age_ms >= SNAKE_FADE_IN_MS ? 255 : snake_age_ms * 255 / SNAKE_FADE_IN_MS;
    if (snake_dying_ms <= SNAKE_DEATH_HOLD_MS) return 255;
    return 255 - (uint32_t)(snake_dying_ms - SNAKE_DEATH_HOLD_MS) * 255 / SNAKE_DEATH_FADE_MS;
}

static void snake_compute_levels(void) {
    uint8_t phase = snake_phase >> 8;
    uint8_t eased = snake_ease(phase);
    uint8_t scene = snake_scene_level();

    // The fade stretches over the step that grows the snake, so eating does not shift the body
    uint16_t length = snake_eaten != ANIM_NONE ? ((snake_len - 1) << 8) + phase : snake_len << 8;

    memset(snake_level, 0, sizeof(snake_level));
    snake_raise(snake_body[0], anim_gamma(eased));
    for (uint8_t i = 1; i < snake_len; i++) {
        snake_raise(snake_body[i], snake_segment_duty(((i - 1) << 8) + phase, length));
    }
    if (snake_ghost != ANIM_NONE) {
        snake_raise(snake_ghost, snake_segment_duty(((snake_len - 1) << 8) + phase, length));
    }
    for (uint8_t i = 0; i < ANIM_LEDS; i++) {
        snake_level[i] = scale8(snake_level[i], anim_gamma(scene));
    }

    // Food breathes around a bright level once faded in
    uint8_t food_in  = snake_food_ms >= SNAKE_FOOD_IN_MS ? 255 : snake_food_ms * 255 / SNAKE_FOOD_IN_MS;
    uint8_t breath   = 180 + scale8(sin8((uint8_t)(g_rgb_timer >> 3)), 75);
    snake_food_level = scale8(scale8(breath, snake_ease(food_in)), scene);

    snake_eaten_level = scale8(snake_eaten_from, 255 - eased);
}

static void snake_compute_colors(void) {
    hsv_t hsv      = rgb_matrix_config.hsv;
    snake_body_rgb = rgb_matrix_hsv_to_rgb(hsv);
    hsv.h += 128;
    snake_accent_rgb = rgb_matrix_hsv_to_rgb(hsv);

    if (snake_dying) {
        uint8_t tint     = snake_dying_ms >= SNAKE_DEATH_TINT_MS ? 255 : snake_dying_ms * 255 / SNAKE_DEATH_TINT_MS;
        snake_body_rgb.r = lerp8by8(snake_body_rgb.r, snake_accent_rgb.r, tint);
        snake_body_rgb.g = lerp8by8(snake_body_rgb.g, snake_accent_rgb.g, tint);
        snake_body_rgb.b = lerp8by8(snake_body_rgb.b, snake_accent_rgb.b, tint);
    }
}

static void snake_update(effect_params_t *params) {
    if (params->init) {
        if (!snake_graph_ready) {
            snake_build_graph();
            snake_graph_ready = true;
        }
        srand(g_rgb_timer);
        snake_last_ms = g_rgb_timer;
        snake_reset();
    }

    uint32_t dt   = anim_frame_dt(&snake_last_ms);
    snake_age_ms  = MIN(snake_age_ms + dt, SNAKE_FADE_IN_MS);
    snake_food_ms = MIN(snake_food_ms + dt, SNAKE_FOOD_IN_MS);

    if (snake_dying) {
        snake_dying_ms += dt;
        if (snake_dying_ms >= SNAKE_DEATH_HOLD_MS + SNAKE_DEATH_FADE_MS) snake_reset();
    } else {
        uint32_t step_ms = SNAKE_STEP_SLOW_MS - rgb_matrix_config.speed * (SNAKE_STEP_SLOW_MS - SNAKE_STEP_FAST_MS) / 255;
        snake_phase += dt * 65536 / step_ms;
        if (snake_phase > 0xFFFF) {
            snake_phase &= 0xFFFF;
            snake_step();
        }
    }

    snake_compute_levels();
    snake_compute_colors();
}

static bool AUTO_SNAKE(effect_params_t *params) {
    RGB_MATRIX_USE_LIMITS(led_min, led_max);

    if (params->iter == 0) snake_update(params);

    for (uint8_t i = led_min; i < led_max; i++) {
        RGB_MATRIX_TEST_LED_FLAGS();

        uint8_t body = snake_level[i];
        uint8_t food = 0;
        if (i == snake_food) food = anim_gamma(snake_food_level);
        if (i == snake_eaten) food = qadd8(food, anim_gamma(snake_eaten_level));

        rgb_matrix_set_color(i, qadd8(scale8(snake_body_rgb.r, body), scale8(snake_accent_rgb.r, food)), qadd8(scale8(snake_body_rgb.g, body), scale8(snake_accent_rgb.g, food)), qadd8(scale8(snake_body_rgb.b, body), scale8(snake_accent_rgb.b, food)));
    }

    return rgb_matrix_check_finished_leds(led_max);
}
