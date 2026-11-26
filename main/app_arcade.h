#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ARCADE_NONE, ARCADE_UP, ARCADE_RIGHT, ARCADE_DOWN, ARCADE_LEFT,
} arcade_direction_t;

typedef struct {
    uint8_t cells[16]; /* A filled cell stores the base-2 exponent. */
    uint32_t random;
    uint32_t score;
    bool won;
    bool over;
} arcade_tiles_t;

void arcade_tiles_init(arcade_tiles_t *game, uint32_t seed);
bool arcade_tiles_move(arcade_tiles_t *game, arcade_direction_t direction);

#define ARCADE_CRATE_SIDE 8
#define ARCADE_CRATE_UNDO 64

typedef struct {
    uint64_t boxes;
    uint32_t moves;
    uint8_t player;
} arcade_crate_turn_t;

typedef struct {
    uint64_t walls;
    uint64_t goals;
    arcade_crate_turn_t turn;
    arcade_crate_turn_t history[ARCADE_CRATE_UNDO];
    uint8_t head;
    uint8_t count;
} arcade_crates_t;

unsigned arcade_crates_levels(void);
void arcade_crates_init(arcade_crates_t *game, unsigned level);
bool arcade_crates_move(arcade_crates_t *game, arcade_direction_t direction);
bool arcade_crates_undo(arcade_crates_t *game);
bool arcade_crates_solved(const arcade_crates_t *game);

#define ARCADE_SNAKE_COLUMNS 22
#define ARCADE_SNAKE_ROWS 17
#define ARCADE_SNAKE_CAPACITY (ARCADE_SNAKE_COLUMNS * ARCADE_SNAKE_ROWS)
#define ARCADE_SNAKE_NO_CELL UINT16_MAX

typedef struct {
    uint16_t body[ARCADE_SNAKE_CAPACITY];
    bool occupied[ARCADE_SNAKE_CAPACITY];
    uint32_t random;
    uint16_t head;
    uint16_t length;
    uint16_t food;
    arcade_direction_t direction;
    arcade_direction_t next;
    bool over;
    bool won;
} arcade_snake_t;

void arcade_snake_init(arcade_snake_t *game, uint32_t seed);
bool arcade_snake_direction(arcade_snake_t *game, arcade_direction_t direction);
bool arcade_snake_step(arcade_snake_t *game);
uint32_t arcade_snake_interval(const arcade_snake_t *game);
uint16_t arcade_snake_cell(const arcade_snake_t *game, unsigned segment);

typedef struct {
    arcade_direction_t held;
    arcade_direction_t candidate;
    uint8_t samples;
    uint8_t neutral_samples;
} arcade_tilt_t;

arcade_direction_t arcade_tilt_update(arcade_tilt_t *tilt, float x_degrees, float y_degrees);

typedef struct {
    float gravity[3], previous[3], gyro_zero[3];
    float grip_turn;
    float candidate_peak, candidate_impulse, candidate_previous;
    uint32_t stamp, steady_ms, lock_ms, candidate_ms;
    arcade_direction_t candidate;
    bool initialized, ready, armed, steady, hold_reference;
} arcade_accel_t;

/* Emit one move for a short screen-plane acceleration pulse. */
bool arcade_accel_update(arcade_accel_t *state, const float accel[3], const float gyro[3],
                         uint32_t stamp, arcade_direction_t *move);

typedef enum {
    ARCADE_JUMP_READY, ARCADE_JUMP_CHARGING, ARCADE_JUMP_FLIGHT,
    ARCADE_JUMP_PAN, ARCADE_JUMP_FALL, ARCADE_JUMP_OVER,
} arcade_jump_phase_t;

typedef struct {
    uint16_t gap;
    uint8_t height;
    uint8_t radius;
} arcade_platform_t;

typedef struct {
    arcade_jump_phase_t phase;
    arcade_platform_t source;
    arcade_platform_t target;
    arcade_platform_t upcoming;
    uint32_t random;
    uint32_t phase_ms;
    uint32_t score;
    uint16_t distance;
    bool perfect;
} arcade_jump_t;

typedef struct {
    int16_t player_x;
    int16_t player_y;
    int16_t camera_x;
    uint16_t power; /* 0 to 1000 */
} arcade_jump_pose_t;

void arcade_jump_init(arcade_jump_t *game, uint32_t seed);
void arcade_jump_press(arcade_jump_t *game, uint32_t now);
void arcade_jump_release(arcade_jump_t *game, uint32_t now);
void arcade_jump_cancel(arcade_jump_t *game);
void arcade_jump_step(arcade_jump_t *game, uint32_t now);
arcade_jump_pose_t arcade_jump_pose(const arcade_jump_t *game, uint32_t now);
uint16_t arcade_jump_distance(uint16_t power);

enum { ARCADE_FALL_COLUMNS = 10, ARCADE_FALL_ROWS = 20, ARCADE_FALL_PIECES = 7 };

typedef struct {
    uint8_t cells[ARCADE_FALL_ROWS][ARCADE_FALL_COLUMNS];
    uint32_t random;
    uint32_t score;
    uint32_t lines;
    uint8_t bag[ARCADE_FALL_PIECES];
    uint8_t bag_pos;
    uint8_t piece, rotation, next;
    int8_t x, y;
    bool over;
} arcade_fall_t;

void arcade_fall_init(arcade_fall_t *game, uint32_t seed);
uint16_t arcade_fall_shape(unsigned piece, unsigned rotation);
bool arcade_fall_move(arcade_fall_t *game, int dx, int dy);
bool arcade_fall_rotate(arcade_fall_t *game);
void arcade_fall_step(arcade_fall_t *game);
void arcade_fall_drop(arcade_fall_t *game);
int arcade_fall_landing(const arcade_fall_t *game);
unsigned arcade_fall_level(const arcade_fall_t *game);
uint32_t arcade_fall_interval(const arcade_fall_t *game);
uint8_t arcade_roll_die(uint32_t *random);
