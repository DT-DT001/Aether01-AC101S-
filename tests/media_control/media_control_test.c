/* The dispatcher and all media public headers are production. Hardware-facing
 * typedefs and backend command/status endpoints alone are substituted. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_media_control.h"

static char audio_token, video_token, nes_token;
#define AUDIO ((aether_audio_player_handle_t)&audio_token)
#define VIDEO ((aether_video_player_handle_t)&video_token)
#define NES ((aether_nes_handle_t)&nes_token)

static aether_audio_player_status_t music;
static aether_video_status_t movie;
static aether_nes_status_t game;
static esp_err_t snapshot_error[3], command_error;
static unsigned snapshots[3], calls;
enum command { NONE, AUDIO_PLAY, AUDIO_PAUSE, AUDIO_STOP, AUDIO_NEXT,
               VIDEO_PAUSE, VIDEO_STOP, NES_PAUSE, NES_STOP };
static enum command last;
static bool requested_pause;
static int requested_direction, requested_volume;
static char requested_path[192];

static esp_err_t record(enum command command)
{
    ++calls;
    last = command;
    return command_error;
}
esp_err_t aether_audio_player_get_status(aether_audio_player_handle_t h,
                                         aether_audio_player_status_t *out)
{
    assert(h == AUDIO && out);
    ++snapshots[0];
    *out = music;
    return snapshot_error[0];
}
esp_err_t aether_video_player_get_status(aether_video_player_handle_t h,
                                         aether_video_status_t *out)
{
    assert(h == VIDEO && out);
    ++snapshots[1];
    *out = movie;
    return snapshot_error[1];
}
esp_err_t aether_nes_get_status(aether_nes_handle_t h, aether_nes_status_t *out)
{
    assert(h == NES && out);
    ++snapshots[2];
    *out = game;
    return snapshot_error[2];
}
esp_err_t aether_audio_player_play(aether_audio_player_handle_t h, const char *path, int volume)
{
    assert(h == AUDIO && path && strlen(path) < sizeof(requested_path));
    strcpy(requested_path, path);
    requested_volume = volume;
    return record(AUDIO_PLAY);
}
esp_err_t aether_audio_player_set_paused(aether_audio_player_handle_t h, bool pause)
{ assert(h == AUDIO); requested_pause = pause; return record(AUDIO_PAUSE); }
esp_err_t aether_audio_player_stop(aether_audio_player_handle_t h)
{ assert(h == AUDIO); return record(AUDIO_STOP); }
esp_err_t aether_audio_player_next(aether_audio_player_handle_t h, int direction)
{ assert(h == AUDIO); requested_direction = direction; return record(AUDIO_NEXT); }
esp_err_t aether_video_player_set_paused(aether_video_player_handle_t h, bool pause)
{ assert(h == VIDEO); requested_pause = pause; return record(VIDEO_PAUSE); }
esp_err_t aether_video_player_stop(aether_video_player_handle_t h)
{ assert(h == VIDEO); return record(VIDEO_STOP); }
esp_err_t aether_nes_set_paused(aether_nes_handle_t h, bool pause)
{ assert(h == NES); requested_pause = pause; return record(NES_PAUSE); }
esp_err_t aether_nes_stop(aether_nes_handle_t h)
{ assert(h == NES); return record(NES_STOP); }

static void clear_calls(void)
{
    calls = 0;
    last = NONE;
    requested_pause = false;
    requested_direction = requested_volume = 0;
    requested_path[0] = 0;
    memset(snapshots, 0, sizeof(snapshots));
}
static void reset(void)
{
    memset(&music, 0, sizeof(music));
    memset(&movie, 0, sizeof(movie));
    memset(&game, 0, sizeof(game));
    memset(snapshot_error, 0, sizeof(snapshot_error));
    command_error = ESP_OK;
    /* A historical playlist/path must never steal the active movie/game. */
    strcpy(music.path, "/sdcard/Music/original-fixture.wav");
    music.volume = 73;
    music.track_count = 2;
    clear_calls();
}
static esp_err_t dispatch(aether_bluetooth_remote_action_t action)
{ return app_media_control_dispatch(AUDIO, VIDEO, NES, action); }
static bool flash_busy(void)
{ return app_media_control_flash_busy(AUDIO, VIDEO, NES); }
static void expect_call(aether_bluetooth_remote_action_t action, enum command expected)
{
    clear_calls();
    assert(dispatch(action) == command_error);
    assert(calls == 1 && last == expected);
    assert(snapshots[0] == 1 && snapshots[1] == 1 && snapshots[2] == 1);
}
static void expect_rejection(aether_bluetooth_remote_action_t action, esp_err_t error)
{
    clear_calls();
    assert(dispatch(action) == error);
    assert(!calls && last == NONE);
}

