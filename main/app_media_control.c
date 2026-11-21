#include "app_media_control.h"

static bool video_owns(const aether_video_status_t *s)
{
    return s->state == AETHER_VIDEO_STARTING || s->state == AETHER_VIDEO_PLAYING ||
           s->state == AETHER_VIDEO_PAUSED;
}

static bool nes_owns(const aether_nes_status_t *s)
{
    return s->state == AETHER_NES_STARTING || s->state == AETHER_NES_RUNNING ||
           s->state == AETHER_NES_PAUSED;
}

bool app_media_control_flash_busy(aether_audio_player_handle_t audio,
    aether_video_player_handle_t video, aether_nes_handle_t nes)
{
    aether_audio_player_status_t a = {0};
    if (audio && (aether_audio_player_get_status(audio, &a) != ESP_OK ||
        a.state == AETHER_AUDIO_STARTING ||
        (a.active && a.state != AETHER_AUDIO_PAUSED))) return true;
    aether_video_status_t v = {0};
    if (video && (aether_video_player_get_status(video, &v) != ESP_OK ||
        v.state == AETHER_VIDEO_STARTING || v.state == AETHER_VIDEO_PLAYING)) return true;
    aether_nes_status_t n = {0};
    return nes && (aether_nes_get_status(nes, &n) != ESP_OK ||
        n.state == AETHER_NES_STARTING || n.state == AETHER_NES_RUNNING);
}

esp_err_t app_media_control_dispatch(aether_audio_player_handle_t audio,
    aether_video_player_handle_t video, aether_nes_handle_t nes,
    aether_bluetooth_remote_action_t action)
{
    aether_audio_player_status_t a = {0};
    aether_video_status_t v = {0};
    aether_nes_status_t n = {0};
    esp_err_t error;
    if (audio && (error = aether_audio_player_get_status(audio, &a)) != ESP_OK) return error;
    if (video && (error = aether_video_player_get_status(video, &v)) != ESP_OK) return error;
    if (nes && (error = aether_nes_get_status(nes, &n)) != ESP_OK) return error;
    const bool music = audio && (a.active || a.state == AETHER_AUDIO_STARTING);
    const bool movie = video && video_owns(&v);
    const bool game = nes && nes_owns(&n);
    if ((unsigned)music + (unsigned)movie + (unsigned)game > 1) return ESP_ERR_INVALID_STATE;
    if (movie) {
        if (action == AETHER_BT_REMOTE_STOP) return aether_video_player_stop(video);
        if (v.state == AETHER_VIDEO_STARTING) return ESP_ERR_INVALID_STATE;
        if (action == AETHER_BT_REMOTE_PLAY || action == AETHER_BT_REMOTE_PAUSE)
            return aether_video_player_set_paused(video, action == AETHER_BT_REMOTE_PAUSE);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (game) {
        if (action == AETHER_BT_REMOTE_STOP) return aether_nes_stop(nes);
        if (n.state == AETHER_NES_STARTING) return ESP_ERR_INVALID_STATE;
        if (action == AETHER_BT_REMOTE_PLAY || action == AETHER_BT_REMOTE_PAUSE)
            return aether_nes_set_paused(nes, action == AETHER_BT_REMOTE_PAUSE);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!audio) return action == AETHER_BT_REMOTE_STOP ? ESP_OK : ESP_ERR_NOT_FOUND;
    if (action == AETHER_BT_REMOTE_STOP) return aether_audio_player_stop(audio);
    if (action == AETHER_BT_REMOTE_NEXT || action == AETHER_BT_REMOTE_PREVIOUS)
        return aether_audio_player_next(audio, action == AETHER_BT_REMOTE_NEXT ? 1 : -1);
    if (action != AETHER_BT_REMOTE_PLAY && action != AETHER_BT_REMOTE_PAUSE) return ESP_ERR_NOT_SUPPORTED;
    if (a.state == AETHER_AUDIO_STARTING) return ESP_ERR_INVALID_STATE;
    if (music) return aether_audio_player_set_paused(audio, action == AETHER_BT_REMOTE_PAUSE);
    if (action == AETHER_BT_REMOTE_PLAY && a.path[0]) return aether_audio_player_play(audio, a.path, a.volume);
    return ESP_ERR_INVALID_STATE;
}
