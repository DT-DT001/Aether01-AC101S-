#include "app_arcade.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned occupied_tiles(const arcade_tiles_t *game)
{
    unsigned count = 0;
    for (unsigned i = 0; i < 16; ++i) count += game->cells[i] != 0;
    return count;
}

static uint64_t tile_total(const arcade_tiles_t *game)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < 16; ++i) {
        if (game->cells[i]) sum += UINT64_C(1) << game->cells[i];
    }
    return sum;
}

static void test_tiles(void)
{
    arcade_tiles_t game;
    arcade_tiles_init(&game, 0);
    assert(occupied_tiles(&game) == 2);
    assert(!arcade_tiles_move(&game, ARCADE_NONE));
    game = (arcade_tiles_t){.cells = {1, 1, 1, 1}, .random = 33};
    assert(arcade_tiles_move(&game, ARCADE_LEFT));
    assert(game.cells[0] == 2 && game.cells[1] == 2 && game.score == 8);
    assert(occupied_tiles(&game) == 3);
    game = (arcade_tiles_t){.cells = {2, 1, 1}, .random = 5};
    assert(arcade_tiles_move(&game, ARCADE_RIGHT));
    assert(game.cells[2] == 2 && game.cells[3] == 2 && game.score == 4);
    game = (arcade_tiles_t){.cells = {[0]=10, [4]=10}};
    assert(arcade_tiles_move(&game, ARCADE_DOWN));
    assert(game.cells[12] == 11 && game.won && game.score == 2048);
    game = (arcade_tiles_t){.cells = {[8]=30, [12]=30}, .score = UINT32_MAX - 1};
    assert(arcade_tiles_move(&game, ARCADE_UP));
    assert(game.cells[0] == 31 && game.score == UINT32_MAX);
    game = (arcade_tiles_t){.cells = {1,2,1,2, 2,1,2,1, 1,2,1,2, 2,1,2,1}, .random = 100};
    assert(!arcade_tiles_move(&game, ARCADE_DOWN) && game.over && game.random == 100);

    for (uint32_t seed = 1; seed <= 100; ++seed) {
        arcade_tiles_init(&game, seed);
        for (unsigned step = 0; step < 250 && !game.over; ++step) {
            const uint64_t prior = tile_total(&game);
            const uint32_t old_random = game.random;
            const bool moved = arcade_tiles_move(&game, (arcade_direction_t)(1 + (step + seed) % 4));
            const uint64_t increase = tile_total(&game) - prior;
            if (moved) assert(increase == 2 || increase == 4);
            else assert(increase == 0 && old_random == game.random);
        }
    }
    puts("PASS number tiles: four directions, single-merge rule, goal, no-op and game over");
}

static arcade_direction_t solution_move(char c)
{
    switch (c) {
    case 'U': return ARCADE_UP;
    case 'R': return ARCADE_RIGHT;
    case 'D': return ARCADE_DOWN;
    case 'L': return ARCADE_LEFT;
    default: return ARCADE_NONE;
    }
}

