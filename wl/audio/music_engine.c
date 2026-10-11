// SPDX-License-Identifier: MIT
// The music track engine over the audio engine (music_engine.h).
#include "wl/audio/music_engine.h"

#include <string.h>

#include "wl/platform/platform.h"

enum { ES_FREE = 0, ES_LOADING, ES_READY, ES_FAILED };

static WlMusicEntry *find(WlMusicEngine *e, WlMusicId id) {
    for (uint32_t i = 0; i < WL_MUSIC_ENGINE_ENTRIES; i++) {
        if (e->entry[i].state != ES_FREE && e->entry[i].id == id) {
            return &e->entry[i];
        }
    }
    return NULL;
}

static void evict(WlMusicEngine *e, WlMusicEntry *en) {
    if (e->loader.release != NULL) {
        e->loader.release(e->loader.user, en->id);
    }
    memset(en, 0, sizeof *en);
    e->stats.evictions++;
}

static bool evictable(const WlMusicEntry *en) {
    return en->state != ES_FREE && en->refs == 0 && !en->pinned;
}

// A slot for a new entry: a free one, else the oldest unreferenced one.
static WlMusicEntry *take_slot(WlMusicEngine *e) {
    WlMusicEntry *best = NULL;
    for (uint32_t i = 0; i < WL_MUSIC_ENGINE_ENTRIES; i++) {
        WlMusicEntry *en = &e->entry[i];
        if (en->state == ES_FREE) {
            return en;
        }
        if (evictable(en) && (best == NULL || en->used < best->used)) {
            best = en;
        }
    }
    if (best != NULL) {
        evict(e, best);
    }
    return best;
}

static WlResult be_prepare(void *user, WlMusicId id, WlMusicRole role) {
    WlMusicEngine *e = user;
    if (id == 0) {
        return WL_ERR_ARG;
    }
    WlMusicEntry *en = find(e, id);
    if (en == NULL) {
        en = take_slot(e);
        if (en == NULL) {
            return WL_ERR_LIMIT;
        }
        const WlResult r = e->loader.request != NULL ? e->loader.request(e->loader.user, id) : WL_ERR_STATE;
        if (r != WL_OK) {
            if (r != WL_ERR_LIMIT) {
                e->stats.load_failures++;
            }
            if (r == WL_ERR_LIMIT) {
                return r;
            }
            // Remember the refusal so an effect that is asked for every frame does not ask the loader every frame.
            memset(en, 0, sizeof *en);
            en->id = id;
            en->state = ES_FAILED;
            en->used = ++e->clock;
            en->pinned = role != WL_MUSIC_ROLE_BGM;
            return r;
        }
        memset(en, 0, sizeof *en);
        en->id = id;
        en->state = ES_LOADING;
        e->stats.loads++;
    }
    en->used = ++e->clock;
    if (role != WL_MUSIC_ROLE_BGM) {
        en->pinned = true;
    }
    if (en->state == ES_LOADING) {
        const WlResult r = e->loader.poll(e->loader.user, id, &en->track);
        if (r == WL_OK) {
            en->state = ES_READY;
        } else if (r != WL_ERR_AGAIN) {
            en->state = ES_FAILED;
            e->stats.load_failures++;
            return r;
        }
    }
    if (en->state == ES_FAILED) {
        return WL_ERR_NOT_FOUND;
    }
    return en->state == ES_READY ? WL_OK : WL_ERR_AGAIN;
}

static WlAudioPlay play_params(WlMusicRole role, uint32_t fade_in_ms, uint16_t gain_q8) {
    WlAudioPlay p = wl_audio_play_defaults(role == WL_MUSIC_ROLE_SFX ? WL_BUS_EFFECTS : role == WL_MUSIC_ROLE_UI ? WL_BUS_UI : WL_BUS_MUSIC);
    p.priority = role == WL_MUSIC_ROLE_JINGLE ? 100u : 64u;
    p.gain = gain_q8;
    p.fade_in_ms = fade_in_ms;
    return p;
}

static WlResult be_play(void *user, WlMusicId id, WlMusicRole role, uint32_t fade_in_ms, uint16_t gain_q8, uint32_t *handle) {
    WlMusicEngine *e = user;
    WlMusicEntry *en = find(e, id);
    if (en == NULL || en->state != ES_READY) {
        return WL_ERR_AGAIN;
    }
    WlMusicVoice *v = NULL;
    for (uint32_t i = 0; i < WL_MUSIC_ENGINE_VOICES; i++) {
        if (e->voice[i].handle == 0) {
            v = &e->voice[i];
            break;
        }
    }
    if (v == NULL) {
        return WL_ERR_LIMIT;
    }
    const WlAudioPlay p = play_params(role, fade_in_ms, gain_q8);
    WlAudioHandle h = 0;
    WlResult r = en->track.is_stream ? wl_audio_stream_play(&en->track.span, &p, &h) : wl_audio_seq_play(&en->track.song, NULL, &p, &h);
    if (r != WL_OK || h == 0) {
        e->stats.play_failures++;
        if (r == WL_ERR_NOMEM) {
            return WL_ERR_LIMIT;
        }
        return r == WL_OK ? WL_ERR_FAILED : r;
    }
    v->handle = h;
    v->entry = (uint16_t)(en - e->entry);
    en->refs++;
    en->used = ++e->clock;
    e->stats.plays++;
    *handle = h;
    return WL_OK;
}

