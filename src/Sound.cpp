#include "Sound.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>
#include <utility>

namespace {

constexpr int Rate = 44100;
constexpr double TwoPi = 2 * std::numbers::pi;
constexpr std::size_t MaxVoices = 16;

using Samples = std::vector<float>;

Samples Silence(double seconds) { return Samples(static_cast<std::size_t>(seconds * Rate), 0.0f); }

void Add(Samples& target, const Samples& source, double at, float gain = 1)
{
    const auto offset = static_cast<std::size_t>(at * Rate);
    if (target.size() < offset + source.size())
        target.resize(offset + source.size(), 0.0f);
    for (std::size_t i = 0; i < source.size(); ++i)
        target[offset + i] += source[i] * gain;
}

// A knock of wood on wood: a short burst of filtered noise and quickly damped resonances.
Samples Knock(double pitch, std::uint32_t seed)
{
    Samples samples = Silence(0.16);
    std::minstd_rand random(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    float filtered = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double t = static_cast<double>(i) / Rate;
        filtered += (noise(random) - filtered) * 0.35f;
        const double burst = filtered * std::exp(-t / 0.006);
        const double body = 0.55 * std::sin(TwoPi * 620 * pitch * t) * std::exp(-t / 0.028) + 0.3 * std::sin(TwoPi * 1480 * pitch * t) * std::exp(-t / 0.012) +
                            0.45 * std::sin(TwoPi * 170 * pitch * t) * std::exp(-t / 0.045);
        const double attack = std::min(1.0, t / 0.0008);
        samples[i] = static_cast<float>((burst * 0.9 + body) * attack);
    }
    return samples;
}

// A small bell: inharmonic partials dying away at their own rates.
Samples Bell(double frequency, double length)
{
    Samples samples = Silence(length);
    constexpr double partials[4][3] = {{1.0, 1.0, 1.0}, {2.0, 0.45, 0.6}, {2.76, 0.3, 0.4}, {5.4, 0.12, 0.25}};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double t = static_cast<double>(i) / Rate;
        double value = 0;
        for (const auto& [ratio, gain, decay] : partials)
            value += gain * std::sin(TwoPi * frequency * ratio * t) * std::exp(-t / (length * 0.35 * decay));
        samples[i] = static_cast<float>(value * std::min(1.0, t / 0.004));
    }
    return samples;
}

// Scaled so that its loudest sample reaches `volume`.
Samples Normalized(Samples samples, float volume)
{
    float peak = 1e-6f;
    for (const float sample : samples)
        peak = std::max(peak, std::abs(sample));
    for (float& sample : samples)
        sample = sample / peak * volume;
    return samples;
}

} // namespace

SoundBank::SoundBank()
{
    auto effect = [this](Effect which) -> Samples& { return m_effects[std::to_underlying(which)]; };
    effect(Effect::Move) = Normalized(Knock(1.0, 1), 0.55f);

    Samples capture = Knock(1.15, 2);
    Add(capture, Knock(0.9, 3), 0.05, 0.8f);
    effect(Effect::Capture) = Normalized(std::move(capture), 0.7f);

    Samples castle = Knock(1.0, 4);
    Add(castle, Knock(1.08, 5), 0.13, 0.9f);
    effect(Effect::Castle) = Normalized(std::move(castle), 0.6f);
    effect(Effect::Check) = Normalized(Bell(1046.5, 0.9), 0.3f);

    Samples chord = Silence(1.8);
    constexpr double notes[4] = {523.25, 659.25, 783.99, 1046.5};
    for (int i = 0; i < 4; ++i)
        Add(chord, Bell(notes[i], 1.5), i * 0.11, 0.8f);
    effect(Effect::GameOver) = Normalized(std::move(chord), 0.35f);

    Samples click = Silence(0.03);
    for (std::size_t i = 0; i < click.size(); ++i) {
        const double t = static_cast<double>(i) / Rate;
        click[i] = static_cast<float>(std::sin(TwoPi * 2600 * t) * std::exp(-t / 0.004));
    }
    effect(Effect::Click) = Normalized(std::move(click), 0.18f);
    m_voices.reserve(MaxVoices);

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "3D Chess: no sound: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec wanted{};
    wanted.freq = Rate;
    wanted.format = AUDIO_F32SYS;
    wanted.channels = 1;
    wanted.samples = 512; // about 12 ms
    wanted.callback = &SoundBank::Mix;
    wanted.userdata = this;
    // SDL converts the samples to whatever the device takes.
    m_device = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
    if (m_device == 0) {
        std::fprintf(stderr, "3D Chess: no sound: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    SDL_PauseAudioDevice(m_device, 0);
}

SoundBank::~SoundBank()
{
    if (m_device != 0) {
        SDL_CloseAudioDevice(m_device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void SoundBank::Play(Effect effect)
{
    if (!m_enabled || m_device == 0)
        return;
    SDL_LockAudioDevice(m_device);
    if (m_voices.size() >= MaxVoices)
        m_voices.erase(m_voices.begin());
    m_voices.push_back({&m_effects[std::to_underlying(effect)], 0});
    SDL_UnlockAudioDevice(m_device);
}

void SoundBank::SetEnabled(bool enabled)
{
    m_enabled = enabled;
    if (!enabled && m_device != 0) {
        SDL_LockAudioDevice(m_device);
        m_voices.clear();
        SDL_UnlockAudioDevice(m_device);
    }
}

// On the audio thread, with the device locked.
void SoundBank::Mix(void* bank, std::uint8_t* stream, int bytes)
{
    auto& voices = static_cast<SoundBank*>(bank)->m_voices;
    const auto count = static_cast<std::size_t>(bytes) / sizeof(float);
    float* out = reinterpret_cast<float*>(stream);
    std::fill_n(out, count, 0.0f);
    for (Voice& voice : voices) {
        const std::size_t length = std::min(count, voice.samples->size() - voice.position);
        for (std::size_t i = 0; i < length; ++i)
            out[i] += (*voice.samples)[voice.position + i];
        voice.position += length;
    }
    std::erase_if(voices, [](const Voice& voice) { return voice.position >= voice.samples->size(); });
    for (std::size_t i = 0; i < count; ++i)
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
}