static void test_crates(void)
{
    static const char *const solutions[] = {
        "UURUL",
        "LURRDRU",
        "UURRULL",
        "RRUUURDUURRDDLLDUUULDDDDRR",
        "RUDLLUURURULDDRRRLLDDRRRUULLDLDRR",
        "RDRRULUULULLDRRLLLDDRRRDRRUUUULLRRDDLDUURUL",
    };
    arcade_crates_t game;
    assert(arcade_crates_levels() == sizeof(solutions) / sizeof(solutions[0]));
    for (unsigned i = 0; i < arcade_crates_levels(); ++i) {
        arcade_crates_init(&game, i);
        assert(!arcade_crates_solved(&game) && !arcade_crates_undo(&game));
        for (const char *move = solutions[i]; *move; ++move) {
            assert(!arcade_crates_solved(&game));
            assert(arcade_crates_move(&game, solution_move(*move)));
        }
        assert(arcade_crates_solved(&game));
        assert(game.turn.moves == strlen(solutions[i]));
        assert(!arcade_crates_move(&game, ARCADE_UP));
        assert(arcade_crates_undo(&game) && !arcade_crates_solved(&game));
    }
    arcade_crates_init(&game, 0);
    const arcade_crate_turn_t start = game.turn;
    assert(arcade_crates_move(&game, ARCADE_UP));
    assert(!arcade_crates_move(&game, ARCADE_RIGHT));
    assert(arcade_crates_undo(&game));
    assert(game.turn.boxes == start.boxes && game.turn.player == start.player);
    for (unsigned i = 0; i < ARCADE_CRATE_UNDO + 6; ++i) {
        assert(arcade_crates_move(&game, i % 2 ? ARCADE_RIGHT : ARCADE_LEFT));
    }
    assert(game.count == ARCADE_CRATE_UNDO);
    for (unsigned i = 0; i < ARCADE_CRATE_UNDO; ++i) assert(arcade_crates_undo(&game));
    assert(!arcade_crates_undo(&game) && game.turn.moves == 6 && game.turn.player == start.player);
    puts("PASS crates: all six original layouts are solvable, wall blocking and bounded undo");
}

static uint16_t snake_index(unsigned x, unsigned y)
{
    return y * ARCADE_SNAKE_COLUMNS + x;
}

static void snake_loop(arcade_snake_t *game, bool tail_hit)
{
    *game = (arcade_snake_t){
        .body = {snake_index(5,5), snake_index(5,6), snake_index(6,6),
                 snake_index(6,5), snake_index(6,4)},
        .length = tail_hit ? 4 : 5,
        .food = ARCADE_SNAKE_NO_CELL,
        .direction = ARCADE_UP,
        .next = ARCADE_UP,
    };
    for (unsigned i = 0; i < game->length; ++i) game->occupied[game->body[i]] = true;
}

static void test_snake(void)
{
    arcade_snake_t game;
    arcade_snake_init(&game, 42);
    assert(game.length == 3 && !game.occupied[game.food] && !arcade_snake_step(&game));
    assert(!arcade_snake_direction(&game, ARCADE_LEFT));
    const uint16_t old_head = arcade_snake_cell(&game, 0);
    game.food = old_head + 1;
    assert(arcade_snake_direction(&game, ARCADE_RIGHT));
    assert(!arcade_snake_direction(&game, ARCADE_UP));
    assert(arcade_snake_step(&game) && game.length == 4);
    assert(arcade_snake_cell(&game, 0) == old_head + 1);
    assert(!game.occupied[game.food] && arcade_snake_interval(&game) == 225);
    assert(!arcade_snake_direction(&game, ARCADE_LEFT));
    assert(arcade_snake_direction(&game, ARCADE_UP));
    while (arcade_snake_step(&game)) {}
    assert(game.over && !game.won);

    arcade_snake_init(&game, 42);
    const unsigned center_x = ARCADE_SNAKE_COLUMNS / 2;
    assert(arcade_snake_cell(&game, 0) == snake_index(center_x, ARCADE_SNAKE_ROWS / 2));
    assert(arcade_snake_direction(&game, ARCADE_DOWN));
    for (unsigned row = ARCADE_SNAKE_ROWS / 2 + 1; row < ARCADE_SNAKE_ROWS; ++row) {
        assert(arcade_snake_step(&game));
        assert(arcade_snake_cell(&game, 0) == snake_index(center_x, row));
    }
    assert(!arcade_snake_step(&game) && game.over);

    snake_loop(&game, true);
    assert(arcade_snake_direction(&game, ARCADE_RIGHT) && arcade_snake_step(&game));
    assert(game.length == 4 && !game.over && game.occupied[snake_index(6,5)]);
    snake_loop(&game, false);
    assert(arcade_snake_direction(&game, ARCADE_RIGHT) && !arcade_snake_step(&game));
    assert(game.over);

    game = (arcade_snake_t){
        .length = ARCADE_SNAKE_CAPACITY - 1, .food = 1,
        .direction = ARCADE_RIGHT, .next = ARCADE_RIGHT,
    };
    game.occupied[0] = true;
    for (unsigned cell = 2; cell < ARCADE_SNAKE_CAPACITY; ++cell) {
        game.occupied[cell] = true;
        game.body[cell - 1] = cell;
    }
    assert(arcade_snake_step(&game) && game.length == ARCADE_SNAKE_CAPACITY);
    assert(game.won && game.over && game.food == ARCADE_SNAKE_NO_CELL);
    puts("PASS snake: buffered direction, expanded rows and bottom wall, body collision, move into tail, growth and full board");
}

