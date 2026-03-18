#include "gameboy_internal.h"

#include <string.h>

static float pyemu_gameboy_pulse_duty(uint8_t duty) {
    static const float duty_cycles[4] = {0.125f, 0.25f, 0.5f, 0.75f};
    return duty_cycles[duty & 0x03];
}

static uint16_t pyemu_gameboy_channel_frequency_raw(const pyemu_gameboy_system* gb, int channel) {
    if (channel == 1) {
        return (uint16_t)(gb->memory[0xFF13] | ((uint16_t)(gb->memory[0xFF14] & 0x07) << 8));
    }
    return (uint16_t)(gb->memory[0xFF18] | ((uint16_t)(gb->memory[0xFF19] & 0x07) << 8));
}

static uint16_t pyemu_gameboy_wave_frequency_raw(const pyemu_gameboy_system* gb) {
    return (uint16_t)(gb->memory[0xFF1D] | ((uint16_t)(gb->memory[0xFF1E] & 0x07) << 8));
}

static float pyemu_gameboy_wave_volume_scale(uint8_t volume_code) {
    static const float scales[4] = {0.0f, 1.0f, 0.5f, 0.25f};
    return scales[volume_code & 0x03];
}

static float pyemu_gameboy_noise_frequency(const pyemu_gameboy_noise_channel* noise) {
    static const float divisors[8] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
    float divisor = divisors[noise->divisor_code & 0x07];
    float scale = (float)(1u << ((noise->clock_shift & 0x0F) + 1));
    return 524288.0f / divisor / scale;
}

static uint8_t pyemu_gameboy_wave_sample(const pyemu_gameboy_system* gb, float phase) {
    uint32_t position = ((uint32_t)(phase * 32.0f)) & 31u;
    uint8_t packed = gb->memory[0xFF30 + (position >> 1)];
    if ((position & 1u) == 0u) {
        return (uint8_t)(packed >> 4);
    }
    return (uint8_t)(packed & 0x0F);
}

static void pyemu_gameboy_update_noise_state_for_frame(pyemu_gameboy_system* gb) {
    pyemu_gameboy_noise_channel* noise = &gb->noise;

    if (!noise->enabled) {
        return;
    }

    if (noise->length_enabled && noise->length_counter > 0) {
        noise->length_cycles += PYEMU_GAMEBOY_CYCLES_PER_FRAME;
        while (noise->length_cycles >= 16384U && noise->enabled) {
            noise->length_cycles -= 16384U;
            if (noise->length_counter > 0) {
                noise->length_counter = (uint8_t)(noise->length_counter - 1);
                if (noise->length_counter == 0) {
                    noise->enabled = 0;
                }
            }
        }
    }

    if (noise->enabled && noise->envelope_period > 0) {
        noise->envelope_cycles += PYEMU_GAMEBOY_CYCLES_PER_FRAME;
        while (noise->envelope_cycles >= 65536U && noise->enabled) {
            noise->envelope_cycles -= 65536U;
            if (noise->envelope_increase) {
                if (noise->volume < 15) {
                    noise->volume = (uint8_t)(noise->volume + 1);
                }
            } else if (noise->volume > 0) {
                noise->volume = (uint8_t)(noise->volume - 1);
                if (noise->volume == 0) {
                    noise->enabled = 0;
                }
            }
        }
    }
}

static void pyemu_gameboy_trigger_pulse(pyemu_gameboy_system* gb, int channel) {
    pyemu_gameboy_pulse_channel* pulse = channel == 1 ? &gb->pulse1 : &gb->pulse2;
    uint8_t length_reg = channel == 1 ? gb->memory[0xFF11] : gb->memory[0xFF16];
    uint8_t envelope = channel == 1 ? gb->memory[0xFF12] : gb->memory[0xFF17];

    if ((gb->memory[0xFF26] & 0x80) == 0) {
        pulse->enabled = 0;
        return;
    }

    pulse->duty = (uint8_t)(length_reg >> 6);
    pulse->volume = (uint8_t)((envelope >> 4) & 0x0F);
    pulse->frequency_raw = pyemu_gameboy_channel_frequency_raw(gb, channel);
    pulse->enabled = pulse->volume > 0 ? 1 : 0;
    pulse->phase = 0.0f;
}

