#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Short sounds made up at start: wooden knocks for moves, a bell for check, a chord at the end. They play on the
// default audio device, over each other; without a device, not at all.
class SoundBank {
public:
    enum class Effect { Move, Capture, Castle, Check, GameOver, Click, Count };

    SoundBank();
    ~SoundBank();
    SoundBank(const SoundBank&) = delete;
    SoundBank& operator=(const SoundBank&) = delete;

    void Play(Effect effect);
    bool Enabled() const { return m_enabled; }
    void SetEnabled(bool enabled);

private:
    struct Voice {
        const std::vector<float>* samples;
        std::size_t position;
    };

    static void Mix(void* bank, std::uint8_t* stream, int bytes);

    std::array<std::vector<float>, static_cast<std::size_t>(Effect::Count)> m_effects;
    std::vector<Voice> m_voices; // shared with the audio thread, under the device's lock
    std::uint32_t m_device = 0;  // SDL_AudioDeviceID
    bool m_enabled = true;
};