static void test_tilt(void)
{
    arcade_tilt_t tilt = {0};
    assert(arcade_tilt_update(&tilt, 24, 24) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, -9.9f) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, -10) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, NAN) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, -10) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, -10) == ARCADE_UP);
    assert(arcade_tilt_update(&tilt, -10, 0) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, -10, 0) == ARCADE_LEFT);
    assert(arcade_tilt_update(&tilt, 0, 10) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, 10) == ARCADE_DOWN);
    assert(arcade_tilt_update(&tilt, 10, 0) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 10, 0) == ARCADE_RIGHT);
    assert(arcade_tilt_update(&tilt, 0, 7) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, 7) == ARCADE_NONE);
    assert(tilt.held == ARCADE_NONE);
    puts("PASS snake tilt: diagonal and invalid readings, two fresh samples, neutral hysteresis and rapid turns");
}

static arcade_direction_t feed_accel(arcade_accel_t *state, const float accel[3],
                                     const float gyro[3], uint32_t stamp)
{
    arcade_direction_t move;
    assert(arcade_accel_update(state, accel, gyro, stamp, &move));
    return move;
}

static void hold_stationary(arcade_accel_t *state, const float accel[3], const float gyro[3],
                            uint32_t *now, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        *now += i % 2 ? 19 : 29;
        assert(feed_accel(state, accel, gyro, *now) == ARCADE_NONE);
    }
}