static void pyemu_gameboy_trigger_wave(pyemu_gameboy_system* gb) {
    if ((gb->memory[0xFF26] & 0x80) == 0) {
        gb->wave.enabled = 0;
        return;
    }

    gb->wave.volume_code = (uint8_t)((gb->memory[0xFF1C] >> 5) & 0x03);
    gb->wave.frequency_raw = pyemu_gameboy_wave_frequency_raw(gb);
    gb->wave.enabled = (uint8_t)(((gb->memory[0xFF1A] & 0x80) != 0) && gb->wave.volume_code != 0);
    gb->wave.phase = 0.0f;
}

static void pyemu_gameboy_trigger_noise(pyemu_gameboy_system* gb) {
    uint8_t nr42;
    uint8_t nr43;
    uint8_t length_value;
    if ((gb->memory[0xFF26] & 0x80) == 0) {
        gb->noise.enabled = 0;
        return;
    }

    nr42 = gb->memory[0xFF21];
    nr43 = gb->memory[0xFF22];
    length_value = (uint8_t)(gb->memory[0xFF20] & 0x3F);
    gb->noise.volume = (uint8_t)((nr42 >> 4) & 0x0F);
    gb->noise.envelope_increase = (uint8_t)((nr42 >> 3) & 0x01);
    gb->noise.envelope_period = (uint8_t)(nr42 & 0x07);
    gb->noise.width_mode = (uint8_t)((nr43 >> 3) & 0x01);
    gb->noise.divisor_code = (uint8_t)(nr43 & 0x07);
    gb->noise.clock_shift = (uint8_t)((nr43 >> 4) & 0x0F);
    gb->noise.length_enabled = (uint8_t)((gb->memory[0xFF23] >> 6) & 0x01);
    gb->noise.length_counter = (uint8_t)(64 - length_value);
    if (gb->noise.length_counter == 0) {
        gb->noise.length_counter = 64;
    }
    gb->noise.envelope_cycles = 0;
    gb->noise.length_cycles = 0;
    gb->noise.enabled = gb->noise.volume > 0 ? 1 : 0;
    gb->noise.lfsr = 0x7FFFu;
    gb->noise.phase = 0.0f;
}

void pyemu_gameboy_apu_handle_write(pyemu_gameboy_system* gb, uint16_t address, uint8_t value) {
    switch (address) {
        case 0xFF11:
            gb->pulse1.duty = (uint8_t)(value >> 6);
            break;
        case 0xFF12:
            gb->pulse1.volume = (uint8_t)((value >> 4) & 0x0F);
            if (gb->pulse1.volume == 0) gb->pulse1.enabled = 0;
            break;
        case 0xFF13:
            gb->pulse1.frequency_raw = pyemu_gameboy_channel_frequency_raw(gb, 1);
            break;
        case 0xFF14:
            gb->pulse1.frequency_raw = pyemu_gameboy_channel_frequency_raw(gb, 1);
            if (value & 0x80) {
                pyemu_gameboy_trigger_pulse(gb, 1);
            }
            break;
        case 0xFF16:
            gb->pulse2.duty = (uint8_t)(value >> 6);
            break;
        case 0xFF17:
            gb->pulse2.volume = (uint8_t)((value >> 4) & 0x0F);
            if (gb->pulse2.volume == 0) gb->pulse2.enabled = 0;
            break;
        case 0xFF18:
            gb->pulse2.frequency_raw = pyemu_gameboy_channel_frequency_raw(gb, 2);
            break;
        case 0xFF19:
            gb->pulse2.frequency_raw = pyemu_gameboy_channel_frequency_raw(gb, 2);
            if (value & 0x80) {
                pyemu_gameboy_trigger_pulse(gb, 2);
            }
            break;
        case 0xFF1A:
            if ((value & 0x80) == 0) {
                gb->wave.enabled = 0;
            }
            break;
        case 0xFF1C:
            gb->wave.volume_code = (uint8_t)((value >> 5) & 0x03);
            break;
        case 0xFF1D:
            gb->wave.frequency_raw = pyemu_gameboy_wave_frequency_raw(gb);
            break;
        case 0xFF1E:
            gb->wave.frequency_raw = pyemu_gameboy_wave_frequency_raw(gb);
            if (value & 0x80) {
                pyemu_gameboy_trigger_wave(gb);
            }
            break;
        case 0xFF21:
            gb->noise.volume = (uint8_t)((value >> 4) & 0x0F);
            gb->noise.envelope_increase = (uint8_t)((value >> 3) & 0x01);
            gb->noise.envelope_period = (uint8_t)(value & 0x07);
            if (gb->noise.volume == 0 && !gb->noise.envelope_increase) gb->noise.enabled = 0;
            break;
        case 0xFF22:
            gb->noise.divisor_code = (uint8_t)(value & 0x07);
            gb->noise.width_mode = (uint8_t)((value >> 3) & 0x01);
            gb->noise.clock_shift = (uint8_t)((value >> 4) & 0x0F);
            break;
        case 0xFF23:
            gb->noise.length_enabled = (uint8_t)((value >> 6) & 0x01);
            if (value & 0x80) {
                pyemu_gameboy_trigger_noise(gb);
            }
            break;
        case 0xFF26:
            if ((value & 0x80) == 0) {
                gb->pulse1.enabled = 0;
                gb->pulse2.enabled = 0;
                gb->wave.enabled = 0;
                gb->noise.enabled = 0;
            }
            break;
        default:
            break;
    }
}