static void test_flash_gate(void)
{
    reset();
    assert(!app_media_control_flash_busy(NULL, NULL, NULL));
    assert(!flash_busy());
    const aether_audio_player_state_t audio_busy[] = {
        AETHER_AUDIO_STARTING, AETHER_AUDIO_PLAYING,
    };
    for (size_t i = 0; i < sizeof(audio_busy) / sizeof(audio_busy[0]); ++i) {
        reset(); music.state = audio_busy[i]; music.active = true;
        assert(flash_busy() && !calls);
    }
    reset(); music.state = AETHER_AUDIO_STARTING; /* Startup cannot escape via active=false. */
    assert(flash_busy());
    reset(); music.state = AETHER_AUDIO_PAUSED; music.active = true;
    assert(!flash_busy());
    const aether_mjpeg_state_t video_states[] = {
        AETHER_VIDEO_STOPPED, AETHER_VIDEO_STARTING, AETHER_VIDEO_PLAYING,
        AETHER_VIDEO_PAUSED, AETHER_VIDEO_FINISHED, AETHER_VIDEO_ERROR,
    };
    for (size_t i = 0; i < sizeof(video_states) / sizeof(video_states[0]); ++i) {
        reset(); movie.state = video_states[i];
        assert(flash_busy() == (i == 1 || i == 2));
    }
    const aether_nes_state_t nes_states[] = {
        AETHER_NES_STOPPED, AETHER_NES_STARTING, AETHER_NES_RUNNING,
        AETHER_NES_PAUSED, AETHER_NES_ERROR,
    };
    for (size_t i = 0; i < sizeof(nes_states) / sizeof(nes_states[0]); ++i) {
        reset(); game.state = nes_states[i];
        assert(flash_busy() == (i == 1 || i == 2));
    }
    for (unsigned backend = 0; backend < 3; ++backend) {
        reset(); snapshot_error[backend] = ESP_ERR_TIMEOUT;
        assert(flash_busy() && !calls);
    }
    puts("PASS Flash gate includes STARTING/running, permits paused/idle, fails busy on unavailable snapshots");
}

