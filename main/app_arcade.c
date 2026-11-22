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

arcade_direction_t arcade_tilt_update(arcade_tilt_t *tilt, float x, float y, bool require_neutral)
{
    enum { RELEASE_DEGREES = 7, TILES_DEGREES = 12, SNAKE_DEGREES = 10,
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
    if (require_neutral && tilt->held != ARCADE_NONE) return ARCADE_NONE;
    const float trigger = require_neutral ? TILES_DEGREES : SNAKE_DEGREES;
    arcade_direction_t direction = ARCADE_NONE;
    if (abs_x >= trigger && abs_x - abs_y >= AXIS_MARGIN_DEGREES) {
        direction = x > 0 ? ARCADE_RIGHT : ARCADE_LEFT;
    } else if (abs_y >= trigger && abs_y - abs_x >= AXIS_MARGIN_DEGREES) {
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