void pyemu_gameboy_update_audio_frame(pyemu_gameboy_system* gb) {
    size_t index;
    float left_master;
    float right_master;
    uint8_t nr50 = gb->memory[0xFF24];
    uint8_t nr51 = gb->memory[0xFF25];
    uint8_t nr52 = gb->memory[0xFF26];

    if (!gb->rom_loaded || (nr52 & 0x80) == 0) {
        memset(gb->audio_frame, 0, sizeof(gb->audio_frame));
        memset(gb->audio_channels, 0, sizeof(gb->audio_channels));
        return;
    }

    gb->pulse1.duty = (uint8_t)(gb->memory[0xFF11] >> 6);
    gb->pulse1.volume = (uint8_t)((gb->memory[0xFF12] >> 4) & 0x0F);
    gb->pulse1.frequency_raw = pyemu_gameboy_channel_frequency_raw(gb, 1);
    if ((gb->memory[0xFF12] & 0xF8) == 0 || gb->pulse1.volume == 0) {
        gb->pulse1.enabled = 0;
    }

    gb->pulse2.duty = (uint8_t)(gb->memory[0xFF16] >> 6);
    gb->pulse2.volume = (uint8_t)((gb->memory[0xFF17] >> 4) & 0x0F);
    gb->pulse2.frequency_raw = pyemu_gameboy_channel_frequency_raw(gb, 2);
    if ((gb->memory[0xFF17] & 0xF8) == 0 || gb->pulse2.volume == 0) {
        gb->pulse2.enabled = 0;
    }

    gb->wave.volume_code = (uint8_t)((gb->memory[0xFF1C] >> 5) & 0x03);
    gb->wave.frequency_raw = pyemu_gameboy_wave_frequency_raw(gb);
    if ((gb->memory[0xFF1A] & 0x80) == 0 || gb->wave.volume_code == 0) {
        gb->wave.enabled = 0;
    }

    gb->noise.divisor_code = (uint8_t)(gb->memory[0xFF22] & 0x07);
    gb->noise.width_mode = (uint8_t)((gb->memory[0xFF22] >> 3) & 0x01);
    gb->noise.clock_shift = (uint8_t)((gb->memory[0xFF22] >> 4) & 0x0F);
    gb->noise.length_enabled = (uint8_t)((gb->memory[0xFF23] >> 6) & 0x01);
    if ((gb->memory[0xFF21] & 0xF8) == 0) {
        gb->noise.enabled = 0;
    }
    if (gb->noise.lfsr == 0) {
        gb->noise.lfsr = 0x7FFFu;
    }
    pyemu_gameboy_update_noise_state_for_frame(gb);

    left_master = ((float)(((nr50 >> 4) & 0x07) + 1)) / 8.0f;
    right_master = ((float)((nr50 & 0x07) + 1)) / 8.0f;

    for (index = 0; index < PYEMU_GAMEBOY_AUDIO_SAMPLES_PER_FRAME; ++index) {
        float left = 0.0f;
        float right = 0.0f;
        int channel;

        for (channel = 0; channel < 2; ++channel) {
            pyemu_gameboy_pulse_channel* pulse = channel == 0 ? &gb->pulse1 : &gb->pulse2;
            float sample;
            float duty;
            float frequency;
            uint8_t route_right;
            uint8_t route_left;

            if (!pulse->enabled || pulse->volume == 0 || pulse->frequency_raw >= 2048) {
                continue;
            }

            frequency = 131072.0f / (2048.0f - (float)pulse->frequency_raw);
            duty = pyemu_gameboy_pulse_duty(pulse->duty);
            sample = pulse->phase < duty ? 1.0f : -1.0f;
            sample *= ((float)pulse->volume / 15.0f) * 0.10f;
            pulse->phase += frequency / (float)PYEMU_GAMEBOY_AUDIO_SAMPLE_RATE;
            while (pulse->phase >= 1.0f) {
                pulse->phase -= 1.0f;
            }

            route_right = (uint8_t)((nr51 >> channel) & 0x01);
            route_left = (uint8_t)((nr51 >> (channel + 4)) & 0x01);
            if (route_left) {
                float routed = sample * left_master;
                gb->audio_channels[channel][index * 2] = (int16_t)(routed * 32767.0f);
                left += routed;
            }
            if (route_right) {
                float routed = sample * right_master;
                gb->audio_channels[channel][index * 2 + 1] = (int16_t)(routed * 32767.0f);
                right += routed;
            }
        }

        if (gb->wave.enabled && gb->wave.frequency_raw < 2048) {
            float frequency = 65536.0f / (2048.0f - (float)gb->wave.frequency_raw);
            float sample = (((float)pyemu_gameboy_wave_sample(gb, gb->wave.phase) / 15.0f) * 2.0f - 1.0f) * pyemu_gameboy_wave_volume_scale(gb->wave.volume_code) * 0.10f;
            gb->wave.phase += frequency / (float)PYEMU_GAMEBOY_AUDIO_SAMPLE_RATE;
            while (gb->wave.phase >= 1.0f) {
                gb->wave.phase -= 1.0f;
            }
            if (nr51 & 0x40) {
                float routed = sample * left_master;
                gb->audio_channels[2][index * 2] = (int16_t)(routed * 32767.0f);
                left += routed;
            }
            if (nr51 & 0x04) {
                float routed = sample * right_master;
                gb->audio_channels[2][index * 2 + 1] = (int16_t)(routed * 32767.0f);
                right += routed;
            }
        }

        if (gb->noise.enabled && gb->noise.volume > 0) {
            float frequency = pyemu_gameboy_noise_frequency(&gb->noise);
            float sample;
            gb->noise.phase += frequency / (float)PYEMU_GAMEBOY_AUDIO_SAMPLE_RATE;
            while (gb->noise.phase >= 1.0f) {
                uint16_t feedback = (uint16_t)((gb->noise.lfsr ^ (gb->noise.lfsr >> 1)) & 0x01);
                gb->noise.lfsr = (uint16_t)((gb->noise.lfsr >> 1) | (feedback << 14));
                if (gb->noise.width_mode) {
                    gb->noise.lfsr = (uint16_t)((gb->noise.lfsr & ~0x40u) | (feedback << 6));
                }
                gb->noise.phase -= 1.0f;
            }
            sample = ((gb->noise.lfsr & 0x01u) == 0u ? 1.0f : -1.0f) * ((float)gb->noise.volume / 15.0f) * 0.08f;
            if (nr51 & 0x80) {
                float routed = sample * left_master;
                gb->audio_channels[3][index * 2] = (int16_t)(routed * 32767.0f);
                left += routed;
            }
            if (nr51 & 0x08) {
                float routed = sample * right_master;
                gb->audio_channels[3][index * 2 + 1] = (int16_t)(routed * 32767.0f);
                right += routed;
            }
        }

        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;
        gb->audio_frame[index * 2] = (int16_t)(left * 32767.0f);
        gb->audio_frame[index * 2 + 1] = (int16_t)(right * 32767.0f);
    }
}