static void be_stop(void *user, uint32_t h, uint32_t fade_ms) {
    (void)user;
    wl_audio_stop(h, fade_ms);
}

static bool be_pause(void *user, uint32_t h, bool paused) {
    (void)user;
    return wl_audio_pause(h, paused);
}

static void be_set_gain(void *user, uint32_t h, uint16_t gain) {
    (void)user;
    wl_audio_set(h, gain, 0, 0);
}

static bool be_alive(void *user, uint32_t h) {
    (void)user;
    const WlAudioState s = wl_audio_state(h);
    return s == WL_AUDIO_PLAYING || s == WL_AUDIO_BUFFERING;
}

static void be_duck(void *user, uint16_t factor) {
    (void)user;
    wl_audio_duck_music(factor);
}

// Voices that have left the mixer give their track back; unreferenced background tracks beyond the keep count are released.
static void reap(WlMusicEngine *e) {
    for (uint32_t i = 0; i < WL_MUSIC_ENGINE_VOICES; i++) {
        WlMusicVoice *v = &e->voice[i];
        if (v->handle != 0 && !be_alive(NULL, v->handle)) {
            WlMusicEntry *en = &e->entry[v->entry];
            if (en->refs > 0) {
                en->refs--;
            }
            v->handle = 0;
        }
    }
    for (;;) {
        uint32_t n = 0;
        WlMusicEntry *oldest = NULL;
        for (uint32_t i = 0; i < WL_MUSIC_ENGINE_ENTRIES; i++) {
            WlMusicEntry *en = &e->entry[i];
            if (evictable(en) && en->state != ES_LOADING) {
                n++;
                if (oldest == NULL || en->used < oldest->used) {
                    oldest = en;
                }
            }
        }
        if (n <= WL_MUSIC_ENGINE_KEEP || oldest == NULL) {
            break;
        }
        evict(e, oldest);
    }
}

static void be_tick(void *user, uint32_t dt_ms) {
    (void)dt_ms;
    WlMusicEngine *e = user;
    if (e->loader.pump != NULL) {
        e->loader.pump(e->loader.user);
    }
    // Loading entries that finished while nobody asked: collected here so the next prepare is instant.
    for (uint32_t i = 0; i < WL_MUSIC_ENGINE_ENTRIES; i++) {
        WlMusicEntry *en = &e->entry[i];
        if (en->state == ES_LOADING) {
            const WlResult r = e->loader.poll(e->loader.user, en->id, &en->track);
            if (r == WL_OK) {
                en->state = ES_READY;
            } else if (r != WL_ERR_AGAIN) {
                en->state = ES_FAILED;
                e->stats.load_failures++;
            }
        }
    }
    reap(e);
}

void wl_music_engine_init(WlMusicEngine *e, const WlMusicLoader *loader) {
    memset(e, 0, sizeof *e);
    if (loader != NULL) {
        e->loader = *loader;
    }
}

const WlMusicBackend *wl_music_engine_backend(WlMusicEngine *e, WlMusicBackend *out) {
    memset(out, 0, sizeof *out);
    out->prepare = be_prepare;
    out->play = be_play;
    out->stop = be_stop;
    out->pause = be_pause;
    out->set_gain = be_set_gain;
    out->alive = be_alive;
    out->duck = be_duck;
    out->tick = be_tick;
    out->user = e;
    return out;
}

void wl_music_engine_usage(const WlMusicEngine *e, uint32_t *entries, uint32_t *voices) {
    uint32_t n = 0, v = 0;
    for (uint32_t i = 0; i < WL_MUSIC_ENGINE_ENTRIES; i++) {
        n += e->entry[i].state != ES_FREE ? 1u : 0u;
    }
    for (uint32_t i = 0; i < WL_MUSIC_ENGINE_VOICES; i++) {
        v += e->voice[i].handle != 0 ? 1u : 0u;
    }
    if (entries != NULL) {
        *entries = n;
    }
    if (voices != NULL) {
        *voices = v;
    }
}

void wl_music_engine_shutdown(WlMusicEngine *e) {
    for (uint32_t i = 0; i < WL_MUSIC_ENGINE_VOICES; i++) {
        if (e->voice[i].handle != 0) {
            wl_audio_stop(e->voice[i].handle, 0);
        }
    }
    for (uint32_t t = 0; t < 100; t++) {
        bool any = false;
        for (uint32_t i = 0; i < WL_MUSIC_ENGINE_VOICES; i++) {
            if (e->voice[i].handle != 0 && be_alive(NULL, e->voice[i].handle)) {
                any = true;
            }
        }
        if (!any) {
            break;
        }
        wl_audio_pump(NULL, NULL);
        wl_platform_thread_sleep_ms(5);
    }
    for (uint32_t i = 0; i < WL_MUSIC_ENGINE_ENTRIES; i++) {
        WlMusicEntry *en = &e->entry[i];
        if (en->state != ES_FREE) {
            en->refs = 0;
            evict(e, en);
        }
    }
    memset(e->voice, 0, sizeof e->voice);
}