static void test_accel(void)
{
    static const float board_gravity[3] = {-.538f, -.229f, 8.863f};
    static const float gyro_bias[3] = {8.58f, 3.19f, -.1f};
    static const struct {
        unsigned axis;
        float impulse;
        arcade_direction_t direction;
    } directions[] = {
        {0, 2.2f, ARCADE_RIGHT}, {0, -2.2f, ARCADE_LEFT},
        {1, 2.2f, ARCADE_UP}, {1, -2.2f, ARCADE_DOWN},
    };
    for (unsigned i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
        arcade_accel_t state = {0};
        uint32_t now = UINT32_MAX - 150U;
        hold_stationary(&state, board_gravity, gyro_bias, &now, 10);
        assert(state.ready && state.armed);
        float pulse[3];
        memcpy(pulse, board_gravity, sizeof(pulse));
        pulse[directions[i].axis] += directions[i].impulse;
        assert(feed_accel(&state, pulse, gyro_bias, now += 22) == ARCADE_NONE);
        assert(feed_accel(&state, pulse, gyro_bias, now) == ARCADE_NONE);
        assert(feed_accel(&state, pulse, gyro_bias, now - 2) == ARCADE_NONE);
        assert(feed_accel(&state, pulse, gyro_bias, now += 27) == directions[i].direction);
        hold_stationary(&state, pulse, gyro_bias, &now, 24);
        assert(state.ready && !state.armed);
        pulse[directions[i].axis] -= 2 * directions[i].impulse;
        hold_stationary(&state, pulse, gyro_bias, &now, 5);
        assert(!state.armed);
        hold_stationary(&state, board_gravity, gyro_bias, &now, 9);
        assert(state.armed);

        /* On a slight slant these pure translations leave |a| unchanged. */
        state = (arcade_accel_t){0};
        float tilted[3] = {0, 0, 8.8f};
        tilted[directions[i].axis] = -directions[i].impulse / 2;
        hold_stationary(&state, tilted, gyro_bias, &now, 10);
        memcpy(pulse, tilted, sizeof(pulse));
        pulse[directions[i].axis] += directions[i].impulse;
        assert(feed_accel(&state, pulse, gyro_bias, now += 24) == ARCADE_NONE);
        assert(feed_accel(&state, pulse, gyro_bias, now += 23) == directions[i].direction);
        hold_stationary(&state, pulse, gyro_bias, &now, 18);
        assert(!state.armed);
    }

    arcade_accel_t state = {0};
    uint32_t now = 0;
    static const float right[3] = {2.5f, -.229f, 8.863f};
    static const float left[3] = {-3.5f, -.229f, 8.863f};
    for (unsigned quiet_frames = 0; quiet_frames < 2; ++quiet_frames) {
        state = (arcade_accel_t){0};
        hold_stationary(&state, board_gravity, gyro_bias, &now, 10);
        assert(feed_accel(&state, right, gyro_bias, now += 20) == ARCADE_NONE);
        hold_stationary(&state, board_gravity, gyro_bias, &now, quiet_frames);
        hold_stationary(&state, left, gyro_bias, &now, 5);
        hold_stationary(&state, board_gravity, gyro_bias, &now, 14);
        assert(state.armed);
    }
    assert(feed_accel(&state, right, gyro_bias, now += 20) == ARCADE_NONE);
    assert(feed_accel(&state, right, gyro_bias, now += 20) == ARCADE_RIGHT);
    /* A slow change of grip shifts gravity past the old quiet threshold. */
    const float rotating[3] = {gyro_bias[0], gyro_bias[1] + 12.5f, gyro_bias[2]};
    float tilted[3] = {0};
    for (unsigned i = 0; i < 35; ++i) {
        const float angle = (float)(i + 1U) * .00523599f;
        tilted[0] = board_gravity[0] * cosf(angle) - board_gravity[2] * sinf(angle);
        tilted[1] = board_gravity[1];
        tilted[2] = board_gravity[2] * cosf(angle) + board_gravity[0] * sinf(angle);
        assert(feed_accel(&state, tilted, rotating, now += 24) == ARCADE_NONE);
    }
    hold_stationary(&state, tilted, gyro_bias, &now, 9);
    assert(state.armed);
    tilted[1] += 2.7f;
    assert(feed_accel(&state, tilted, gyro_bias, now += 26) == ARCADE_NONE);
    assert(feed_accel(&state, tilted, gyro_bias, now += 24) == ARCADE_UP);

    state = (arcade_accel_t){0};
    hold_stationary(&state, board_gravity, NULL, &now, 10);
    assert(state.armed);
    const float pure_tilt[3] = {3.0f, 0, sqrtf(.538f * .538f + .229f * .229f + 8.863f * 8.863f - 9.0f)};
    hold_stationary(&state, pure_tilt, NULL, &now, 15);
    assert(state.armed);
    const float pure_lift[3] = {3.0f, 0, pure_tilt[2] + 4.0f};
    hold_stationary(&state, pure_lift, NULL, &now, 9);
    hold_stationary(&state, pure_tilt, NULL, &now, 15);
    const float diagonal[3] = {6.2f, 3.2f, pure_tilt[2]};
    hold_stationary(&state, diagonal, NULL, &now, 8);
    hold_stationary(&state, pure_tilt, NULL, &now, 15);
    assert(state.armed);
    const float up[3] = {3.0f, 2.8f, pure_tilt[2]};
    assert(feed_accel(&state, up, NULL, now += 20) == ARCADE_NONE);
    assert(feed_accel(&state, up, NULL, now += 90) == ARCADE_NONE && !state.ready);
    hold_stationary(&state, pure_tilt, NULL, &now, 9);
    assert(feed_accel(&state, up, NULL, now += 20) == ARCADE_NONE);
    assert(feed_accel(&state, up, NULL, now += 20) == ARCADE_UP);
    const float bad[3] = {NAN, 0, 0};
    arcade_direction_t move = ARCADE_UP;
    assert(!arcade_accel_update(&state, bad, gyro_bias, now += 20, &move));
    assert(!state.ready && move == ARCADE_NONE);
    puts("PASS 2048 acceleration: biased real-board rest, four directions, slanted grip, stale frames, tilt/diagonal/lift rejection, single-frame pulse, brake/hold and regrip");
}

