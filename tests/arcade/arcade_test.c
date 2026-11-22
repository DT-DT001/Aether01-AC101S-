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
    assert(arcade_tilt_update(&tilt, 24, 24, true) == ARCADE_NONE);
    for (unsigned i = 0; i < 10; ++i) {
        assert(arcade_tilt_update(&tilt, 11.9f, 0, true) == ARCADE_NONE);
        assert(arcade_tilt_update(&tilt, 12, 9.1f, true) == ARCADE_NONE);
    }
    assert(arcade_tilt_update(&tilt, 12, 9, true) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 12, 6, true) == ARCADE_RIGHT);
    for (unsigned i = 0; i < 300; ++i) {
        assert(arcade_tilt_update(&tilt, -21, 5, true) == ARCADE_NONE);
    }
    assert(arcade_tilt_update(&tilt, 0, 7, true) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, 7.1f, true) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, -14, 0, true) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, -14, 0, true) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, 6, true) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, 7, true) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, -12, 0, true) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, -12, 0, true) == ARCADE_LEFT);
    tilt = (arcade_tilt_t){0};
    assert(arcade_tilt_update(&tilt, 0, -9.9f, false) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, -10, false) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, NAN, false) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, -10, false) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, -10, false) == ARCADE_UP);
    assert(arcade_tilt_update(&tilt, -10, 0, false) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, -10, 0, false) == ARCADE_LEFT);
    assert(arcade_tilt_update(&tilt, 0, 10, false) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 0, 10, false) == ARCADE_DOWN);
    assert(arcade_tilt_update(&tilt, 10, 0, false) == ARCADE_NONE);
    assert(arcade_tilt_update(&tilt, 10, 0, false) == ARCADE_RIGHT);
    puts("PASS tilt: per-game trigger boundaries, diagonal rejection, fresh confirmation, neutral hysteresis and rapid snake turns");
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

int main(void)
{
    test_tiles();
    test_crates();
    test_snake();
    test_tilt();
    test_jump();
    return 0;
}