static void test_music_and_idle(void)
{
    reset();
    expect_call(AETHER_BT_REMOTE_PLAY, AUDIO_PLAY);
    assert(requested_volume == 73 && !strcmp(requested_path, music.path));
    expect_rejection(AETHER_BT_REMOTE_PAUSE, ESP_ERR_INVALID_STATE);
    expect_call(AETHER_BT_REMOTE_NEXT, AUDIO_NEXT); assert(requested_direction == 1);
    expect_call(AETHER_BT_REMOTE_PREVIOUS, AUDIO_NEXT); assert(requested_direction == -1);
    expect_call(AETHER_BT_REMOTE_STOP, AUDIO_STOP);
    music.path[0] = 0;
    expect_rejection(AETHER_BT_REMOTE_PLAY, ESP_ERR_INVALID_STATE);
    reset(); music.active = true; music.state = AETHER_AUDIO_PLAYING;
    expect_call(AETHER_BT_REMOTE_PAUSE, AUDIO_PAUSE); assert(requested_pause);
    music.state = AETHER_AUDIO_PAUSED;
    expect_call(AETHER_BT_REMOTE_PLAY, AUDIO_PAUSE); assert(!requested_pause);
    expect_call(AETHER_BT_REMOTE_PAUSE, AUDIO_PAUSE); assert(requested_pause);
    expect_call(AETHER_BT_REMOTE_STOP, AUDIO_STOP);
    music.active = false; music.state = AETHER_AUDIO_STARTING;
    expect_rejection(AETHER_BT_REMOTE_PLAY, ESP_ERR_INVALID_STATE);
    expect_rejection(AETHER_BT_REMOTE_PAUSE, ESP_ERR_INVALID_STATE);
    expect_call(AETHER_BT_REMOTE_STOP, AUDIO_STOP);
    reset(); music.state = AETHER_AUDIO_FINISHED;
    expect_call(AETHER_BT_REMOTE_PLAY, AUDIO_PLAY);
    reset(); music.state = AETHER_AUDIO_ERROR;
    expect_call(AETHER_BT_REMOTE_PLAY, AUDIO_PLAY);
    clear_calls();
    assert(app_media_control_dispatch(NULL, NULL, NULL, AETHER_BT_REMOTE_STOP) == ESP_OK);
    assert(!calls);
    assert(app_media_control_dispatch(NULL, NULL, NULL, AETHER_BT_REMOTE_PLAY) == ESP_ERR_NOT_FOUND);
    puts("PASS music owner pause/resume/stop, next directions, idle replay preserves path/volume, empty/no-handle refusal");
}

static void test_movie_and_game(void)
{
    const aether_mjpeg_state_t movie_owners[] = {
        AETHER_VIDEO_STARTING, AETHER_VIDEO_PLAYING, AETHER_VIDEO_PAUSED,
    };
    for (size_t i = 0; i < sizeof(movie_owners) / sizeof(movie_owners[0]); ++i) {
        reset(); movie.state = movie_owners[i];
        const esp_err_t unsupported = i == 0 ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_SUPPORTED;
        expect_rejection(AETHER_BT_REMOTE_NEXT, unsupported);
        expect_rejection(AETHER_BT_REMOTE_PREVIOUS, unsupported);
        if (i == 0) {
            expect_rejection(AETHER_BT_REMOTE_PLAY, ESP_ERR_INVALID_STATE);
            expect_rejection(AETHER_BT_REMOTE_PAUSE, ESP_ERR_INVALID_STATE);
        } else {
            expect_call(AETHER_BT_REMOTE_PLAY, VIDEO_PAUSE); assert(!requested_pause);
            expect_call(AETHER_BT_REMOTE_PAUSE, VIDEO_PAUSE); assert(requested_pause);
        }
        expect_call(AETHER_BT_REMOTE_STOP, VIDEO_STOP);
    }
    const aether_nes_state_t game_owners[] = {
        AETHER_NES_STARTING, AETHER_NES_RUNNING, AETHER_NES_PAUSED,
    };
    for (size_t i = 0; i < sizeof(game_owners) / sizeof(game_owners[0]); ++i) {
        reset(); game.state = game_owners[i];
        const esp_err_t unsupported = i == 0 ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_SUPPORTED;
        expect_rejection(AETHER_BT_REMOTE_NEXT, unsupported);
        expect_rejection(AETHER_BT_REMOTE_PREVIOUS, unsupported);
        if (i == 0) {
            expect_rejection(AETHER_BT_REMOTE_PLAY, ESP_ERR_INVALID_STATE);
            expect_rejection(AETHER_BT_REMOTE_PAUSE, ESP_ERR_INVALID_STATE);
        } else {
            expect_call(AETHER_BT_REMOTE_PLAY, NES_PAUSE); assert(!requested_pause);
            expect_call(AETHER_BT_REMOTE_PAUSE, NES_PAUSE); assert(requested_pause);
        }
        expect_call(AETHER_BT_REMOTE_STOP, NES_STOP);
    }
    reset(); movie.state = AETHER_VIDEO_FINISHED; game.state = AETHER_NES_ERROR;
    expect_call(AETHER_BT_REMOTE_PLAY, AUDIO_PLAY);
    puts("PASS movie/NES STARTING/playing/paused block old-music NEXT/PREV/PLAY; actual owner receives stop/pause/resume");
}