static void test_soft_accel(void)
{
    static const float grip[3] = {-.538f, -.229f, 8.863f};
    static const float bias[3] = {8.58f, 3.19f, -.1f};
    static const arcade_direction_t directions[] = {ARCADE_RIGHT, ARCADE_LEFT, ARCADE_UP, ARCADE_DOWN};
    static const struct { float peak; int duration; bool slow_samples, low_pass; } pulses[] = {
        {1.78f, 47, false, false}, {1.64f, 73, false, false}, {1.55f, 107, false, false},
        {1.72f, 114, true, false}, {2.40f, 54, false, true}, {1.99f, 80, false, true},
        {1.76f, 108, false, true},
    };
    for (unsigned m = 0; m < sizeof(pulses) / sizeof(pulses[0]); ++m) {
        for (unsigned dir = 0; dir < sizeof(directions) / sizeof(directions[0]); ++dir) {
            for (int phase = 0; phase < 10; ++phase) {
                arcade_accel_t state = {0};
                uint32_t now = 0;
                hold_stationary(&state, grip, bias, &now, 10);
                const unsigned axis = dir / 2;
                const float sign = dir % 2 ? -1.0f : 1.0f;
                const int duration = pulses[m].duration;
                const int brake_start = duration + 4;
                unsigned moves = 0;
                float filtered[3] = {grip[0], grip[1], grip[2]};
                for (int t = -phase; t < brake_start + 440;) {
                    /* Eight to twelve milliseconds, or legacy twenty to
                     * twenty-eight milliseconds, with different frame phase. */
                    const int interval = (pulses[m].slow_samples ? 21 : 8) + (t + phase) % 5;
                    const float angle = (float)(t < 0 ? 0 : t > duration ? duration : t) *
                                        sign * .00012f;
                    const unsigned side = 1U - axis;
                    float a[3] = {grip[0], grip[1], grip[2]};
                    float gyro[3] = {bias[0], bias[1], bias[2]};
                    a[axis] = grip[axis] * cosf(angle) - grip[2] * sinf(angle);
                    a[2] = grip[2] * cosf(angle) + grip[axis] * sinf(angle);
                    if (t >= 0 && t < duration) gyro[side] += axis == 0 ? sign * 6.88f : -sign * 6.88f;
                    const float push = t >= 0 && t < duration ? sinf(3.14159265f * (float)t / (float)duration) : 0.0f;
                    const float stop = t >= brake_start && t < brake_start + 76 ?
                                       sinf(3.14159265f * (float)(t - brake_start) / 76.0f) : 0.0f;
                    a[axis] += sign * (pulses[m].peak * push - 2.45f * stop) + .035f * sinf((float)t);
                    a[side] += .26f * push + .04f * cosf((float)t);
                    a[2] += .23f * push;
                    if (pulses[m].low_pass) {
                        /* Approximate the QMI8658A's ~6 Hz digital filter. */
                        const float gain = 1.0f - expf((float)-interval * .037699f);
                        for (unsigned i = 0; i < 3; ++i) {
                            filtered[i] += gain * (a[i] - filtered[i]);
                            a[i] = filtered[i];
                        }
                    }
                    const arcade_direction_t move = feed_accel(&state, a, gyro, now += (unsigned)interval);
                    if (move != ARCADE_NONE) {
                        assert(move == directions[dir] && t < brake_start + (pulses[m].low_pass ? 16 : 0));
                        ++moves;
                    }
                    t += interval;
                }
                if (moves != 1 || !state.armed)
                    fprintf(stderr, "Soft input: curve %u, direction %u, phase %d, moves %u, armed %d, stored gravity %.2f/%.2f/%.2f\n",
                            m, dir, phase, moves, state.armed, state.gravity[0], state.gravity[1], state.gravity[2]);
                assert(moves == 1 && state.armed);
            }
        }
    }

    arcade_accel_t state = {0};
    uint32_t now = 0;
    hold_stationary(&state, grip, bias, &now, 10);
    float peak[3] = {grip[0] + 2.1f, grip[1], grip[2]};
    float decay[3] = {grip[0] + .75f, grip[1], grip[2]};
    assert(feed_accel(&state, peak, bias, now += 10) == ARCADE_NONE);
    assert(feed_accel(&state, decay, bias, now += 10) == ARCADE_RIGHT);
    const float left[3] = {grip[0] - 2.9f, grip[1], grip[2]};
    hold_stationary(&state, left, bias, &now, 4);
    hold_stationary(&state, grip, bias, &now, 12);
    assert(state.armed);
    /* Continuous physical tilting at several real rates must not move a tile. */
    const float rates[] = {10, 23, 39, 68};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        state = (arcade_accel_t){0};
        hold_stationary(&state, grip, bias, &now, 10);
        float angle = 0.0f;
        float a[3] = {0};
        const float gyro[3] = {bias[0], bias[1] + rates[i], bias[2]};
        for (unsigned n = 0; n < 80; ++n) {
            const int interval = 8 + n % 5;
            angle += (float)interval * rates[i] * .0000174532925f;
            a[0] = grip[0] * cosf(angle) - grip[2] * sinf(angle) + .035f * sinf((float)n);
            a[1] = grip[1];
            a[2] = grip[2] * cosf(angle) + grip[0] * sinf(angle);
            assert(feed_accel(&state, a, gyro, now += (unsigned)interval) == ARCADE_NONE);
        }
        hold_stationary(&state, a, bias, &now, 17);
        assert(state.armed);
    }
    puts("PASS 2048 soft input: 280 short/gentle mixed-axis trajectories, ~6 Hz filter, decaying confirmation and real-rate pure tilt");
}

