/* Physical key geometry and helpers shared by the custom effects */

#define ANIM_LEDS RGB_MATRIX_LED_COUNT
#define ANIM_NONE UINT8_MAX

/* Longest time step, so a stall cannot make an effect jump */
#define ANIM_MAX_DT_MS 100

/* Lengths are in 1/16 of a key unit, which puts every key center on an integer */
#define ANIM_UNIT 16
#define ANIM_KEY_Y(row) ((row) * ANIM_UNIT + ANIM_UNIT / 2)

typedef struct {
    uint8_t x; // center
    uint8_t w; // width
    uint8_t row;
} anim_key_t;

/* Key centers and widths from LAYOUT_all, in LED order. g_led_config.point has no key widths. */
// clang-format off
static const anim_key_t anim_keys[] = {
    {224, 32, 0}, //  0 [0,13]
    {200, 16, 0}, //  1 [0,12]
    {184, 16, 0}, //  2 [0,11]
    {168, 16, 0}, //  3 [0,10]
    {152, 16, 0}, //  4 [0,9]
    {136, 16, 0}, //  5 [0,8]
    {120, 16, 0}, //  6 [0,7]
    {104, 16, 0}, //  7 [0,6]
    { 88, 16, 0}, //  8 [0,5]
    { 72, 16, 0}, //  9 [0,4]
    { 56, 16, 0}, // 10 [0,3]
    { 40, 16, 0}, // 11 [0,2]
    { 24, 16, 0}, // 12 [0,1]
    {  8, 16, 0}, // 13 [0,0]
    { 12, 24, 1}, // 14 [1,0]
    { 32, 16, 1}, // 15 [1,1]
    { 48, 16, 1}, // 16 [1,2]
    { 64, 16, 1}, // 17 [1,3]
    { 80, 16, 1}, // 18 [1,4]
    { 96, 16, 1}, // 19 [1,5]
    {112, 16, 1}, // 20 [1,6]
    {128, 16, 1}, // 21 [1,7]
    {144, 16, 1}, // 22 [1,8]
    {160, 16, 1}, // 23 [1,9]
    {176, 16, 1}, // 24 [1,10]
    {192, 16, 1}, // 25 [1,11]
    {208, 16, 1}, // 26 [1,12]
    {228, 24, 1}, // 27 [1,13]
    {248, 16, 1}, // 28 [1,15]
    {248, 16, 2}, // 29 [2,15]
    {222, 36, 2}, // 30 [2,13]
    {196, 16, 2}, // 31 [2,11]
    {180, 16, 2}, // 32 [2,10]
    {164, 16, 2}, // 33 [2,9]
    {148, 16, 2}, // 34 [2,8]
    {132, 16, 2}, // 35 [2,7]
    {116, 16, 2}, // 36 [2,6]
    {100, 16, 2}, // 37 [2,5]
    { 84, 16, 2}, // 38 [2,4]
    { 68, 16, 2}, // 39 [2,3]
    { 52, 16, 2}, // 40 [2,2]
    { 36, 16, 2}, // 41 [2,1]
    { 14, 28, 2}, // 42 [2,0]
    { 10, 20, 3}, // 43 [3,0]
    { 28, 16, 3}, // 44 [3,11]
    { 44, 16, 3}, // 45 [3,1]
    { 60, 16, 3}, // 46 [3,2]
    { 76, 16, 3}, // 47 [3,3]
    { 92, 16, 3}, // 48 [3,4]
    {108, 16, 3}, // 49 [3,5]
    {124, 16, 3}, // 50 [3,6]
    {140, 16, 3}, // 51 [3,7]
    {156, 16, 3}, // 52 [3,8]
    {172, 16, 3}, // 53 [3,9]
    {188, 16, 3}, // 54 [3,10]
    {210, 28, 3}, // 55 [3,13]
    {232, 16, 3}, // 56 [3,14]
    {248, 16, 3}, // 57 [3,15]
    {248, 16, 4}, // 58 [4,15]
    {232, 16, 4}, // 59 [4,14]
    {216, 16, 4}, // 60 [4,13]
    {190, 20, 4}, // 61 [4,11]
    {170, 20, 4}, // 62 [4,10]
    {138, 44, 4}, // 63 [4,7]
    {106, 20, 4}, // 64 [4,5]
    { 78, 36, 4}, // 65 [4,3]
    { 50, 20, 4}, // 66 [4,2]
    { 30, 20, 4}, // 67 [4,1]
    { 10, 20, 4}, // 68 [4,0]
};
// clang-format on

_Static_assert(sizeof(anim_keys) / sizeof(anim_keys[0]) == ANIM_LEDS, "anim_keys must have one entry per LED");

/* Time since the previous frame, called once per frame */
static uint32_t anim_frame_dt(uint32_t *last_ms) {
    uint32_t dt = g_rgb_timer - *last_ms;
    *last_ms    = g_rgb_timer;
    return MIN(dt, ANIM_MAX_DT_MS);
}

/* Perceptual level to PWM duty, the eye sees LED output as roughly the square root */
static inline uint8_t anim_gamma(uint8_t level) {
    return scale8(level, level);
}

/* Set an LED to a color scaled by a perceptual level */
static inline void anim_set_scaled(uint8_t led, rgb_t rgb, uint8_t level) {
    uint8_t duty = anim_gamma(level);
    rgb_matrix_set_color(led, scale8(rgb.r, duty), scale8(rgb.g, duty), scale8(rgb.b, duty));
}
