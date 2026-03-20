#include "gbc_internal.h"

#include <string.h>

static float pyemu_gbc_pulse_duty(uint8_t duty) {
    static const float duty_cycles[4] = {0.125f, 0.25f, 0.5f, 0.75f};
    return duty_cycles[duty & 0x03];
}

static uint16_t pyemu_gbc_channel_frequency_raw(const pyemu_gbc_system* gbc, int channel) {
    if (channel == 1) {
        return (uint16_t)(gbc->memory[0xFF13] | ((uint16_t)(gbc->memory[0xFF14] & 0x07) << 8));
    }
    return (uint16_t)(gbc->memory[0xFF18] | ((uint16_t)(gbc->memory[0xFF19] & 0x07) << 8));
}

static uint16_t pyemu_gbc_wave_frequency_raw(const pyemu_gbc_system* gbc) {
    return (uint16_t)(gbc->memory[0xFF1D] | ((uint16_t)(gbc->memory[0xFF1E] & 0x07) << 8));
}

static float pyemu_gbc_wave_volume_scale(uint8_t volume_code) {
    static const float scales[4] = {0.0f, 1.0f, 0.5f, 0.25f};
    return scales[volume_code & 0x03];
}

static float pyemu_gbc_noise_frequency(const pyemu_gbc_noise_channel* noise) {
    static const float divisors[8] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
    float divisor = divisors[noise->divisor_code & 0x07];
    float scale = (float)(1u << ((noise->clock_shift & 0x0F) + 1));
    return 524288.0f / divisor / scale;
}

static uint8_t pyemu_gbc_wave_sample(const pyemu_gbc_system* gbc, float phase) {
    uint32_t position = ((uint32_t)(phase * 32.0f)) & 31u;
    uint8_t packed = gbc->memory[0xFF30 + (position >> 1)];
    if ((position & 1u) == 0u) {
        return (uint8_t)(packed >> 4);
    }
    return (uint8_t)(packed & 0x0F);
}