static void test_jump(void)
{
    arcade_jump_t game;
    arcade_jump_init(&game, 123);
    arcade_jump_release(&game, 100);
    assert(game.phase == ARCADE_JUMP_READY);
    arcade_jump_press(&game, 150);
    arcade_jump_cancel(&game);
    assert(game.phase == ARCADE_JUMP_READY);
    arcade_jump_press(&game, 300);
    assert(arcade_jump_pose(&game, 450).power > 0);
    assert(arcade_jump_pose(&game, 2000).power == 1000);
    arcade_jump_release(&game, 600);
    assert(game.phase == ARCADE_JUMP_FLIGHT && game.distance >= 105 && game.distance <= 108);
    assert(arcade_jump_pose(&game, 845).player_y > 90);
    arcade_jump_step(&game, 1090);
    assert(game.phase == ARCADE_JUMP_PAN && game.perfect && game.score == 2);
    assert(arcade_jump_pose(&game, 1200).camera_x > 0);
    arcade_jump_step(&game, 1300);
    assert(game.phase == ARCADE_JUMP_READY && game.source.gap == 106 && game.target.gap >= 88);
    const uint16_t gap = game.target.gap;
    for (unsigned i = 0; i < 200; ++i) {
        const uint32_t start = 1500 + i * 2400;
        const uint16_t target = game.target.gap;
        const uint32_t hold = ((uint32_t)(target - 38) * 850 + 195) / 196;
        arcade_jump_press(&game, start);
        arcade_jump_release(&game, start + hold);
        arcade_jump_step(&game, start + hold + 1000);
        assert(game.phase == ARCADE_JUMP_READY && game.perfect);
        assert(game.target.gap >= 88 && game.target.gap <= 196);
    }
    assert(game.score == 402 && gap <= 159);
    const uint32_t wrap = UINT32_MAX - 80;
    arcade_jump_press(&game, wrap);
    arcade_jump_release(&game, wrap + 20);
    arcade_jump_step(&game, wrap + 510);
    assert(game.phase == ARCADE_JUMP_FALL && game.score == 402);
    arcade_jump_step(&game, wrap + 900);
    assert(game.phase == ARCADE_JUMP_OVER && arcade_jump_pose(&game, 1000).player_y < 0);
    puts("PASS jump: charge/cancel, physical arc, perfect/failed landing, camera and tick rollover");
}

