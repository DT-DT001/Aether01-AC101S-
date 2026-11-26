#include "app_arcade.h"

#include <math.h>
#include <string.h>

static uint32_t random_next(uint32_t *state)
{
    uint32_t value = *state ? *state : 0xA341316Cu;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void add_score(uint32_t *score, uint32_t points)
{
    *score = points > UINT32_MAX - *score ? UINT32_MAX : *score + points;
}

static bool valid_direction(arcade_direction_t direction)
{
    return direction >= ARCADE_UP && direction <= ARCADE_LEFT;
}

static int neighbor(int cell, int columns, int rows, arcade_direction_t direction)
{
    static const int dx[] = {0, 0, 1, 0, -1};
    static const int dy[] = {0, -1, 0, 1, 0};
    const int x = cell % columns + dx[direction];
    const int y = cell / columns + dy[direction];
    return x >= 0 && x < columns && y >= 0 && y < rows ? y * columns + x : -1;
}

static void tiles_spawn(arcade_tiles_t *game)
{
    unsigned open[16], count = 0;
    for (unsigned i = 0; i < 16; ++i) {
        if (!game->cells[i]) open[count++] = i;
    }
    if (count) {
        const unsigned index = open[random_next(&game->random) % count];
        game->cells[index] = random_next(&game->random) % 10 == 0 ? 2 : 1;
    }
}

static bool tiles_movable(const arcade_tiles_t *game)
{
    for (unsigned i = 0; i < 16; ++i) {
        const uint8_t tile = game->cells[i];
        if (!tile) return true;
        if (tile >= 31) continue;
        if (i % 4 != 3 && game->cells[i + 1] == tile) return true;
        if (i < 12 && game->cells[i + 4] == tile) return true;
    }
    return false;
}

static unsigned tiles_index(arcade_direction_t direction, unsigned line, unsigned position)
{
    switch (direction) {
    case ARCADE_UP: return position * 4 + line;
    case ARCADE_RIGHT: return line * 4 + 3 - position;
    case ARCADE_DOWN: return (3 - position) * 4 + line;
    default: return line * 4 + position;
    }
}

void arcade_tiles_init(arcade_tiles_t *game, uint32_t seed)
{
    *game = (arcade_tiles_t){.random = seed};
    tiles_spawn(game);
    tiles_spawn(game);
}

bool arcade_tiles_move(arcade_tiles_t *game, arcade_direction_t direction)
{
    if (!valid_direction(direction) || game->over) return false;
    bool moved = false;
    for (unsigned line = 0; line < 4; ++line) {
        uint8_t merged[4] = {0};
        unsigned written = 0;
        bool can_merge = false;
        for (unsigned pos = 0; pos < 4; ++pos) {
            const uint8_t value = game->cells[tiles_index(direction, line, pos)];
            if (!value) continue;
            if (can_merge && value < 31 && value == merged[written - 1]) {
                const unsigned combined = ++merged[written - 1];
                add_score(&game->score, UINT32_C(1) << combined);
                if (combined == 11) game->won = true;
                can_merge = false;
            } else {
                merged[written++] = value;
                can_merge = true;
            }
        }
        for (unsigned pos = 0; pos < 4; ++pos) {
            uint8_t *cell = &game->cells[tiles_index(direction, line, pos)];
            if (*cell != merged[pos]) moved = true;
            *cell = merged[pos];
        }
    }
    if (moved) tiles_spawn(game);
    game->over = !tiles_movable(game);
    return moved;
}

static const char levels[][ARCADE_CRATE_SIDE][ARCADE_CRATE_SIDE + 1] = {
    {
        "########", "##     #", "# .    #", "#  $$. #",
        "#   #  #", "#  @   #", "#      #", "########",
    },
    {
        "########", "#      #", "# .  . #", "# $  $ #",
        "#  @   #", "#      #", "#      #", "########",
    },
    {
        "########", "##     #", "# .    #", "#  $$ .#",
        "#   #  #", "#  @   #", "#      #", "########",
    },
    {
        "########", "# .    #", "# #  # #", "#   $$ #",
        "# #$ # #", "#@    .#", "#  .   #", "########",
    },
    {
        "########", "#.  #  #", "# $ # .#", "#      #",
        "# #$$# #", "# @   .#", "#    # #", "########",
    },
    {
        "########", "#  ..  #", "#  $   #", "# ###$ #",
        "#  @ $ #", "#  # . #", "#      #", "########",
    },
};

unsigned arcade_crates_levels(void)
{
    return sizeof(levels) / sizeof(levels[0]);
}

void arcade_crates_init(arcade_crates_t *game, unsigned level)
{
    memset(game, 0, sizeof(*game));
    const char (*rows)[ARCADE_CRATE_SIDE + 1] = levels[level % arcade_crates_levels()];
    for (unsigned y = 0; y < ARCADE_CRATE_SIDE; ++y) {
        for (unsigned x = 0; x < ARCADE_CRATE_SIDE; ++x) {
            const unsigned cell = y * ARCADE_CRATE_SIDE + x;
            const uint64_t bit = UINT64_C(1) << cell;
            switch (rows[y][x]) {
            case '#': game->walls |= bit; break;
            case '.': game->goals |= bit; break;
            case '$': game->turn.boxes |= bit; break;
            case '@': game->turn.player = cell; break;
            default: break;
            }
        }
    }
}

bool arcade_crates_move(arcade_crates_t *game, arcade_direction_t direction)
{
    if (!valid_direction(direction) || arcade_crates_solved(game)) return false;
    const int cell = neighbor(game->turn.player, ARCADE_CRATE_SIDE, ARCADE_CRATE_SIDE, direction);
    if (cell < 0) return false;
    const uint64_t bit = UINT64_C(1) << cell;
    if (game->walls & bit) return false;
    uint64_t boxes = game->turn.boxes;
    if (boxes & bit) {
        const int push = neighbor(cell, ARCADE_CRATE_SIDE, ARCADE_CRATE_SIDE, direction);
        if (push < 0) return false;
        const uint64_t pushed_bit = UINT64_C(1) << push;
        if ((game->walls | boxes) & pushed_bit) return false;
        boxes = (boxes ^ bit) | pushed_bit;
    }
    game->history[game->head] = game->turn;
    game->head = (game->head + 1) % ARCADE_CRATE_UNDO;
    if (game->count < ARCADE_CRATE_UNDO) ++game->count;
    game->turn.player = cell;
    game->turn.boxes = boxes;
    add_score(&game->turn.moves, 1);
    return true;
}

bool arcade_crates_undo(arcade_crates_t *game)
{
    if (!game->count) return false;
    game->head = (game->head + ARCADE_CRATE_UNDO - 1) % ARCADE_CRATE_UNDO;
    game->turn = game->history[game->head];
    --game->count;
    return true;
}

bool arcade_crates_solved(const arcade_crates_t *game)
{
    return game->turn.boxes == game->goals;
}

uint16_t arcade_snake_cell(const arcade_snake_t *game, unsigned segment)
{
    return game->body[(game->head + segment) % ARCADE_SNAKE_CAPACITY];
}

static void snake_food(arcade_snake_t *game)
{
    unsigned open = ARCADE_SNAKE_CAPACITY - game->length;
    game->food = ARCADE_SNAKE_NO_CELL;
    if (!open) {
        game->won = true;
        game->over = true;
        return;
    }
    unsigned choice = random_next(&game->random) % open;
    for (unsigned i = 0; i < ARCADE_SNAKE_CAPACITY; ++i) {
        if (!game->occupied[i] && choice-- == 0) {
            game->food = i;
            return;
        }
    }
}

void arcade_snake_init(arcade_snake_t *game, uint32_t seed)
{
    memset(game, 0, sizeof(*game));
    game->random = seed;
    game->length = 3;
    for (unsigned i = 0; i < game->length; ++i) {
        game->body[i] = (ARCADE_SNAKE_ROWS / 2) * ARCADE_SNAKE_COLUMNS + ARCADE_SNAKE_COLUMNS / 2 - i;
        game->occupied[game->body[i]] = true;
    }
    snake_food(game);
}

bool arcade_snake_direction(arcade_snake_t *game, arcade_direction_t direction)
{
    if (!valid_direction(direction) || game->over || game->next != game->direction) return false;
    if (game->direction == direction || (game->direction == ARCADE_NONE && direction == ARCADE_LEFT)) return false;
    if (game->direction != ARCADE_NONE && (game->direction + 1) % 4 + 1 == direction) return false;
    game->next = direction;
    return true;
}

bool arcade_snake_step(arcade_snake_t *game)
{
    if (game->over || !valid_direction(game->next)) return false;
    game->direction = game->next;
    const int next = neighbor(arcade_snake_cell(game, 0), ARCADE_SNAKE_COLUMNS,
                              ARCADE_SNAKE_ROWS, game->direction);
    const uint16_t tail = arcade_snake_cell(game, game->length - 1);
    const bool grow = next == game->food;
    if (next < 0 || (game->occupied[next] && (next != tail || grow))) {
        game->over = true;
        return false;
    }
    if (grow) {
        ++game->length;
    } else {
        game->occupied[tail] = false;
    }
    game->head = (game->head + ARCADE_SNAKE_CAPACITY - 1) % ARCADE_SNAKE_CAPACITY;
    game->body[game->head] = next;
    game->occupied[next] = true;
    if (grow) snake_food(game);
    return true;
}

uint32_t arcade_snake_interval(const arcade_snake_t *game)
{
    const unsigned eaten = game->length - 3;
    return eaten < 22 ? 232 - eaten * 7 : 78;
}

arcade_direction_t arcade_tilt_update(arcade_tilt_t *tilt, float x, float y)
{
    enum { RELEASE_DEGREES = 7, TRIGGER_DEGREES = 10,
           AXIS_MARGIN_DEGREES = 3, CONFIRM_SAMPLES = 2 };
    const float abs_x = fabsf(x), abs_y = fabsf(y);
    if (!isfinite(abs_x) || !isfinite(abs_y)) {
        tilt->candidate = ARCADE_NONE;
        tilt->samples = 0;
        tilt->neutral_samples = 0;
        return ARCADE_NONE;
    }
    if (abs_x <= RELEASE_DEGREES && abs_y <= RELEASE_DEGREES) {
        tilt->candidate = ARCADE_NONE;
        tilt->samples = 0;
        if (tilt->held != ARCADE_NONE && ++tilt->neutral_samples >= CONFIRM_SAMPLES) {
            *tilt = (arcade_tilt_t){0};
        }
        return ARCADE_NONE;
    }
    tilt->neutral_samples = 0;
    arcade_direction_t direction = ARCADE_NONE;
    if (abs_x >= TRIGGER_DEGREES && abs_x - abs_y >= AXIS_MARGIN_DEGREES) {
        direction = x > 0 ? ARCADE_RIGHT : ARCADE_LEFT;
    } else if (abs_y >= TRIGGER_DEGREES && abs_y - abs_x >= AXIS_MARGIN_DEGREES) {
        direction = y > 0 ? ARCADE_DOWN : ARCADE_UP;
    }
    if (direction != tilt->candidate) {
        tilt->candidate = direction;
        tilt->samples = 0;
    }
    if (direction == ARCADE_NONE || direction == tilt->held) return ARCADE_NONE;
    if (tilt->samples < CONFIRM_SAMPLES) ++tilt->samples;
    if (tilt->samples < CONFIRM_SAMPLES) return ARCADE_NONE;
    tilt->held = direction;
    return direction;
}

static float vector_length_squared(const float value[3])
{
    return value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
}

static void accel_lock(arcade_accel_t *state, uint32_t stamp, bool hold_reference)
{
    state->armed = state->steady = false;
    state->lock_ms = stamp;
    state->candidate = ARCADE_NONE;
    state->hold_reference = hold_reference;
    state->grip_turn = 0.0f;
}

bool arcade_accel_update(arcade_accel_t *state, const float accel[3], const float gyro[3],
                         uint32_t stamp, arcade_direction_t *move)
{
    enum { MAX_SAMPLE_GAP_MS = 85, READY_MS = 140, REARM_MS = 100, LOCK_MS = 235 };
    enum { PULSE_CONFIRM_MS = 8, PULSE_WINDOW_MS = 90, MAX_PULSE_GAP_MS = 40 };
    const float max_rotation_squared = 42.0f * 42.0f;
    const float accel_squared = vector_length_squared(accel);
    *move = ARCADE_NONE;
    if (!isfinite(accel_squared) || accel_squared > 80.0f * 80.0f) {
        *state = (arcade_accel_t){0};
        return false;
    }
    uint32_t elapsed = 0;
    if (state->initialized) {
        elapsed = stamp - state->stamp;
        if (!elapsed || elapsed > UINT32_MAX / 2U) return true;
        if (elapsed > MAX_SAMPLE_GAP_MS) {
            *state = (arcade_accel_t){0};
            elapsed = 0;
        }
    }
    const float gyro_squared = gyro ? vector_length_squared(gyro) : INFINITY;
    const bool have_gyro = gyro_squared < 2200.0f * 2200.0f;
    float difference[3], step[3];
    for (unsigned i = 0; i < 3; ++i) {
        difference[i] = accel[i] - state->gravity[i];
        step[i] = accel[i] - state->previous[i];
        state->previous[i] = accel[i];
    }
    const float difference_squared = vector_length_squared(difference);
    const float step_squared = vector_length_squared(step);
    const float gravity_squared = vector_length_squared(state->gravity);
    const bool valid_gravity = accel_squared > 8.0f * 8.0f && accel_squared < 11.6f * 11.6f;
    const bool stable_accel = valid_gravity && step_squared < .24f;
    if (!state->ready && !state->steady && have_gyro && gyro_squared < 38.0f * 38.0f && stable_accel)
        memcpy(state->gyro_zero, gyro, sizeof(state->gyro_zero));
    float angular[3] = {0};
    if (have_gyro)
        for (unsigned i = 0; i < 3; ++i) angular[i] = gyro[i] - state->gyro_zero[i];
    const float angular_squared = vector_length_squared(angular);
    const bool stable = stable_accel && angular_squared < 19.0f * 19.0f;
    state->stamp = stamp;
    state->initialized = true;
    if (!state->armed) {
        if (state->hold_reference && angular_squared > 4.0f * 4.0f) {
            const float heading = angular[0] * state->gravity[0] + angular[1] * state->gravity[1] +
                                  angular[2] * state->gravity[2];
            const float transverse_squared = angular_squared - heading * heading / gravity_squared;
            if (transverse_squared > 4.0f * 4.0f)
                state->grip_turn += sqrtf(transverse_squared) * ((float)elapsed * .001f);
            if (state->grip_turn >= 4.0f) state->hold_reference = false;
        }
        /* After a translational pulse, a constant force in the same direction
         * is not a new rest position. Wait for the actual return to rest. */
        if (!stable || (state->hold_reference && difference_squared > .85f)) {
            state->steady = false;
            return true;
        }
        /* A quiet, repeatable gravity vector establishes the current grip. */
        if (!state->steady || (!state->hold_reference && difference_squared > .50f)) {
            state->steady = true;
            state->steady_ms = stamp;
            if (!state->hold_reference) memcpy(state->gravity, accel, sizeof(state->gravity));
        }
        if (stamp - state->steady_ms >= (state->ready ? REARM_MS : READY_MS) &&
            (!state->ready || stamp - state->lock_ms >= LOCK_MS)) {
            state->ready = state->armed = true;
        }
        return true;
    }
    if (accel_squared < 4.0f * 4.0f || angular_squared > max_rotation_squared) {
        accel_lock(state, stamp, angular_squared <= max_rotation_squared);
        return true;
    }
    const float gravity_difference = fabsf(accel_squared - gravity_squared);
    const float x = fabsf(difference[0]), y = fabsf(difference[1]);
    const bool horizontal = x > y;
    const float primary = horizontal ? x : y;
    const bool planar = primary >= 1.45f * (horizontal ? y : x) &&
                        fabsf(difference[2]) <= .68f * primary + .35f;
    /* AC101S native +X is screen right, +Y is screen up for translation. */
    const arcade_direction_t direction = horizontal ? (difference[0] > 0 ? ARCADE_RIGHT : ARCADE_LEFT) :
                                                      (difference[1] > 0 ? ARCADE_UP : ARCADE_DOWN);
    if (state->candidate != ARCADE_NONE) {
        const uint32_t duration = stamp - state->candidate_ms;
        if (!planar || primary < .55f || direction != state->candidate ||
            elapsed > MAX_PULSE_GAP_MS || duration > PULSE_WINDOW_MS) {
            /* Even an aborted or ambiguous pulse may have a strong rebound. */
            accel_lock(state, stamp, true);
            return true;
        }
        if (primary > state->candidate_peak) state->candidate_peak = primary;
        state->candidate_impulse += (primary + state->candidate_previous) * (float)elapsed * .0005f;
        state->candidate_previous = primary;
        if (duration >= PULSE_CONFIRM_MS && state->candidate_peak >= 1.20f && state->candidate_impulse >= .012f) {
            *move = direction;
            accel_lock(state, stamp, true);
        }
        return true;
    }
    if (primary >= .75f) {
        /* An inclined grip can preserve the norm even during a translation.
         * Compare onset to the largest gravity change explained by rotation. */
        const float angular_limit = have_gyro ? sqrtf(angular_squared) * .00027f * (float)elapsed : 0.0f;
        const float abs_step = fabsf(step[horizontal ? 0 : 1]);
        const bool abrupt = have_gyro && abs_step > .11f + angular_limit;
        const bool norm_changed = gravity_difference >= (have_gyro ? 1.3f : 1.0f);
        if (!planar) {
            accel_lock(state, stamp, true);
        } else if (abrupt || (norm_changed && angular_squared < 19.0f * 19.0f)) {
            state->candidate = direction;
            state->candidate_ms = stamp;
            state->candidate_previous = state->candidate_peak = primary;
            state->candidate_impulse = 0.0f;
        } else {
            accel_lock(state, stamp, false);
        }
    } else if (fabsf(difference[2]) > 2.2f) {
        accel_lock(state, stamp, true);
    } else if (stable && difference_squared < 1.4f) {
        /* Keep the response consistent when fresh frames arrive at 10 or 25 ms. */
        const float interval = (float)elapsed;
        const float gravity_gain = interval / (160.0f + interval);
        const float gyro_gain = interval / (1400.0f + interval);
        for (unsigned i = 0; i < 3; ++i) {
            state->gravity[i] += difference[i] * gravity_gain;
            if (have_gyro) state->gyro_zero[i] += angular[i] * gyro_gain;
        }
    }
    return true;
}

enum { JUMP_CHARGE_MS = 850, JUMP_FLIGHT_MS = 490, JUMP_PAN_MS = 210, JUMP_FALL_MS = 230 };

static uint16_t jump_progress(uint32_t now, uint32_t start, uint32_t duration)
{
    const uint32_t elapsed = now - start;
    return elapsed >= duration ? 1000 : elapsed * 1000 / duration;
}

uint16_t arcade_jump_distance(uint16_t power)
{
    if (power > 1000) power = 1000;
    return 38 + power * 196 / 1000;
}

/* Bit n is the cell at column n % 4, row n / 4 of the piece's box. */
uint16_t arcade_fall_shape(unsigned piece, unsigned rotation)
{
    static const uint16_t shapes[ARCADE_FALL_PIECES][4] = {
        {0x00F0, 0x4444, 0x0F00, 0x2222},
        {0x0066, 0x0066, 0x0066, 0x0066},
        {0x0072, 0x0262, 0x0270, 0x0232},
        {0x0036, 0x0462, 0x0360, 0x0231},
        {0x0063, 0x0264, 0x0630, 0x0132},
        {0x0071, 0x0226, 0x0470, 0x0322},
        {0x0074, 0x0622, 0x0170, 0x0223},
    };
    return shapes[piece][rotation & 3U];
}

static bool fall_fits(const arcade_fall_t *game, int x, int y, unsigned rotation)
{
    for (unsigned cells = arcade_fall_shape(game->piece, rotation); cells; cells &= cells - 1U) {
        const unsigned bit = __builtin_ctz(cells);
        const int col = x + (int)(bit % 4U), row = y + (int)(bit / 4U);
        if (col < 0 || col >= ARCADE_FALL_COLUMNS || row >= ARCADE_FALL_ROWS ||
            (row >= 0 && game->cells[row][col])) return false;
    }
    return true;
}

static uint8_t fall_draw_piece(arcade_fall_t *game)
{
    if (game->bag_pos == ARCADE_FALL_PIECES) {
        for (unsigned i = 0; i < ARCADE_FALL_PIECES; ++i) game->bag[i] = i;
        for (unsigned i = ARCADE_FALL_PIECES - 1; i > 0; --i) {
            const unsigned j = random_next(&game->random) % (i + 1U);
            const uint8_t piece = game->bag[i];
            game->bag[i] = game->bag[j];
            game->bag[j] = piece;
        }
        game->bag_pos = 0;
    }
    return game->bag[game->bag_pos++];
}

static void fall_spawn(arcade_fall_t *game)
{
    game->piece = game->next;
    game->next = fall_draw_piece(game);
    game->rotation = 0;
    game->x = (ARCADE_FALL_COLUMNS - 4) / 2;
    game->y = 0;
    game->over = !fall_fits(game, game->x, game->y, 0);
}

void arcade_fall_init(arcade_fall_t *game, uint32_t seed)
{
    memset(game, 0, sizeof(*game));
    game->random = seed;
    game->bag_pos = ARCADE_FALL_PIECES;
    game->next = fall_draw_piece(game);
    fall_spawn(game);
}

bool arcade_fall_move(arcade_fall_t *game, int dx, int dy)
{
    if (game->over || !fall_fits(game, game->x + dx, game->y + dy, game->rotation)) return false;
    game->x += dx;
    game->y += dy;
    return true;
}

bool arcade_fall_rotate(arcade_fall_t *game)
{
    if (game->over) return false;
    const unsigned rotation = (game->rotation + 1U) & 3U;
    static const int8_t kicks[][2] = {{0, 0}, {-1, 0}, {1, 0}, {-2, 0}, {2, 0},
                                     {0, -1}, {-1, -1}, {1, -1}, {0, -2}};
    for (unsigned i = 0; i < sizeof(kicks) / sizeof(kicks[0]); ++i) {
        const int x = game->x + kicks[i][0], y = game->y + kicks[i][1];
        if (!fall_fits(game, x, y, rotation)) continue;
        game->x = x;
        game->y = y;
        game->rotation = rotation;
        return true;
    }
    return false;
}

int arcade_fall_landing(const arcade_fall_t *game)
{
    int y = game->y;
    while (fall_fits(game, game->x, y + 1, game->rotation)) ++y;
    return y;
}

unsigned arcade_fall_level(const arcade_fall_t *game)
{
    const unsigned level = game->lines / 10U;
    return 1U + (level < 13U ? level : 13U);
}

uint32_t arcade_fall_interval(const arcade_fall_t *game)
{
    return 800U - (arcade_fall_level(game) - 1U) * 52U;
}

static void fall_lock(arcade_fall_t *game)
{
    const unsigned shape = arcade_fall_shape(game->piece, game->rotation);
    const int first_row = game->y + (int)(__builtin_ctz(shape) / 4U);
    if (first_row < 0) {
        game->over = true;
        return;
    }
    for (unsigned cells = shape; cells; cells &= cells - 1U) {
        const unsigned bit = __builtin_ctz(cells);
        game->cells[game->y + (int)(bit / 4U)][game->x + (int)(bit % 4U)] = game->piece + 1U;
    }
    int write = ARCADE_FALL_ROWS - 1;
    for (int row = write; row >= 0; --row) {
        unsigned col = 0;
        while (col < ARCADE_FALL_COLUMNS && game->cells[row][col]) ++col;
        if (col == ARCADE_FALL_COLUMNS) continue;
        if (row != write) memcpy(game->cells[write], game->cells[row], sizeof(game->cells[row]));
        --write;
    }
    const unsigned cleared = (unsigned)(write + 1);
    memset(game->cells, 0, cleared * sizeof(game->cells[0]));
    static const uint16_t line_points[] = {0, 100, 300, 500, 800};
    add_score(&game->score, line_points[cleared > 4U ? 4U : cleared] * arcade_fall_level(game));
    add_score(&game->lines, cleared);
    fall_spawn(game);
}

void arcade_fall_step(arcade_fall_t *game)
{
    if (!game->over && !arcade_fall_move(game, 0, 1)) fall_lock(game);
}

void arcade_fall_drop(arcade_fall_t *game)
{
    if (game->over) return;
    const int landing = arcade_fall_landing(game);
    add_score(&game->score, 2U * (unsigned)(landing - game->y));
    game->y = landing;
    fall_lock(game);
}

uint8_t arcade_roll_die(uint32_t *random)
{
    /* Xorshift visits all nonzero words. Discard the incomplete final group. */
    uint32_t draw;
    do { draw = random_next(random); } while (draw > UINT32_MAX - UINT32_MAX % 6U);
    return 1U + (draw - 1U) % 6U;
}

void arcade_jump_init(arcade_jump_t *game, uint32_t seed)
{
    *game = (arcade_jump_t){
        .random = seed,
        .source = {.radius = 33},
        .target = {.gap = 106, .radius = 26, .height = 16},
    };
}

void arcade_jump_press(arcade_jump_t *game, uint32_t now)
{
    if (game->phase != ARCADE_JUMP_READY) return;
    game->phase = ARCADE_JUMP_CHARGING;
    game->phase_ms = now;
    game->perfect = false;
}

void arcade_jump_release(arcade_jump_t *game, uint32_t now)
{
    if (game->phase != ARCADE_JUMP_CHARGING) return;
    game->distance = arcade_jump_distance(jump_progress(now, game->phase_ms, JUMP_CHARGE_MS));
    game->phase = ARCADE_JUMP_FLIGHT;
    game->phase_ms = now;
}

void arcade_jump_cancel(arcade_jump_t *game)
{
    if (game->phase == ARCADE_JUMP_CHARGING) game->phase = ARCADE_JUMP_READY;
}

static arcade_platform_t jump_platform(arcade_jump_t *game)
{
    const uint32_t draw = random_next(&game->random);
    const uint16_t range = game->score < 12 ? 72 : 109;
    const uint8_t radius = game->score < 8 ? 25 : game->score < 20 ? 21 : 17;
    return (arcade_platform_t){
        .gap = 88 + draw % range,
        .radius = radius + (draw >> 9) % 4,
        .height = ((draw >> 15) % 5) * 9,
    };
}

void arcade_jump_step(arcade_jump_t *game, uint32_t now)
{
    const uint32_t elapsed = now - game->phase_ms;
    if (game->phase == ARCADE_JUMP_FLIGHT && elapsed >= JUMP_FLIGHT_MS) {
        const int offset = (int)game->distance - game->target.gap;
        const unsigned error = offset < 0 ? (unsigned)-offset : (unsigned)offset;
        game->phase_ms += JUMP_FLIGHT_MS;
        if (error <= game->target.radius) {
            game->perfect = error <= 7;
            add_score(&game->score, game->perfect ? 2 : 1);
            game->upcoming = jump_platform(game);
            game->phase = ARCADE_JUMP_PAN;
        } else {
            game->phase = ARCADE_JUMP_FALL;
        }
    }
    if (game->phase == ARCADE_JUMP_PAN && now - game->phase_ms >= JUMP_PAN_MS) {
        game->source = game->target;
        game->target = game->upcoming;
        game->phase = ARCADE_JUMP_READY;
    }
    if (game->phase == ARCADE_JUMP_FALL && now - game->phase_ms >= JUMP_FALL_MS) {
        game->phase = ARCADE_JUMP_OVER;
    }
}

arcade_jump_pose_t arcade_jump_pose(const arcade_jump_t *game, uint32_t now)
{
    arcade_jump_pose_t pose = {.player_y = game->source.height};
    switch (game->phase) {
    case ARCADE_JUMP_CHARGING:
        pose.power = jump_progress(now, game->phase_ms, JUMP_CHARGE_MS);
        break;
    case ARCADE_JUMP_FLIGHT: {
        const int p = jump_progress(now, game->phase_ms, JUMP_FLIGHT_MS);
        const int height = game->target.height - (int)game->source.height;
        pose.player_x = game->distance * p / 1000;
        pose.player_y += height * p / 1000 + 360 * p * (1000 - p) / 1000000;
        break;
    }
    case ARCADE_JUMP_PAN: {
        const int p = jump_progress(now, game->phase_ms, JUMP_PAN_MS);
        const int remaining = 1000 - p;
        pose.camera_x = game->target.gap * (1000000 - remaining * remaining) / 1000000;
        pose.player_x = game->distance + ((int)game->target.gap - game->distance) * p / 1000;
        pose.player_y = game->target.height;
        break;
    }
    case ARCADE_JUMP_FALL:
    case ARCADE_JUMP_OVER: {
        const int p = game->phase == ARCADE_JUMP_OVER ? 1000 : jump_progress(now, game->phase_ms, JUMP_FALL_MS);
        pose.player_x = game->distance + 14 * p / 1000;
        pose.player_y = game->target.height - 115 * p * p / 1000000;
        break;
    }
    default: break;
    }
    return pose;
}