void pyemu_gameboy_get_audio_buffer(const pyemu_system* system, pyemu_audio_buffer* out_audio) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (out_audio == NULL) {
        return;
    }
    out_audio->sample_rate = PYEMU_GAMEBOY_AUDIO_SAMPLE_RATE;
    out_audio->channels = PYEMU_GAMEBOY_AUDIO_CHANNELS;
    out_audio->samples = gb->audio_frame;
    out_audio->sample_count = PYEMU_GAMEBOY_AUDIO_SAMPLE_COUNT;
}

pyemu_audio_buffer pyemu_gameboy_get_audio_channel_buffer(const pyemu_system* system, int channel) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    pyemu_audio_buffer out_audio;
    memset(&out_audio, 0, sizeof(out_audio));
    if (gb == NULL || channel < 1 || channel > 4) {
        return out_audio;
    }
    out_audio.sample_rate = PYEMU_GAMEBOY_AUDIO_SAMPLE_RATE;
    out_audio.channels = PYEMU_GAMEBOY_AUDIO_CHANNELS;
    out_audio.samples = gb->audio_channels[channel - 1];
    out_audio.sample_count = PYEMU_GAMEBOY_AUDIO_SAMPLE_COUNT;
    return out_audio;
}

void pyemu_gameboy_get_audio_debug_info(const pyemu_system* system, pyemu_gameboy_audio_debug_info* out_info) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (gb == NULL || out_info == NULL) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->nr10 = gb->memory[0xFF10];
    out_info->nr11 = gb->memory[0xFF11];
    out_info->nr12 = gb->memory[0xFF12];
    out_info->nr13 = gb->memory[0xFF13];
    out_info->nr14 = gb->memory[0xFF14];
    out_info->nr21 = gb->memory[0xFF16];
    out_info->nr22 = gb->memory[0xFF17];
    out_info->nr23 = gb->memory[0xFF18];
    out_info->nr24 = gb->memory[0xFF19];
    out_info->nr30 = gb->memory[0xFF1A];
    out_info->nr32 = gb->memory[0xFF1C];
    out_info->nr33 = gb->memory[0xFF1D];
    out_info->nr34 = gb->memory[0xFF1E];
    out_info->nr41 = gb->memory[0xFF20];
    out_info->nr42 = gb->memory[0xFF21];
    out_info->nr43 = gb->memory[0xFF22];
    out_info->nr44 = gb->memory[0xFF23];
    out_info->nr50 = gb->memory[0xFF24];
    out_info->nr51 = gb->memory[0xFF25];
    out_info->nr52 = gb->memory[0xFF26];
    out_info->ch1_enabled = gb->pulse1.enabled;
    out_info->ch1_duty = gb->pulse1.duty;
    out_info->ch1_volume = gb->pulse1.volume;
    out_info->ch1_frequency_raw = gb->pulse1.frequency_raw;
    out_info->ch2_enabled = gb->pulse2.enabled;
    out_info->ch2_duty = gb->pulse2.duty;
    out_info->ch2_volume = gb->pulse2.volume;
    out_info->ch2_frequency_raw = gb->pulse2.frequency_raw;
    out_info->ch3_enabled = gb->wave.enabled;
    out_info->ch3_volume_code = gb->wave.volume_code;
    out_info->ch3_frequency_raw = gb->wave.frequency_raw;
    out_info->ch4_enabled = gb->noise.enabled;
    out_info->ch4_volume = gb->noise.volume;
    out_info->ch4_width_mode = gb->noise.width_mode;
    out_info->ch4_divisor_code = gb->noise.divisor_code;
    out_info->ch4_clock_shift = gb->noise.clock_shift;
    out_info->ch4_lfsr = gb->noise.lfsr;
}
