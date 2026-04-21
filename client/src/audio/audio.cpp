// audio.cpp — miniaudio integration.
// Sound files live in data/audio/sfx/*.wav  and  data/audio/music/*.ogg
// Missing files are silently skipped — audio degrades gracefully.

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio.h"
#include <cstdio>
#include <array>
#include <string>

namespace rco::audio {

// ---------------------------------------------------------------------------
// File paths
// ---------------------------------------------------------------------------

static const char* kSfxPaths[] = {
    "data/audio/sfx/sword_hit.wav",    // SwordHit
    "data/audio/sfx/fireball.wav",     // SpellFire
    "data/audio/sfx/heal.wav",         // SpellHeal
    "data/audio/sfx/lightning.wav",    // SpellLight
    "data/audio/sfx/npc_death.wav",    // NPCDeath
    "data/audio/sfx/player_death.wav", // PlayerDeath
    "data/audio/sfx/level_up.wav",     // LevelUp
    "data/audio/sfx/pickup.wav",       // PickupItem
    "data/audio/sfx/portal.wav",       // Portal
    "data/audio/sfx/buy.wav",          // BuyItem
    "data/audio/sfx/sell.wav",         // SellItem
};
static_assert(sizeof(kSfxPaths)/sizeof(*kSfxPaths) == static_cast<int>(SfxId::Count),
              "kSfxPaths length mismatch");

static const char* kMusicPaths[] = {
    nullptr,                               // 0 = stop
    "data/audio/music/starter_zone.ogg",   // 1
    "data/audio/music/forest.ogg",         // 2
    "data/audio/music/combat.ogg",         // 3
};
static constexpr int kMusicCount = static_cast<int>(sizeof(kMusicPaths)/sizeof(*kMusicPaths));

// ---------------------------------------------------------------------------
// AudioImpl — owns the ma_engine and current music sound
// ---------------------------------------------------------------------------

struct AudioImpl {
    ma_engine engine;
    ma_sound  music;
    bool      musicLoaded = false;
};

// ---------------------------------------------------------------------------
// AudioSystem
// ---------------------------------------------------------------------------

AudioSystem::AudioSystem()  = default;
AudioSystem::~AudioSystem() { Shutdown(); }

bool AudioSystem::Init(float masterVolume) {
    impl_ = std::make_unique<AudioImpl>();

    ma_engine_config cfg = ma_engine_config_init();
    if (ma_engine_init(&cfg, &impl_->engine) != MA_SUCCESS) {
        std::fprintf(stderr, "[audio] Failed to init miniaudio engine\n");
        impl_.reset();
        return false;
    }

    ma_engine_set_volume(&impl_->engine, masterVolume);
    ready_ = true;
    return true;
}

void AudioSystem::Shutdown() {
    if (!impl_) return;
    StopMusic();
    ma_engine_uninit(&impl_->engine);
    impl_.reset();
    ready_ = false;
}

void AudioSystem::PlaySfx(SfxId id, float volume) {
    if (!ready_) return;
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= static_cast<int>(SfxId::Count)) return;
    const char* path = kSfxPaths[idx];
    // ma_engine_play_sound is fire-and-forget; silently ignores missing files.
    ma_engine_play_sound(&impl_->engine, path, nullptr);
    (void)volume; // per-sound volume requires ma_sound; one-shot API uses master
}

void AudioSystem::PlaySfx(uint8_t rawId, float volume) {
    PlaySfx(static_cast<SfxId>(rawId), volume);
}

void AudioSystem::PlayMusic(uint8_t trackId, float volume) {
    if (!ready_) return;
    if (trackId == currentTrack_) return;

    StopMusic();
    currentTrack_ = trackId;

    if (trackId == 0 || trackId >= kMusicCount) return;
    const char* path = kMusicPaths[trackId];
    if (!path) return;

    // MA_SOUND_FLAG_STREAM: don't load entire file into RAM.
    ma_result r = ma_sound_init_from_file(&impl_->engine, path,
                                          MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_ASYNC,
                                          nullptr, nullptr, &impl_->music);
    if (r != MA_SUCCESS) {
        std::fprintf(stderr, "[audio] Music not found: %s\n", path);
        return;
    }
    ma_sound_set_volume(&impl_->music, volume);
    ma_sound_set_looping(&impl_->music, MA_TRUE);
    ma_sound_start(&impl_->music);
    impl_->musicLoaded = true;
}

void AudioSystem::StopMusic() {
    if (!impl_ || !impl_->musicLoaded) return;
    ma_sound_stop(&impl_->music);
    ma_sound_uninit(&impl_->music);
    impl_->musicLoaded = false;
    currentTrack_      = 0;
}

void AudioSystem::SetMasterVolume(float v) {
    if (ready_) ma_engine_set_volume(&impl_->engine, v);
}

} // namespace rco::audio