static void test_fall(void)
{
    arcade_fall_t game;
    for (uint32_t seed = 0; seed < 64; ++seed) {
        arcade_fall_init(&game, seed);
        for (unsigned bag = 0; bag < 8; ++bag) {
            unsigned seen = 0;
            for (unsigned i = 0; i < ARCADE_FALL_PIECES; ++i) {
                assert(game.piece < ARCADE_FALL_PIECES && !(seen & (1U << game.piece)));
                seen |= 1U << game.piece;
                const uint8_t next = game.next;
                arcade_fall_drop(&game);
                assert(!game.over && game.piece == next && game.y == 0);
                memset(game.cells, 0, sizeof(game.cells));
            }
        }
    }

    arcade_fall_init(&game, 22);
    game.piece = 0;
    game.rotation = 1;
    game.x = -2;
    assert(!arcade_fall_move(&game, -1, 0));
    assert(arcade_fall_rotate(&game) && game.x == 0 && game.rotation == 2);
    game.x = 3;
    game.rotation = 1;
    game.lines = 9;
    game.cells[15][0] = 2;
    for (int row = 16; row < 20; ++row) {
        for (int col = 0; col < 10; ++col) {
            if (col != 5) game.cells[row][col] = 4;
        }
    }
    assert(arcade_fall_interval(&game) == 800 && arcade_fall_landing(&game) == 16);
    arcade_fall_drop(&game);
    assert(game.lines == 13 && game.score == 832 && arcade_fall_level(&game) == 2);
    assert(game.cells[19][0] == 2 && !game.cells[18][0] && !game.cells[0][0]);
    assert(arcade_fall_interval(&game) < 800);
    arcade_fall_init(&game, 100);
    for (unsigned i = 0; !game.over && i < 20; ++i) arcade_fall_drop(&game);
    assert(game.over && !arcade_fall_rotate(&game) && !arcade_fall_move(&game, 0, 1));
    const uint32_t score = game.score;
    arcade_fall_drop(&game);
    arcade_fall_step(&game);
    assert(game.score == score);
    puts("PASS falling blocks: 512 complete bags, next-piece order, wall kick, four-line collapse, speed and top-out");
}

static void test_dice(void)
{
    uint32_t random = 0;
    unsigned distribution[6] = {0};
    for (unsigned i = 0; i < 60000; ++i) {
        const uint8_t face = arcade_roll_die(&random);
        assert(face >= 1 && face <= 6);
        ++distribution[face - 1];
    }
    for (unsigned i = 0; i < 6; ++i) assert(distribution[i] > 9600 && distribution[i] < 10400);
    puts("PASS dice: 60,000 rolls with legal faces and even distribution");
}

int main(void)
{
    test_tiles();
    test_crates();
    test_snake();
    test_tilt();
    test_accel();
    test_soft_accel();
    test_jump();
    test_fall();
    test_dice();
    return 0;
}