static void test_snapshot_and_command_failures(void)
{
    const aether_bluetooth_remote_action_t actions[] = {
        AETHER_BT_REMOTE_PLAY, AETHER_BT_REMOTE_PAUSE, AETHER_BT_REMOTE_STOP,
        AETHER_BT_REMOTE_NEXT, AETHER_BT_REMOTE_PREVIOUS,
    };
    for (unsigned backend = 0; backend < 3; ++backend) {
        for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
            reset(); music.active = true; music.state = AETHER_AUDIO_PLAYING;
            snapshot_error[backend] = ESP_ERR_TIMEOUT;
            expect_rejection(actions[i], ESP_ERR_TIMEOUT);
        }
    }
    reset(); command_error = ESP_ERR_NO_MEM;
    expect_call(AETHER_BT_REMOTE_PLAY, AUDIO_PLAY);
    expect_call(AETHER_BT_REMOTE_NEXT, AUDIO_NEXT);
    expect_call(AETHER_BT_REMOTE_STOP, AUDIO_STOP);
    music.active = true; music.state = AETHER_AUDIO_PLAYING;
    expect_call(AETHER_BT_REMOTE_PAUSE, AUDIO_PAUSE);
    music.active = false; movie.state = AETHER_VIDEO_PLAYING;
    expect_call(AETHER_BT_REMOTE_PAUSE, VIDEO_PAUSE);
    expect_call(AETHER_BT_REMOTE_STOP, VIDEO_STOP);
    movie.state = AETHER_VIDEO_STOPPED; game.state = AETHER_NES_RUNNING;
    expect_call(AETHER_BT_REMOTE_PLAY, NES_PAUSE);
    expect_call(AETHER_BT_REMOTE_STOP, NES_STOP);
    puts("PASS any unavailable snapshot blocks dispatch; backend command errors propagate without fallback or second owner");
}

static void test_multiple_owners(void)
{
    const aether_bluetooth_remote_action_t actions[] = {
        AETHER_BT_REMOTE_PLAY, AETHER_BT_REMOTE_PAUSE, AETHER_BT_REMOTE_STOP,
        AETHER_BT_REMOTE_NEXT, AETHER_BT_REMOTE_PREVIOUS,
    };
    for (unsigned owners = 0; owners < 8; ++owners) {
        if (owners == 0 || owners == 1 || owners == 2 || owners == 4) continue;
        for (unsigned starting = 0; starting < 2; ++starting) {
            reset();
            music.active = (owners & 1) && !starting;
            music.state = !(owners & 1) ? AETHER_AUDIO_STOPPED :
                starting ? AETHER_AUDIO_STARTING : AETHER_AUDIO_PAUSED;
            movie.state = !(owners & 2) ? AETHER_VIDEO_STOPPED :
                starting ? AETHER_VIDEO_STARTING : AETHER_VIDEO_PAUSED;
            game.state = !(owners & 4) ? AETHER_NES_STOPPED :
                starting ? AETHER_NES_STARTING : AETHER_NES_PAUSED;
            for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
                expect_rejection(actions[i], ESP_ERR_INVALID_STATE);
        }
    }
    reset();
    expect_rejection(AETHER_BT_REMOTE_VOLUME, ESP_ERR_NOT_SUPPORTED);
    expect_rejection((aether_bluetooth_remote_action_t)999, ESP_ERR_NOT_SUPPORTED);
    puts("PASS every dual/triple owner combination rejects commands, including STARTING and paused owners");
}

int main(void)
{
    test_flash_gate();
    test_music_and_idle();
    test_movie_and_game();
    test_snapshot_and_command_failures();
    test_multiple_owners();
    puts("All production media-control dispatcher regressions passed.");
    return 0;
}