static void pyemu_gbc_update_noise_state_for_frame(pyemu_gbc_system* gbc) {
    pyemu_gbc_noise_channel* noise = &gbc->noise;

    if (!noise->enabled) {
        return;
    }

    if (noise->length_enabled && noise->length_counter > 0) {
        noise->length_cycles += PYEMU_GBC_CYCLES_PER_FRAME;
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
        noise->envelope_cycles += PYEMU_GBC_CYCLES_PER_FRAME;
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

static void pyemu_gbc_trigger_pulse(pyemu_gbc_system* gbc, int channel) {
    pyemu_gbc_pulse_channel* pulse = channel == 1 ? &gbc->pulse1 : &gbc->pulse2;
    uint8_t length_reg = channel == 1 ? gbc->memory[0xFF11] : gbc->memory[0xFF16];
    uint8_t envelope = channel == 1 ? gbc->memory[0xFF12] : gbc->memory[0xFF17];

    if ((gbc->memory[0xFF26] & 0x80) == 0) {
        pulse->enabled = 0;
        return;
    }

    pulse->duty = (uint8_t)(length_reg >> 6);
    pulse->volume = (uint8_t)((envelope >> 4) & 0x0F);
    pulse->frequency_raw = pyemu_gbc_channel_frequency_raw(gbc, channel);
    pulse->enabled = pulse->volume > 0 ? 1 : 0;
    pulse->phase = 0.0f;
}

static void pyemu_gbc_trigger_wave(pyemu_gbc_system* gbc) {
    if ((gbc->memory[0xFF26] & 0x80) == 0) {
        gbc->wave.enabled = 0;
        return;
    }

    gbc->wave.volume_code = (uint8_t)((gbc->memory[0xFF1C] >> 5) & 0x03);
    gbc->wave.frequency_raw = pyemu_gbc_wave_frequency_raw(gbc);
    gbc->wave.enabled = (uint8_t)(((gbc->memory[0xFF1A] & 0x80) != 0) && gbc->wave.volume_code != 0);
    gbc->wave.phase = 0.0f;
}

static void pyemu_gbc_trigger_noise(pyemu_gbc_system* gbc) {
    uint8_t nr42;
    uint8_t nr43;
    uint8_t length_value;
    if ((gbc->memory[0xFF26] & 0x80) == 0) {
        gbc->noise.enabled = 0;
        return;
    }

    nr42 = gbc->memory[0xFF21];
    nr43 = gbc->memory[0xFF22];
    length_value = (uint8_t)(gbc->memory[0xFF20] & 0x3F);
    gbc->noise.volume = (uint8_t)((nr42 >> 4) & 0x0F);
    gbc->noise.envelope_increase = (uint8_t)((nr42 >> 3) & 0x01);
    gbc->noise.envelope_period = (uint8_t)(nr42 & 0x07);
    gbc->noise.width_mode = (uint8_t)((nr43 >> 3) & 0x01);
    gbc->noise.divisor_code = (uint8_t)(nr43 & 0x07);
    gbc->noise.clock_shift = (uint8_t)((nr43 >> 4) & 0x0F);
    gbc->noise.length_enabled = (uint8_t)((gbc->memory[0xFF23] >> 6) & 0x01);
    gbc->noise.length_counter = (uint8_t)(64 - length_value);
    if (gbc->noise.length_counter == 0) {
        gbc->noise.length_counter = 64;
    }
    gbc->noise.envelope_cycles = 0;
    gbc->noise.length_cycles = 0;
    gbc->noise.enabled = gbc->noise.volume > 0 ? 1 : 0;
    gbc->noise.lfsr = 0x7FFFu;
    gbc->noise.phase = 0.0f;
}

void pyemu_gbc_apu_handle_write(pyemu_gbc_system* gbc, uint16_t address, uint8_t value) {
    switch (address) {
        case 0xFF11:
            gbc->pulse1.duty = (uint8_t)(value >> 6);
            break;
        case 0xFF12:
            gbc->pulse1.volume = (uint8_t)((value >> 4) & 0x0F);
            if (gbc->pulse1.volume == 0) gbc->pulse1.enabled = 0;
            break;
        case 0xFF13:
            gbc->pulse1.frequency_raw = pyemu_gbc_channel_frequency_raw(gbc, 1);
            break;
        case 0xFF14:
            gbc->pulse1.frequency_raw = pyemu_gbc_channel_frequency_raw(gbc, 1);
            if (value & 0x80) {
                pyemu_gbc_trigger_pulse(gbc, 1);
            }
            break;
        case 0xFF16:
            gbc->pulse2.duty = (uint8_t)(value >> 6);
            break;
        case 0xFF17:
            gbc->pulse2.volume = (uint8_t)((value >> 4) & 0x0F);
            if (gbc->pulse2.volume == 0) gbc->pulse2.enabled = 0;
            break;
        case 0xFF18:
            gbc->pulse2.frequency_raw = pyemu_gbc_channel_frequency_raw(gbc, 2);
            break;
        case 0xFF19:
            gbc->pulse2.frequency_raw = pyemu_gbc_channel_frequency_raw(gbc, 2);
            if (value & 0x80) {
                pyemu_gbc_trigger_pulse(gbc, 2);
            }
            break;
        case 0xFF1A:
            if ((value & 0x80) == 0) {
                gbc->wave.enabled = 0;
            }
            break;
        case 0xFF1C:
            gbc->wave.volume_code = (uint8_t)((value >> 5) & 0x03);
            break;
        case 0xFF1D:
            gbc->wave.frequency_raw = pyemu_gbc_wave_frequency_raw(gbc);
            break;
        case 0xFF1E:
            gbc->wave.frequency_raw = pyemu_gbc_wave_frequency_raw(gbc);
            if (value & 0x80) {
                pyemu_gbc_trigger_wave(gbc);
            }
            break;
        case 0xFF21:
            gbc->noise.volume = (uint8_t)((value >> 4) & 0x0F);
            gbc->noise.envelope_increase = (uint8_t)((value >> 3) & 0x01);
            gbc->noise.envelope_period = (uint8_t)(value & 0x07);
            if (gbc->noise.volume == 0 && !gbc->noise.envelope_increase) gbc->noise.enabled = 0;
            break;
        case 0xFF22:
            gbc->noise.divisor_code = (uint8_t)(value & 0x07);
            gbc->noise.width_mode = (uint8_t)((value >> 3) & 0x01);
            gbc->noise.clock_shift = (uint8_t)((value >> 4) & 0x0F);
            break;
        case 0xFF23:
            gbc->noise.length_enabled = (uint8_t)((value >> 6) & 0x01);
            if (value & 0x80) {
                pyemu_gbc_trigger_noise(gbc);
            }
            break;
        case 0xFF26:
            if ((value & 0x80) == 0) {
                gbc->pulse1.enabled = 0;
                gbc->pulse2.enabled = 0;
                gbc->wave.enabled = 0;
                gbc->noise.enabled = 0;
            }
            break;
        default:
            break;
    }
}

void pyemu_gbc_update_audio_frame(pyemu_gbc_system* gbc) {
    size_t index;
    float left_master;
    float right_master;
    uint8_t nr50 = gbc->memory[0xFF24];
    uint8_t nr51 = gbc->memory[0xFF25];
    uint8_t nr52 = gbc->memory[0xFF26];

    if (!gbc->rom_loaded || (nr52 & 0x80) == 0) {
        memset(gbc->audio_frame, 0, sizeof(gbc->audio_frame));
        memset(gbc->audio_channels, 0, sizeof(gbc->audio_channels));
        return;
    }

    gbc->pulse1.duty = (uint8_t)(gbc->memory[0xFF11] >> 6);
    gbc->pulse1.volume = (uint8_t)((gbc->memory[0xFF12] >> 4) & 0x0F);
    gbc->pulse1.frequency_raw = pyemu_gbc_channel_frequency_raw(gbc, 1);
    if ((gbc->memory[0xFF12] & 0xF8) == 0 || gbc->pulse1.volume == 0) {
        gbc->pulse1.enabled = 0;
    }

    gbc->pulse2.duty = (uint8_t)(gbc->memory[0xFF16] >> 6);
    gbc->pulse2.volume = (uint8_t)((gbc->memory[0xFF17] >> 4) & 0x0F);
    gbc->pulse2.frequency_raw = pyemu_gbc_channel_frequency_raw(gbc, 2);
    if ((gbc->memory[0xFF17] & 0xF8) == 0 || gbc->pulse2.volume == 0) {
        gbc->pulse2.enabled = 0;
    }

    gbc->wave.volume_code = (uint8_t)((gbc->memory[0xFF1C] >> 5) & 0x03);
    gbc->wave.frequency_raw = pyemu_gbc_wave_frequency_raw(gbc);
    if ((gbc->memory[0xFF1A] & 0x80) == 0 || gbc->wave.volume_code == 0) {
        gbc->wave.enabled = 0;
    }

    gbc->noise.divisor_code = (uint8_t)(gbc->memory[0xFF22] & 0x07);
    gbc->noise.width_mode = (uint8_t)((gbc->memory[0xFF22] >> 3) & 0x01);
    gbc->noise.clock_shift = (uint8_t)((gbc->memory[0xFF22] >> 4) & 0x0F);
    gbc->noise.length_enabled = (uint8_t)((gbc->memory[0xFF23] >> 6) & 0x01);
    if ((gbc->memory[0xFF21] & 0xF8) == 0) {
        gbc->noise.enabled = 0;
    }
    if (gbc->noise.lfsr == 0) {
        gbc->noise.lfsr = 0x7FFFu;
    }
    pyemu_gbc_update_noise_state_for_frame(gbc);

    left_master = ((float)(((nr50 >> 4) & 0x07) + 1)) / 8.0f;
    right_master = ((float)((nr50 & 0x07) + 1)) / 8.0f;

    for (index = 0; index < PYEMU_GBC_AUDIO_SAMPLES_PER_FRAME; ++index) {
        float left = 0.0f;
        float right = 0.0f;
        int channel;

        for (channel = 0; channel < 2; ++channel) {
            pyemu_gbc_pulse_channel* pulse = channel == 0 ? &gbc->pulse1 : &gbc->pulse2;
            float sample;
            float duty;
            float frequency;
            uint8_t route_right;
            uint8_t route_left;

            if (!pulse->enabled || pulse->volume == 0 || pulse->frequency_raw >= 2048) {
                continue;
            }

            frequency = 131072.0f / (2048.0f - (float)pulse->frequency_raw);
            duty = pyemu_gbc_pulse_duty(pulse->duty);
            sample = pulse->phase < duty ? 1.0f : -1.0f;
            sample *= ((float)pulse->volume / 15.0f) * 0.10f;
            pulse->phase += frequency / (float)PYEMU_GBC_AUDIO_SAMPLE_RATE;
            while (pulse->phase >= 1.0f) {
                pulse->phase -= 1.0f;
            }

            route_right = (uint8_t)((nr51 >> channel) & 0x01);
            route_left = (uint8_t)((nr51 >> (channel + 4)) & 0x01);
            if (route_left) {
                float routed = sample * left_master;
                gbc->audio_channels[channel][index * 2] = (int16_t)(routed * 32767.0f);
                left += routed;
            }
            if (route_right) {
                float routed = sample * right_master;
                gbc->audio_channels[channel][index * 2 + 1] = (int16_t)(routed * 32767.0f);
                right += routed;
            }
        }

        if (gbc->wave.enabled && gbc->wave.frequency_raw < 2048) {
            float frequency = 65536.0f / (2048.0f - (float)gbc->wave.frequency_raw);
            float sample = (((float)pyemu_gbc_wave_sample(gbc, gbc->wave.phase) / 15.0f) * 2.0f - 1.0f) * pyemu_gbc_wave_volume_scale(gbc->wave.volume_code) * 0.10f;
            gbc->wave.phase += frequency / (float)PYEMU_GBC_AUDIO_SAMPLE_RATE;
            while (gbc->wave.phase >= 1.0f) {
                gbc->wave.phase -= 1.0f;
            }
            if (nr51 & 0x40) {
                float routed = sample * left_master;
                gbc->audio_channels[2][index * 2] = (int16_t)(routed * 32767.0f);
                left += routed;
            }
            if (nr51 & 0x04) {
                float routed = sample * right_master;
                gbc->audio_channels[2][index * 2 + 1] = (int16_t)(routed * 32767.0f);
                right += routed;
            }
        }

        if (gbc->noise.enabled && gbc->noise.volume > 0) {
            float frequency = pyemu_gbc_noise_frequency(&gbc->noise);
            float sample;
            gbc->noise.phase += frequency / (float)PYEMU_GBC_AUDIO_SAMPLE_RATE;
            while (gbc->noise.phase >= 1.0f) {
                uint16_t feedback = (uint16_t)((gbc->noise.lfsr ^ (gbc->noise.lfsr >> 1)) & 0x01);
                gbc->noise.lfsr = (uint16_t)((gbc->noise.lfsr >> 1) | (feedback << 14));
                if (gbc->noise.width_mode) {
                    gbc->noise.lfsr = (uint16_t)((gbc->noise.lfsr & ~0x40u) | (feedback << 6));
                }
                gbc->noise.phase -= 1.0f;
            }
            sample = ((gbc->noise.lfsr & 0x01u) == 0u ? 1.0f : -1.0f) * ((float)gbc->noise.volume / 15.0f) * 0.08f;
            if (nr51 & 0x80) {
                float routed = sample * left_master;
                gbc->audio_channels[3][index * 2] = (int16_t)(routed * 32767.0f);
                left += routed;
            }
            if (nr51 & 0x08) {
                float routed = sample * right_master;
                gbc->audio_channels[3][index * 2 + 1] = (int16_t)(routed * 32767.0f);
                right += routed;
            }
        }

        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;
        gbc->audio_frame[index * 2] = (int16_t)(left * 32767.0f);
        gbc->audio_frame[index * 2 + 1] = (int16_t)(right * 32767.0f);
    }
}

void pyemu_gbc_get_audio_buffer(const pyemu_system* system, pyemu_audio_buffer* out_audio) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    if (out_audio == NULL) {
        return;
    }
    out_audio->sample_rate = PYEMU_GBC_AUDIO_SAMPLE_RATE;
    out_audio->channels = PYEMU_GBC_AUDIO_CHANNELS;
    out_audio->samples = gbc->audio_frame;
    out_audio->sample_count = PYEMU_GBC_AUDIO_SAMPLE_COUNT;
}

pyemu_audio_buffer pyemu_gbc_get_audio_channel_buffer(const pyemu_system* system, int channel) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    pyemu_audio_buffer out_audio;
    memset(&out_audio, 0, sizeof(out_audio));
    if (gbc == NULL || channel < 1 || channel > 4) {
        return out_audio;
    }
    out_audio.sample_rate = PYEMU_GBC_AUDIO_SAMPLE_RATE;
    out_audio.channels = PYEMU_GBC_AUDIO_CHANNELS;
    out_audio.samples = gbc->audio_channels[channel - 1];
    out_audio.sample_count = PYEMU_GBC_AUDIO_SAMPLE_COUNT;
    return out_audio;
}

