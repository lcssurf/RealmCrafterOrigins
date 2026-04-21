#pragma once

#include <cstdint>
#include <memory>

namespace rco::audio {

// Sound effect IDs — must match server PSound payload values.
enum class SfxId : uint8_t {
    SwordHit    = 0,
    SpellFire   = 1,
    SpellHeal   = 2,
    SpellLight  = 3,
    NPCDeath    = 4,
    PlayerDeath = 5,
    LevelUp     = 6,
    PickupItem  = 7,
    Portal      = 8,
    BuyItem     = 9,
    SellItem    = 10,
    Count
};

struct AudioImpl; // defined in audio.cpp — keeps miniaudio out of the header

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();

    // Returns false if miniaudio engine failed to initialize (audio disabled gracefully).
    bool Init(float masterVolume = 0.7f);
    void Shutdown();

    void PlaySfx(SfxId id, float volume = 1.0f);
    void PlaySfx(uint8_t rawId, float volume = 1.0f);

    // trackId: 1=StarterZone 2=Forest 3=Combat; 0=stop
    void PlayMusic(uint8_t trackId, float volume = 0.5f);
    void StopMusic();

    void SetMasterVolume(float v);

    bool IsReady() const { return ready_; }

private:
    std::unique_ptr<AudioImpl> impl_;
    bool    ready_        = false;
    uint8_t currentTrack_ = 0;
};

} // namespace rco::audio