void pyemu_gbc_get_audio_debug_info(const pyemu_system* system, pyemu_gameboy_audio_debug_info* out_info) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    if (gbc == NULL || out_info == NULL) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->nr10 = gbc->memory[0xFF10];
    out_info->nr11 = gbc->memory[0xFF11];
    out_info->nr12 = gbc->memory[0xFF12];
    out_info->nr13 = gbc->memory[0xFF13];
    out_info->nr14 = gbc->memory[0xFF14];
    out_info->nr21 = gbc->memory[0xFF16];
    out_info->nr22 = gbc->memory[0xFF17];
    out_info->nr23 = gbc->memory[0xFF18];
    out_info->nr24 = gbc->memory[0xFF19];
    out_info->nr30 = gbc->memory[0xFF1A];
    out_info->nr32 = gbc->memory[0xFF1C];
    out_info->nr33 = gbc->memory[0xFF1D];
    out_info->nr34 = gbc->memory[0xFF1E];
    out_info->nr41 = gbc->memory[0xFF20];
    out_info->nr42 = gbc->memory[0xFF21];
    out_info->nr43 = gbc->memory[0xFF22];
    out_info->nr44 = gbc->memory[0xFF23];
    out_info->nr50 = gbc->memory[0xFF24];
    out_info->nr51 = gbc->memory[0xFF25];
    out_info->nr52 = gbc->memory[0xFF26];
    out_info->ch1_enabled = gbc->pulse1.enabled;
    out_info->ch1_duty = gbc->pulse1.duty;
    out_info->ch1_volume = gbc->pulse1.volume;
    out_info->ch1_frequency_raw = gbc->pulse1.frequency_raw;
    out_info->ch2_enabled = gbc->pulse2.enabled;
    out_info->ch2_duty = gbc->pulse2.duty;
    out_info->ch2_volume = gbc->pulse2.volume;
    out_info->ch2_frequency_raw = gbc->pulse2.frequency_raw;
    out_info->ch3_enabled = gbc->wave.enabled;
    out_info->ch3_volume_code = gbc->wave.volume_code;
    out_info->ch3_frequency_raw = gbc->wave.frequency_raw;
    out_info->ch4_enabled = gbc->noise.enabled;
    out_info->ch4_volume = gbc->noise.volume;
    out_info->ch4_width_mode = gbc->noise.width_mode;
    out_info->ch4_divisor_code = gbc->noise.divisor_code;
    out_info->ch4_clock_shift = gbc->noise.clock_shift;
    out_info->ch4_lfsr = gbc->noise.lfsr;
}
