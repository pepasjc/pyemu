#include "gbc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void pyemu_gbc_step_instruction_internal(pyemu_gbc_system* gbc, int render_frame);
static uint8_t pyemu_gbc_normalize_wram_bank(uint8_t value) {
    uint8_t bank = (uint8_t)(value & 0x07);
    return bank == 0 ? 1 : bank;
}

static void pyemu_gbc_run_hdma(pyemu_gbc_system* gbc, uint8_t value) {
    uint16_t source = (uint16_t)(((uint16_t)gbc->memory[PYEMU_GBC_HDMA1] << 8) | (gbc->memory[PYEMU_GBC_HDMA2] & 0xF0));
    uint16_t dest = (uint16_t)(0x8000 | (((uint16_t)gbc->memory[PYEMU_GBC_HDMA3] & 0x1F) << 8) | (gbc->memory[PYEMU_GBC_HDMA4] & 0xF0));
    uint16_t length = (uint16_t)((((value & 0x7F) + 1) & 0x7F) * 0x10);
    uint16_t index;

    for (index = 0; index < length; ++index) {
        uint16_t dst = (uint16_t)(dest + index);
        uint8_t copied = pyemu_gbc_peek_memory(gbc, (uint16_t)(source + index));
        if (dst >= 0x8000 && dst <= 0x9FFF) {
            gbc->vram[(dst - 0x8000) + (gbc->current_vbk * 0x2000)] = copied;
            gbc->memory[dst] = copied;
        }
    }

    source = (uint16_t)(source + length);
    dest = (uint16_t)(dest + length);
    gbc->memory[PYEMU_GBC_HDMA1] = (uint8_t)(source >> 8);
    gbc->memory[PYEMU_GBC_HDMA2] = (uint8_t)(source & 0xF0);
    gbc->memory[PYEMU_GBC_HDMA3] = (uint8_t)((dest >> 8) & 0x1F);
    gbc->memory[PYEMU_GBC_HDMA4] = (uint8_t)(dest & 0xF0);
    gbc->memory[PYEMU_GBC_HDMA5] = 0xFF;
}

static uint8_t pyemu_gbc_wram_value(const pyemu_gbc_system* gbc, uint16_t address) {
    if (address >= 0xC000 && address <= 0xCFFF) {
        return gbc->wram[address - 0xC000];
    }
    if (address >= 0xD000 && address <= 0xDFFF) {
        size_t offset = (size_t)gbc->current_wram_bank * PYEMU_GBC_WRAM_BANK_SIZE + (size_t)(address - 0xD000);
        return gbc->wram[offset];
    }
    if (address >= 0xE000 && address <= 0xEFFF) {
        return gbc->wram[address - 0xE000];
    }
    if (address >= 0xF000 && address <= 0xFDFF) {
        size_t offset = (size_t)gbc->current_wram_bank * PYEMU_GBC_WRAM_BANK_SIZE + (size_t)(address - 0xF000);
        return gbc->wram[offset];
    }
    return gbc->memory[address];
}

static void pyemu_gbc_get_cartridge_debug_info(const pyemu_system* system, pyemu_cartridge_debug_info* out_info);
static int pyemu_gbc_save_state(const pyemu_system* system, const char* path);
static int pyemu_gbc_load_state(pyemu_system* system, const char* path);
static int pyemu_gbc_execute_hotpath(pyemu_gbc_system* gbc, int* out_cycles);
static uint8_t pyemu_gbc_current_bank_for_address(const pyemu_gbc_system* gbc, uint16_t address);
static pyemu_gbc_block_cache_entry* pyemu_gbc_get_block_cache_entry(pyemu_gbc_system* gbc, uint16_t pc, uint8_t bank);
static int pyemu_gbc_decode_block(pyemu_gbc_system* gbc, pyemu_gbc_block_cache_entry* entry, uint16_t pc, uint8_t bank);
static int pyemu_gbc_execute_block(pyemu_gbc_system* gbc, const pyemu_gbc_block_cache_entry* entry, int* out_cycles);
static int pyemu_gbc_execute_opcode(pyemu_gbc_system* gbc, uint8_t opcode);
static const char* pyemu_gbc_name(const pyemu_system* system);
static void pyemu_gbc_reset(pyemu_system* system);
static int pyemu_gbc_load_rom(pyemu_system* system, const char* path);
static void pyemu_gbc_step_instruction(pyemu_system* system);
static void pyemu_gbc_step_frame(pyemu_system* system);
static void pyemu_gbc_destroy(pyemu_system* system);

static int pyemu_gbc_is_tracked_access(uint16_t address) {
    if (address < 0x8000) {
        return 1;
    }
    if ((address >= 0x8000 && address <= 0x9FFF) || (address >= 0xFE00 && address <= 0xFE9F)) {
        return 1;
    }
    return address >= 0xFF00;
}

static void pyemu_gbc_record_access(pyemu_gbc_system* gbc, uint16_t address, uint8_t value, int is_write) {
    if (!gbc->bus_tracking_enabled || !pyemu_gbc_is_tracked_access(address)) {
        return;
    }
    gbc->last_access.address = address;
    gbc->last_access.value = value;
    gbc->last_access.is_write = (uint8_t)(is_write ? 1 : 0);
    gbc->last_access.valid = 1;
    if (is_write && address < 0x8000) {
        gbc->last_mapper_access = gbc->last_access;
    }
}

static void pyemu_gbc_sync_memory(pyemu_gbc_system* gbc) {
    pyemu_gbc_refresh_rom_mapping(gbc);
    memcpy(gbc->memory + 0x0000, gbc->rom_bank0, sizeof(gbc->rom_bank0));
    memcpy(gbc->memory + 0x4000, gbc->rom_bankx, sizeof(gbc->rom_bankx));
    memcpy(gbc->memory + 0x8000, gbc->vram + (gbc->current_vbk * 0x2000), 0x2000);
    pyemu_gbc_refresh_eram_window(gbc);
    memcpy(gbc->memory + 0xC000, gbc->wram, PYEMU_GBC_WRAM_BANK_SIZE);
    memcpy(gbc->memory + 0xD000, gbc->wram + ((size_t)gbc->current_wram_bank * PYEMU_GBC_WRAM_BANK_SIZE), PYEMU_GBC_WRAM_BANK_SIZE);
    memcpy(gbc->memory + 0xE000, gbc->wram, PYEMU_GBC_WRAM_BANK_SIZE);
    memcpy(gbc->memory + 0xF000, gbc->wram + ((size_t)gbc->current_wram_bank * PYEMU_GBC_WRAM_BANK_SIZE), PYEMU_GBC_WRAM_BANK_SIZE - 0x200);
    memcpy(gbc->memory + 0xFF80, gbc->hram, sizeof(gbc->hram));
    gbc->memory[PYEMU_GBC_SVBK] = (uint8_t)(0xF8 | gbc->current_wram_bank);
}

uint8_t pyemu_gbc_peek_memory(const pyemu_gbc_system* gbc, uint16_t address) {
    if (address == 0xFF00) {
        return pyemu_gbc_current_joypad_value(gbc);
    }
    if (address == PYEMU_GBC_BG_PALETTE_INDEX) {
        return gbc->bg_palette_index;
    }
    if (address == PYEMU_GBC_BG_PALETTE_DATA) {
        uint8_t palette_offset = (uint8_t)(gbc->bg_palette_index & 0x3F);
        uint8_t palette_idx = (uint8_t)(palette_offset >> 3);
        uint8_t byte_idx = (uint8_t)(palette_offset & 0x07);
        return palette_idx < 8 ? gbc->bg_palettes[palette_idx].data[byte_idx] : 0xFF;
    }
    if (address == PYEMU_GBC_SPRITE_PALETTE_INDEX) {
        return gbc->sp_palette_index;
    }
    if (address == PYEMU_GBC_SPRITE_PALETTE_DATA) {
        uint8_t palette_offset = (uint8_t)(gbc->sp_palette_index & 0x3F);
        uint8_t palette_idx = (uint8_t)(palette_offset >> 3);
        uint8_t byte_idx = (uint8_t)(palette_offset & 0x07);
        return palette_idx < 8 ? gbc->sp_palettes[palette_idx].data[byte_idx] : 0xFF;
    }
    if (address >= 0xA000 && address <= 0xBFFF) {
        if (!gbc->ram_enabled || !pyemu_gbc_has_external_ram(gbc)) {
            return 0xFF;
        }
        /* MBC3 RTC register read */
        if (pyemu_gbc_uses_mbc3(gbc) && gbc->mbc3_ram_bank >= 0x08 && gbc->mbc3_ram_bank <= 0x0C) {
            switch (gbc->mbc3_ram_bank) {
                case 0x08: return gbc->rtc_latch_s;
                case 0x09: return gbc->rtc_latch_m;
                case 0x0A: return gbc->rtc_latch_h;
                case 0x0B: return gbc->rtc_latch_dl;
                case 0x0C: return gbc->rtc_latch_dh;
                default:   return 0xFF;
            }
        }
        size_t offset = pyemu_gbc_current_eram_offset(gbc) + (size_t)(address - 0xA000);
        return offset < gbc->eram_size ? gbc->eram[offset] : 0xFF;
    }
    if (address == PYEMU_GBC_VBK) {
        return (uint8_t)(0xFE | gbc->current_vbk);
    }
    if (address == PYEMU_GBC_SVBK) {
        return (uint8_t)(0xF8 | gbc->current_wram_bank);
    }
    if ((address >= 0xC000 && address <= 0xDFFF) || (address >= 0xE000 && address <= 0xFDFF)) {
        return pyemu_gbc_wram_value(gbc, address);
    }
    return gbc->memory[address];
}

uint8_t pyemu_gbc_read_memory(const pyemu_gbc_system* gbc, uint16_t address) {
    uint8_t value;

    if (address >= 0x8000 && address <= 0x9FFF && !pyemu_gbc_cpu_can_access_vram(gbc)) {
        value = 0xFF;
    } else if (address >= 0xFE00 && address <= 0xFE9F && !pyemu_gbc_cpu_can_access_oam(gbc)) {
        value = 0xFF;
    } else {
        value = pyemu_gbc_peek_memory(gbc, address);
    }

    pyemu_gbc_record_access((pyemu_gbc_system*)gbc, address, value, 0);
    return value;
}

void pyemu_gbc_write_memory(pyemu_gbc_system* gbc, uint16_t address, uint8_t value) {
    if (address < 0x8000) {
        pyemu_gbc_record_access(gbc, address, value, 1);
        if (address <= 0x1FFF) {
            if (pyemu_gbc_uses_mbc1(gbc) || pyemu_gbc_uses_mbc3(gbc) || pyemu_gbc_uses_mbc5(gbc)) {
                gbc->ram_enabled = ((value & 0x0F) == 0x0A) ? 1 : 0;
            }
        } else if (address <= 0x2FFF) {
            if (pyemu_gbc_uses_mbc1(gbc)) {
                gbc->mbc1_rom_bank = pyemu_gbc_normalize_mbc1_bank(gbc, value);
            } else if (pyemu_gbc_uses_mbc3(gbc)) {
                gbc->mbc3_rom_bank = (uint8_t)(value & 0x7F);
                if (gbc->mbc3_rom_bank == 0) {
                    gbc->mbc3_rom_bank = 1;
                }
            } else if (pyemu_gbc_uses_mbc5(gbc)) {
                gbc->mbc3_rom_bank = value;
            }
            pyemu_gbc_refresh_rom_mapping(gbc);
            memcpy(gbc->memory + 0x0000, gbc->rom_bank0, sizeof(gbc->rom_bank0));
            memcpy(gbc->memory + 0x4000, gbc->rom_bankx, sizeof(gbc->rom_bankx));
        } else if (address <= 0x3FFF) {
            if (pyemu_gbc_uses_mbc5(gbc)) {
                gbc->mbc3_ram_bank = (uint8_t)(value & 0x01);
                pyemu_gbc_refresh_rom_mapping(gbc);
                memcpy(gbc->memory + 0x0000, gbc->rom_bank0, sizeof(gbc->rom_bank0));
                memcpy(gbc->memory + 0x4000, gbc->rom_bankx, sizeof(gbc->rom_bankx));
            }
        } else if (address <= 0x5FFF) {
            if (pyemu_gbc_uses_mbc1(gbc)) {
                gbc->mbc3_ram_bank = (uint8_t)(value & 0x03);
                pyemu_gbc_refresh_rom_mapping(gbc);
                memcpy(gbc->memory + 0x0000, gbc->rom_bank0, sizeof(gbc->rom_bank0));
                memcpy(gbc->memory + 0x4000, gbc->rom_bankx, sizeof(gbc->rom_bankx));
                pyemu_gbc_refresh_eram_window(gbc);
            } else if (pyemu_gbc_uses_mbc3(gbc)) {
                if (value <= 0x03) {
                    /* RAM bank select (0-3) */
                    if (gbc->eram_bank_count > 0) {
                        gbc->mbc3_ram_bank = (uint8_t)(value & 0x03);
                        pyemu_gbc_refresh_eram_window(gbc);
                    }
                } else if (value >= 0x08 && value <= 0x0C) {
                    /* RTC register select */
                    gbc->mbc3_ram_bank = value;
                }
            } else if (pyemu_gbc_uses_mbc5(gbc)) {
                if (gbc->eram_bank_count > 0) {
                    gbc->mbc3_ram_bank = (uint8_t)(value & 0x0F);
                    pyemu_gbc_refresh_eram_window(gbc);
                }
            }
        } else if (address <= 0x7FFF) {
            if (pyemu_gbc_uses_mbc1(gbc)) {
                gbc->mbc3_rom_bank = (uint8_t)(value & 0x01);
                pyemu_gbc_refresh_rom_mapping(gbc);
                memcpy(gbc->memory + 0x0000, gbc->rom_bank0, sizeof(gbc->rom_bank0));
                memcpy(gbc->memory + 0x4000, gbc->rom_bankx, sizeof(gbc->rom_bankx));
                pyemu_gbc_refresh_eram_window(gbc);
            } else if (pyemu_gbc_uses_mbc3(gbc)) {
                /* RTC latch: write 0x00 then 0x01 to copy live RTC into latched regs */
                if (value == 0x00) {
                    gbc->rtc_latch_ready = 1;
                } else if (value == 0x01 && gbc->rtc_latch_ready) {
                    gbc->rtc_latch_s  = gbc->rtc_s;
                    gbc->rtc_latch_m  = gbc->rtc_m;
                    gbc->rtc_latch_h  = gbc->rtc_h;
                    gbc->rtc_latch_dl = gbc->rtc_dl;
                    gbc->rtc_latch_dh = gbc->rtc_dh;
                    gbc->rtc_latch_ready = 0;
                }
            }
        }
        return;
    }

    if (address >= 0x8000 && address <= 0x9FFF) {
        pyemu_gbc_record_access(gbc, address, value, 1);
        if (!pyemu_gbc_cpu_can_access_vram(gbc)) {
            return;
        }
        gbc->memory[address] = value;
        gbc->vram[address - 0x8000 + (gbc->current_vbk * 0x2000)] = value;
        return;
    }

    if (address >= 0xFE00 && address <= 0xFE9F) {
        pyemu_gbc_record_access(gbc, address, value, 1);
        if (!pyemu_gbc_cpu_can_access_oam(gbc)) {
            return;
        }
        gbc->memory[address] = value;
        return;
    }

    if (address >= 0xA000 && address <= 0xBFFF) {
        if (gbc->ram_enabled && pyemu_gbc_has_external_ram(gbc)) {
            /* MBC3 RTC register write */
            if (pyemu_gbc_uses_mbc3(gbc) && gbc->mbc3_ram_bank >= 0x08 && gbc->mbc3_ram_bank <= 0x0C) {
                switch (gbc->mbc3_ram_bank) {
                    case 0x08: gbc->rtc_s  = (uint8_t)(value & 0x3F); break;
                    case 0x09: gbc->rtc_m  = (uint8_t)(value & 0x3F); break;
                    case 0x0A: gbc->rtc_h  = (uint8_t)(value & 0x1F); break;
                    case 0x0B: gbc->rtc_dl = value; break;
                    case 0x0C: gbc->rtc_dh = (uint8_t)(value & 0xC1); break;
                }
            } else {
                size_t offset = pyemu_gbc_current_eram_offset(gbc) + (size_t)(address - 0xA000);
                if (offset < gbc->eram_size) {
                    gbc->eram[offset] = value;
                    gbc->memory[address] = value;
                    gbc->battery_dirty = 1;
                }
            }
        }
        pyemu_gbc_record_access(gbc, address, value, 1);
        return;
    }

    if (address == 0xFF00) {
        uint8_t previous_value = pyemu_gbc_current_joypad_value(gbc);
        gbc->memory[address] = (uint8_t)(0xC0 | (value & 0x30) | 0x0F);
        pyemu_gbc_refresh_joypad(gbc, previous_value);
        pyemu_gbc_record_access(gbc, address, gbc->memory[address], 1);
        return;
    }
    if (address == PYEMU_GBC_SPEED) {
        /* Bit 0 = prepare-speed-switch arm bit; bit 7 (read-only) = current speed.
           Writing only latches the arm bit; the actual switch happens on STOP. */
        uint8_t current_speed_bit = (uint8_t)(gbc->double_speed ? 0x80 : 0x00);
        gbc->memory[address] = (uint8_t)(current_speed_bit | (value & 0x01));
        pyemu_gbc_record_access(gbc, address, value, 1);
        return;
    }
    if (address == PYEMU_GBC_VBK) {
        gbc->current_vbk = (uint8_t)(value & 0x01);
        gbc->memory[address] = (uint8_t)(0xFE | (value & 0x01));
        pyemu_gbc_sync_memory(gbc);
        pyemu_gbc_record_access(gbc, address, gbc->memory[address], 1);
        return;
    }
    if (address == PYEMU_GBC_SVBK) {
        gbc->current_wram_bank = pyemu_gbc_normalize_wram_bank(value);
        gbc->memory[address] = (uint8_t)(0xF8 | gbc->current_wram_bank);
        pyemu_gbc_sync_memory(gbc);
        pyemu_gbc_record_access(gbc, address, gbc->memory[address], 1);
        return;
    }
    if (address == PYEMU_GBC_HDMA1) {
        gbc->memory[address] = value;
        pyemu_gbc_record_access(gbc, address, value, 1);
        return;
    }
    if (address == PYEMU_GBC_HDMA2) {
        gbc->memory[address] = (uint8_t)(value & 0xF0);
        pyemu_gbc_record_access(gbc, address, gbc->memory[address], 1);
        return;
    }
    if (address == PYEMU_GBC_HDMA3) {
        gbc->memory[address] = (uint8_t)(value & 0x1F);
        pyemu_gbc_record_access(gbc, address, gbc->memory[address], 1);
        return;
    }
    if (address == PYEMU_GBC_HDMA4) {
        gbc->memory[address] = (uint8_t)(value & 0xF0);
        pyemu_gbc_record_access(gbc, address, gbc->memory[address], 1);
        return;
    }
    if (address == PYEMU_GBC_HDMA5) {
        pyemu_gbc_run_hdma(gbc, value);
        pyemu_gbc_record_access(gbc, address, gbc->memory[address], 1);
        return;
    }
    if (address == PYEMU_GBC_BG_PALETTE_INDEX) {
        gbc->bg_palette_index = (uint8_t)(value & 0xBF);
        gbc->memory[address] = value;
        pyemu_gbc_record_access(gbc, address, value, 1);
        return;
    }
    if (address == PYEMU_GBC_BG_PALETTE_DATA) {
        uint8_t palette_offset = (uint8_t)(gbc->bg_palette_index & 0x3F);
        uint8_t palette_idx = (uint8_t)(palette_offset >> 3);
        uint8_t byte_idx = (uint8_t)(palette_offset & 0x07);
        gbc->bg_palette_data = value;
        gbc->memory[address] = value;
        if (palette_idx < 8) {
            gbc->bg_palettes[palette_idx].data[byte_idx] = value;
        }
        if (gbc->bg_palette_index & 0x80) {
            gbc->bg_palette_index = (uint8_t)(0x80 | ((palette_offset + 1) & 0x3F));
            gbc->memory[PYEMU_GBC_BG_PALETTE_INDEX] = gbc->bg_palette_index;
        }
        pyemu_gbc_record_access(gbc, address, value, 1);
        return;
    }
    if (address == PYEMU_GBC_SPRITE_PALETTE_INDEX) {
        gbc->sp_palette_index = (uint8_t)(value & 0xBF);
        gbc->memory[address] = value;
        pyemu_gbc_record_access(gbc, address, value, 1);
        return;
    }
    if (address == PYEMU_GBC_SPRITE_PALETTE_DATA) {
        uint8_t palette_offset = (uint8_t)(gbc->sp_palette_index & 0x3F);
        uint8_t palette_idx = (uint8_t)(palette_offset >> 3);
        uint8_t byte_idx = (uint8_t)(palette_offset & 0x07);
        gbc->sp_palette_data = value;
        gbc->memory[address] = value;
        if (palette_idx < 8) {
            gbc->sp_palettes[palette_idx].data[byte_idx] = value;
        }
        if (gbc->sp_palette_index & 0x80) {
            gbc->sp_palette_index = (uint8_t)(0x80 | ((palette_offset + 1) & 0x3F));
            gbc->memory[PYEMU_GBC_SPRITE_PALETTE_INDEX] = gbc->sp_palette_index;
        }
        pyemu_gbc_record_access(gbc, address, value, 1);
        return;
    }
    if (address == PYEMU_IO_DIV) {
        int old_signal = pyemu_gbc_timer_signal(gbc);
        gbc->div_counter = 0;
        gbc->memory[address] = 0;
        pyemu_gbc_apply_timer_edge(gbc, old_signal, pyemu_gbc_timer_signal(gbc));
        pyemu_gbc_record_access(gbc, address, 0, 1);
        return;
    }
    if (address == PYEMU_IO_LY) {
        gbc->ppu_counter = 0;
        gbc->memory[address] = 0;
        pyemu_gbc_update_stat(gbc);
        pyemu_gbc_record_access(gbc, address, 0, 1);
        return;
    }
    if (address == PYEMU_IO_TAC) {
        int old_signal = pyemu_gbc_timer_signal(gbc);
        gbc->memory[address] = (uint8_t)(value & 0x07);
        pyemu_gbc_apply_timer_edge(gbc, old_signal, pyemu_gbc_timer_signal(gbc));
        pyemu_gbc_record_access(gbc, address, gbc->memory[address], 1);
        return;
    }
    if (address == PYEMU_IO_IF) {
        gbc->memory[address] = (uint8_t)(0xE0 | (value & 0x1F));
        pyemu_gbc_record_access(gbc, address, gbc->memory[address], 1);
        return;
    }
    if (address == PYEMU_IO_STAT) {
        gbc->memory[address] = (uint8_t)((value & 0x78) | (gbc->memory[address] & 0x07));
        pyemu_gbc_update_stat(gbc);
        pyemu_gbc_record_access(gbc, address, gbc->memory[address], 1);
        return;
    }
    if (address == PYEMU_IO_LYC) {
        gbc->memory[address] = value;
        pyemu_gbc_update_stat(gbc);
        pyemu_gbc_record_access(gbc, address, gbc->memory[address], 1);
        return;
    }
    if (address == PYEMU_IO_LCDC) {
        uint8_t previous = gbc->memory[address];
        gbc->memory[address] = value;
        if ((previous & 0x80) && !(value & 0x80)) {
            gbc->ppu_counter = 0;
            gbc->memory[PYEMU_IO_LY] = 0;
            gbc->stat_irq_line = 0;
            gbc->stat_mode = 0;
            gbc->stat_coincidence = 0;
        } else if (!(previous & 0x80) && (value & 0x80)) {
            gbc->ppu_counter = 0;
            gbc->memory[PYEMU_IO_LY] = 0;
            gbc->stat_irq_line = 0;
            gbc->stat_mode = 2;
            gbc->stat_coincidence = 0;
        }
        pyemu_gbc_update_stat(gbc);
        pyemu_gbc_record_access(gbc, address, value, 1);
        return;
    }
    if (address == PYEMU_IO_DMA) {
        uint16_t source = (uint16_t)(value << 8);
        uint16_t index;
        gbc->memory[address] = value;
        pyemu_gbc_record_access(gbc, address, value, 1);
        for (index = 0; index < PYEMU_GBC_OAM_SIZE; ++index) {
            gbc->memory[0xFE00 + index] = pyemu_gbc_read_memory(gbc, (uint16_t)(source + index));
            pyemu_gbc_record_access(gbc, (uint16_t)(0xFE00 + index), gbc->memory[0xFE00 + index], 1);
        }
        return;
    }

    gbc->memory[address] = value;
    pyemu_gbc_record_access(gbc, address, value, 1);

    if (address >= 0xFF10 && address <= 0xFF26) {
        pyemu_gbc_apu_handle_write(gbc, address, value);
    }

    if (address >= 0xC000 && address <= 0xCFFF) {
        size_t offset = (size_t)(address - 0xC000);
        gbc->wram[offset] = value;
        gbc->memory[address] = value;
        gbc->memory[address + 0x2000] = value;
    } else if (address >= 0xD000 && address <= 0xDFFF) {
        size_t offset = (size_t)gbc->current_wram_bank * PYEMU_GBC_WRAM_BANK_SIZE + (size_t)(address - 0xD000);
        gbc->wram[offset] = value;
        gbc->memory[address] = value;
        if (address <= 0xDDFF) {
            gbc->memory[address + 0x2000] = value;
        }
    } else if (address >= 0xE000 && address <= 0xEFFF) {
        size_t offset = (size_t)(address - 0xE000);
        gbc->wram[offset] = value;
        gbc->memory[address] = value;
        gbc->memory[address - 0x2000] = value;
    } else if (address >= 0xF000 && address <= 0xFDFF) {
        size_t offset = (size_t)gbc->current_wram_bank * PYEMU_GBC_WRAM_BANK_SIZE + (size_t)(address - 0xF000);
        gbc->wram[offset] = value;
        gbc->memory[address] = value;
        gbc->memory[address - 0x2000] = value;
    } else if (address >= 0xFF80 && address <= 0xFFFE) {
        gbc->hram[address - 0xFF80] = value;
    }
}

static void pyemu_gbc_init_default_palettes(pyemu_gbc_system* gbc) {
    static const uint16_t dmg_like[4] = {0x7FFF, 0x56B5, 0x294A, 0x0000};
    int palette;
    int color;

    for (palette = 0; palette < 8; ++palette) {
        for (color = 0; color < 4; ++color) {
            uint16_t rgb15 = dmg_like[color];
            gbc->bg_palettes[palette].data[color * 2] = (uint8_t)(rgb15 & 0xFF);
            gbc->bg_palettes[palette].data[color * 2 + 1] = (uint8_t)(rgb15 >> 8);
            gbc->sp_palettes[palette].data[color * 2] = (uint8_t)(rgb15 & 0xFF);
            gbc->sp_palettes[palette].data[color * 2 + 1] = (uint8_t)(rgb15 >> 8);
        }
    }
}

static void pyemu_gbc_extract_title(pyemu_gbc_system* gbc) {
    size_t title_length = 0;
    size_t index;

    memset(gbc->cartridge_title, 0, sizeof(gbc->cartridge_title));
    for (index = PYEMU_GBC_TITLE_START; index <= PYEMU_GBC_TITLE_END; ++index) {
        uint8_t value = gbc->rom_bank0[index];
        if (value == 0) {
            break;
        }
        if (title_length + 1 >= sizeof(gbc->cartridge_title)) {
            break;
        }
        gbc->cartridge_title[title_length] = (char)value;
        title_length += 1;
    }

    if (title_length == 0) {
        strncpy(gbc->cartridge_title, "Unknown", sizeof(gbc->cartridge_title) - 1);
    }
}

static void pyemu_gbc_set_post_boot_state(pyemu_gbc_system* gbc) {
    /* On real CGB hardware the boot ROM sets A=0x11 before handing off to the
     * cartridge. Games use this to detect GBC mode and enable color features.
     * If the cartridge header at 0x0143 has the CGB flag (0x80 or 0xC0), set
     * A=0x11 so the game takes the CGB code path instead of the DMG/SGB path. */
    uint8_t cgb_flag = gbc->rom_bank0[0x0143];
    int is_cgb = (cgb_flag == 0x80 || cgb_flag == 0xC0);
    pyemu_gbc_set_af(gbc, is_cgb ? 0x11B0 : 0x01B0);
    pyemu_gbc_set_bc(gbc, is_cgb ? 0x0000 : 0x0013);
    pyemu_gbc_set_de(gbc, is_cgb ? 0xFF56 : 0x00D8);
    pyemu_gbc_set_hl(gbc, is_cgb ? 0x000D : 0x014D);
    gbc->cpu.sp = 0xFFFE;
    gbc->cpu.pc = 0x0100;
    gbc->cpu.halted = 0;
    gbc->faulted = 0;
    gbc->ime = 0;
    gbc->ime_pending = 0;
    gbc->cycle_count = 0;
    gbc->div_counter = 0;
    gbc->timer_counter = 0;
    gbc->ppu_counter = 0;
    gbc->joypad_buttons = 0x0F;
    gbc->joypad_directions = 0x0F;
    gbc->ram_enabled = 0;
    gbc->double_speed = 0;
    gbc->current_vbk = 0;
    gbc->current_wram_bank = 1;
    gbc->bg_palette_index = 0;
    gbc->bg_palette_data = 0;
    gbc->sp_palette_index = 0;
    gbc->sp_palette_data = 0;

    gbc->memory[PYEMU_IO_DIV] = 0xAB;
    pyemu_gbc_write_memory(gbc, 0xFF00, 0xCF);
    pyemu_gbc_write_memory(gbc, PYEMU_IO_TIMA, 0x00);
    pyemu_gbc_write_memory(gbc, PYEMU_IO_TMA, 0x00);
    pyemu_gbc_write_memory(gbc, PYEMU_IO_TAC, 0x00);
    pyemu_gbc_write_memory(gbc, PYEMU_IO_IF, 0xE1);
    pyemu_gbc_write_memory(gbc, 0xFF01, 0x00);  /* SB: Serial data = 0 */
    pyemu_gbc_write_memory(gbc, 0xFF02, 0x7E);  /* SC: Serial control = idle (no transfer, external clock) */
    pyemu_gbc_write_memory(gbc, PYEMU_GBC_SPEED, 0x7E);
    pyemu_gbc_write_memory(gbc, PYEMU_GBC_VBK, 0xFE);
    pyemu_gbc_write_memory(gbc, PYEMU_GBC_SVBK, 0xF9);
    pyemu_gbc_write_memory(gbc, 0xFF10, 0x80);
    pyemu_gbc_write_memory(gbc, 0xFF11, 0xBF);
    pyemu_gbc_write_memory(gbc, 0xFF12, 0xF3);
    pyemu_gbc_write_memory(gbc, 0xFF14, 0xBF);
    pyemu_gbc_write_memory(gbc, 0xFF16, 0x3F);
    pyemu_gbc_write_memory(gbc, 0xFF17, 0x00);
    pyemu_gbc_write_memory(gbc, 0xFF19, 0xBF);
    pyemu_gbc_write_memory(gbc, 0xFF1A, 0x7F);
    pyemu_gbc_write_memory(gbc, 0xFF1B, 0xFF);
    pyemu_gbc_write_memory(gbc, 0xFF1C, 0x9F);
    pyemu_gbc_write_memory(gbc, 0xFF1E, 0xBF);
    pyemu_gbc_write_memory(gbc, 0xFF20, 0xFF);
    pyemu_gbc_write_memory(gbc, 0xFF21, 0x00);
    pyemu_gbc_write_memory(gbc, 0xFF22, 0x00);
    pyemu_gbc_write_memory(gbc, 0xFF23, 0xBF);
    pyemu_gbc_write_memory(gbc, 0xFF24, 0x77);
    pyemu_gbc_write_memory(gbc, 0xFF25, 0xF3);
    pyemu_gbc_write_memory(gbc, 0xFF26, 0xF1);
    pyemu_gbc_write_memory(gbc, 0xFF40, 0x91);
    pyemu_gbc_write_memory(gbc, 0xFF41, 0x85);
    pyemu_gbc_update_stat(gbc);
    pyemu_gbc_write_memory(gbc, 0xFF42, 0x00);
    pyemu_gbc_write_memory(gbc, 0xFF43, 0x00);
    pyemu_gbc_write_memory(gbc, PYEMU_IO_LY, 0x00);
    pyemu_gbc_write_memory(gbc, 0xFF45, 0x00);
    pyemu_gbc_write_memory(gbc, 0xFF47, 0xFC);
    pyemu_gbc_write_memory(gbc, 0xFF48, 0xFF);
    pyemu_gbc_write_memory(gbc, 0xFF49, 0xFF);
    pyemu_gbc_write_memory(gbc, 0xFF4A, 0x00);
    pyemu_gbc_write_memory(gbc, 0xFF4B, 0x00);
    pyemu_gbc_init_default_palettes(gbc);
    pyemu_gbc_write_memory(gbc, PYEMU_GBC_BG_PALETTE_INDEX, 0x00);
    pyemu_gbc_write_memory(gbc, PYEMU_GBC_BG_PALETTE_DATA, gbc->bg_palettes[0].data[0]);
    pyemu_gbc_write_memory(gbc, PYEMU_GBC_SPRITE_PALETTE_INDEX, 0x00);
    pyemu_gbc_write_memory(gbc, PYEMU_GBC_SPRITE_PALETTE_DATA, gbc->sp_palettes[0].data[0]);
    pyemu_gbc_write_memory(gbc, PYEMU_IO_IE, 0x00);
}

static const char* pyemu_gbc_name(const pyemu_system* system) {
    (void)system;
    return "gbc";
}

static void pyemu_gbc_reset(pyemu_system* system) {
    pyemu_gbc_system* gbc = (pyemu_gbc_system*)system;
    memset(gbc->memory, 0, sizeof(gbc->memory));
    memset(gbc->frame, 0, sizeof(gbc->frame));
    memset(gbc->vram, 0, sizeof(gbc->vram));
    memset(gbc->wram, 0, sizeof(gbc->wram));
    memset(gbc->hram, 0, sizeof(gbc->hram));
    memset(gbc->line_scx, 0, sizeof(gbc->line_scx));
    memset(gbc->line_scy, 0, sizeof(gbc->line_scy));
    memset(gbc->line_wx, 0, sizeof(gbc->line_wx));
    memset(gbc->line_wy, 0, sizeof(gbc->line_wy));
    memset(gbc->line_lcdc, 0, sizeof(gbc->line_lcdc));
    memset(gbc->line_bgp, 0, sizeof(gbc->line_bgp));
    memset(gbc->line_obp0, 0, sizeof(gbc->line_obp0));
    memset(gbc->line_obp1, 0, sizeof(gbc->line_obp1));
    memset(gbc->line_win_y, 0, sizeof(gbc->line_win_y));
    gbc->window_line_counter = 0;
    memset(gbc->block_cache, 0, sizeof(gbc->block_cache));
    memset(gbc->bg_palettes, 0, sizeof(gbc->bg_palettes));
    memset(gbc->sp_palettes, 0, sizeof(gbc->sp_palettes));
    memset(&gbc->cpu, 0, sizeof(gbc->cpu));
    gbc->last_opcode = 0;
    gbc->stat_irq_line = 0;
    gbc->current_vbk = 0;
    memset(&gbc->last_access, 0, sizeof(gbc->last_access));
    memset(&gbc->last_mapper_access, 0, sizeof(gbc->last_mapper_access));
    gbc->bus_tracking_enabled = 1;

    if (!gbc->rom_loaded) {
        memset(gbc->rom_bank0, 0, sizeof(gbc->rom_bank0));
        memset(gbc->rom_bankx, 0, sizeof(gbc->rom_bankx));
        memset(gbc->loaded_rom, 0, sizeof(gbc->loaded_rom));
        memset(gbc->cartridge_title, 0, sizeof(gbc->cartridge_title));
        memset(gbc->eram, 0, sizeof(gbc->eram));
        gbc->rom_size = 0;
        gbc->rom_bank_count = 0;
        gbc->cartridge_type = 0;
        gbc->ram_size_code = 0;
        gbc->eram_size = 0;
        gbc->eram_bank_count = 0;
        gbc->ram_enabled = 0;
        gbc->mbc1_rom_bank = 1;
        gbc->mbc3_rom_bank = 0;
        gbc->mbc3_ram_bank = 0;
    } else {
        gbc->ram_enabled = 0;
        gbc->mbc1_rom_bank = 1;
        gbc->mbc3_rom_bank = pyemu_gbc_uses_mbc1(gbc) ? 0 : 1;
        gbc->mbc3_ram_bank = 0;
    }

    /* Zero RTC state on every reset */
    gbc->rtc_s = 0; gbc->rtc_m = 0; gbc->rtc_h = 0; gbc->rtc_dl = 0; gbc->rtc_dh = 0;
    gbc->rtc_latch_s = 0; gbc->rtc_latch_m = 0; gbc->rtc_latch_h = 0;
    gbc->rtc_latch_dl = 0; gbc->rtc_latch_dh = 0;
    gbc->rtc_latch_ready = 0;
    gbc->rtc_cycle_accum = 0;

    pyemu_gbc_sync_memory(gbc);
    pyemu_gbc_set_post_boot_state(gbc);
    pyemu_gbc_update_demo_frame(gbc);
}

static int pyemu_gbc_load_rom(pyemu_system* system, const char* path) {
    pyemu_gbc_system* gbc = (pyemu_gbc_system*)system;
    FILE* rom_file;
    long file_size;
    size_t read_size;
    uint8_t* rom_data;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    rom_file = fopen(path, "rb");
    if (rom_file == NULL) {
        return 0;
    }

    if (fseek(rom_file, 0, SEEK_END) != 0) {
        fclose(rom_file);
        return 0;
    }

    file_size = ftell(rom_file);
    if (file_size <= 0 || fseek(rom_file, 0, SEEK_SET) != 0) {
        fclose(rom_file);
        return 0;
    }

    rom_data = (uint8_t*)malloc((size_t)file_size);
    if (rom_data == NULL) {
        fclose(rom_file);
        return 0;
    }

    read_size = fread(rom_data, 1, (size_t)file_size, rom_file);
    fclose(rom_file);
    if (read_size != (size_t)file_size) {
        free(rom_data);
        return 0;
    }

    free(gbc->rom_data);
    gbc->rom_data = rom_data;
    gbc->rom_size = (size_t)file_size;
    gbc->rom_bank_count = (gbc->rom_size + (PYEMU_GBC_ROM_BANKX_SIZE - 1)) / PYEMU_GBC_ROM_BANKX_SIZE;
    gbc->cartridge_type = gbc->rom_size > 0x0147 ? gbc->rom_data[0x0147] : 0;
    gbc->ram_size_code = gbc->rom_size > 0x0149 ? gbc->rom_data[0x0149] : 0;
    gbc->eram_size = pyemu_gbc_eram_size_from_code(gbc->ram_size_code);
    if (gbc->eram_size > sizeof(gbc->eram)) {
        gbc->eram_size = sizeof(gbc->eram);
    }
    gbc->eram_bank_count = gbc->eram_size == 0 ? 0 : ((gbc->eram_size + PYEMU_GBC_ERAM_BANK_SIZE - 1) / PYEMU_GBC_ERAM_BANK_SIZE);
    gbc->ram_enabled = 0;
    gbc->mbc1_rom_bank = 1;
    gbc->mbc3_rom_bank = pyemu_gbc_uses_mbc1(gbc) ? 0 : 1;
    gbc->mbc3_ram_bank = 0;
    gbc->rom_loaded = 1;
    strncpy(gbc->loaded_rom, path, sizeof(gbc->loaded_rom) - 1);
    gbc->loaded_rom[sizeof(gbc->loaded_rom) - 1] = '\0';
    memset(gbc->eram, 0, sizeof(gbc->eram));
    pyemu_gbc_load_battery_ram(gbc);
    pyemu_gbc_refresh_rom_mapping(gbc);
    pyemu_gbc_extract_title(gbc);
    pyemu_gbc_reset(system);
    return 1;
}

static int pyemu_gbc_save_state(const pyemu_system* system, const char* path) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    pyemu_gbc_state_file snapshot;
    FILE* state_file;

    if (gbc == NULL || !gbc->rom_loaded || path == NULL || path[0] == 0) {
        return 0;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.magic = PYEMU_GBC_STATE_MAGIC;
    snapshot.version = PYEMU_GBC_STATE_VERSION;
    snapshot.cpu = gbc->cpu;
    memcpy(snapshot.memory, gbc->memory, sizeof(snapshot.memory));
    memcpy(snapshot.vram, gbc->vram, sizeof(snapshot.vram));
    memcpy(snapshot.wram, gbc->wram, sizeof(snapshot.wram));
    memcpy(snapshot.hram, gbc->hram, sizeof(snapshot.hram));
    memcpy(snapshot.eram, gbc->eram, sizeof(snapshot.eram));
    memcpy(snapshot.line_scx, gbc->line_scx, sizeof(snapshot.line_scx));
    memcpy(snapshot.line_scy, gbc->line_scy, sizeof(snapshot.line_scy));
    memcpy(snapshot.line_wx, gbc->line_wx, sizeof(snapshot.line_wx));
    memcpy(snapshot.line_wy, gbc->line_wy, sizeof(snapshot.line_wy));
    memcpy(snapshot.line_lcdc, gbc->line_lcdc, sizeof(snapshot.line_lcdc));
    memcpy(snapshot.line_bgp, gbc->line_bgp, sizeof(snapshot.line_bgp));
    memcpy(snapshot.line_obp0, gbc->line_obp0, sizeof(snapshot.line_obp0));
    memcpy(snapshot.line_obp1, gbc->line_obp1, sizeof(snapshot.line_obp1));
    memcpy(snapshot.line_win_y, gbc->line_win_y, sizeof(snapshot.line_win_y));
    snapshot.window_line_counter = gbc->window_line_counter;
    memcpy(snapshot.bg_palettes, gbc->bg_palettes, sizeof(snapshot.bg_palettes));
    memcpy(snapshot.sp_palettes, gbc->sp_palettes, sizeof(snapshot.sp_palettes));
    snapshot.current_vbk = gbc->current_vbk;
    memcpy(snapshot.loaded_rom, gbc->loaded_rom, sizeof(snapshot.loaded_rom));
    memcpy(snapshot.cartridge_title, gbc->cartridge_title, sizeof(snapshot.cartridge_title));
    snapshot.rom_size = gbc->rom_size;
    snapshot.rom_bank_count = gbc->rom_bank_count;
    snapshot.rom_loaded = gbc->rom_loaded;
    snapshot.cartridge_type = gbc->cartridge_type;
    snapshot.ram_size_code = gbc->ram_size_code;
    snapshot.eram_size = gbc->eram_size;
    snapshot.eram_bank_count = gbc->eram_bank_count;
    snapshot.ram_enabled = gbc->ram_enabled;
    snapshot.mbc1_rom_bank = gbc->mbc1_rom_bank;
    snapshot.mbc3_rom_bank = gbc->mbc3_rom_bank;
    snapshot.mbc3_ram_bank = gbc->mbc3_ram_bank;
    snapshot.ime = gbc->ime;
    snapshot.ime_pending = gbc->ime_pending;
    snapshot.last_opcode = gbc->last_opcode;
    snapshot.cycle_count = gbc->cycle_count;
    snapshot.div_counter = gbc->div_counter;
    snapshot.timer_counter = gbc->timer_counter;
    snapshot.ppu_counter = gbc->ppu_counter;
    snapshot.joypad_buttons = gbc->joypad_buttons;
    snapshot.joypad_directions = gbc->joypad_directions;
    snapshot.stat_coincidence = gbc->stat_coincidence;
    snapshot.stat_mode = gbc->stat_mode;
    snapshot.last_access = gbc->last_access;
    snapshot.double_speed = gbc->double_speed;
    snapshot.faulted = gbc->faulted;
    snapshot.serial_counter = gbc->serial_counter;
    snapshot.rtc_s = gbc->rtc_s;
    snapshot.rtc_m = gbc->rtc_m;
    snapshot.rtc_h = gbc->rtc_h;
    snapshot.rtc_dl = gbc->rtc_dl;
    snapshot.rtc_dh = gbc->rtc_dh;
    snapshot.rtc_latch_s = gbc->rtc_latch_s;
    snapshot.rtc_latch_m = gbc->rtc_latch_m;
    snapshot.rtc_latch_h = gbc->rtc_latch_h;
    snapshot.rtc_latch_dl = gbc->rtc_latch_dl;
    snapshot.rtc_latch_dh = gbc->rtc_latch_dh;
    snapshot.rtc_latch_ready = gbc->rtc_latch_ready;
    snapshot.rtc_cycle_accum = gbc->rtc_cycle_accum;

    state_file = fopen(path, "wb");
    if (state_file == NULL) {
        return 0;
    }
    if (fwrite(&snapshot, sizeof(snapshot), 1, state_file) != 1) {
        fclose(state_file);
        return 0;
    }
    fclose(state_file);
    return 1;
}

static int pyemu_gbc_load_state(pyemu_system* system, const char* path) {
    pyemu_gbc_system* gbc = (pyemu_gbc_system*)system;
    pyemu_gbc_state_file snapshot;
    FILE* state_file;

    if (gbc == NULL || !gbc->rom_loaded || path == NULL || path[0] == 0) {
        return 0;
    }

    state_file = fopen(path, "rb");
    if (state_file == NULL) {
        return 0;
    }
    if (fread(&snapshot, sizeof(snapshot), 1, state_file) != 1) {
        fclose(state_file);
        return 0;
    }
    fclose(state_file);

    if (snapshot.magic != PYEMU_GBC_STATE_MAGIC || snapshot.version != PYEMU_GBC_STATE_VERSION) {
        return 0;
    }
    if (!snapshot.rom_loaded || strcmp(snapshot.loaded_rom, gbc->loaded_rom) != 0 || strcmp(snapshot.cartridge_title, gbc->cartridge_title) != 0) {
        return 0;
    }

    gbc->cpu = snapshot.cpu;
    memcpy(gbc->memory, snapshot.memory, sizeof(gbc->memory));
    memcpy(gbc->vram, snapshot.vram, sizeof(gbc->vram));
    memcpy(gbc->wram, snapshot.wram, sizeof(gbc->wram));
    memcpy(gbc->hram, snapshot.hram, sizeof(gbc->hram));
    memcpy(gbc->eram, snapshot.eram, sizeof(gbc->eram));
    memcpy(gbc->line_scx, snapshot.line_scx, sizeof(gbc->line_scx));
    memcpy(gbc->line_scy, snapshot.line_scy, sizeof(gbc->line_scy));
    memcpy(gbc->line_wx, snapshot.line_wx, sizeof(gbc->line_wx));
    memcpy(gbc->line_wy, snapshot.line_wy, sizeof(gbc->line_wy));
    memcpy(gbc->line_lcdc, snapshot.line_lcdc, sizeof(gbc->line_lcdc));
    memcpy(gbc->line_bgp, snapshot.line_bgp, sizeof(snapshot.line_bgp));
    memcpy(gbc->line_obp0, snapshot.line_obp0, sizeof(gbc->line_obp0));
    memcpy(gbc->line_obp1, snapshot.line_obp1, sizeof(gbc->line_obp1));
    memcpy(gbc->line_win_y, snapshot.line_win_y, sizeof(gbc->line_win_y));
    gbc->window_line_counter = snapshot.window_line_counter;
    memcpy(gbc->bg_palettes, snapshot.bg_palettes, sizeof(gbc->bg_palettes));
    memcpy(gbc->sp_palettes, snapshot.sp_palettes, sizeof(gbc->sp_palettes));
    gbc->current_vbk = snapshot.current_vbk;
    gbc->current_wram_bank = snapshot.current_wram_bank == 0 ? 1 : snapshot.current_wram_bank;
    gbc->rom_size = snapshot.rom_size;
    gbc->rom_bank_count = snapshot.rom_bank_count;
    gbc->rom_loaded = snapshot.rom_loaded;
    gbc->cartridge_type = snapshot.cartridge_type;
    gbc->ram_size_code = snapshot.ram_size_code;
    gbc->eram_size = snapshot.eram_size;
    gbc->eram_bank_count = snapshot.eram_bank_count;
    gbc->ram_enabled = snapshot.ram_enabled;
    gbc->mbc1_rom_bank = snapshot.mbc1_rom_bank;
    gbc->mbc3_rom_bank = snapshot.mbc3_rom_bank;
    gbc->mbc3_ram_bank = snapshot.mbc3_ram_bank;
    gbc->ime = snapshot.ime;
    gbc->ime_pending = snapshot.ime_pending;
    gbc->ime_delay = 0;
    gbc->halt_bug = 0;
    gbc->last_opcode = snapshot.last_opcode;
    gbc->cycle_count = snapshot.cycle_count;
    gbc->div_counter = snapshot.div_counter;
    gbc->timer_counter = snapshot.timer_counter;
    gbc->ppu_counter = snapshot.ppu_counter;
    gbc->joypad_buttons = snapshot.joypad_buttons;
    gbc->joypad_directions = snapshot.joypad_directions;
    gbc->stat_coincidence = snapshot.stat_coincidence;
    gbc->stat_mode = snapshot.stat_mode;
    gbc->stat_irq_line = 0;
    gbc->last_access = snapshot.last_access;
    memset(&gbc->last_mapper_access, 0, sizeof(gbc->last_mapper_access));
    gbc->double_speed = snapshot.double_speed;
    gbc->faulted = snapshot.faulted;
    gbc->serial_counter = snapshot.serial_counter;
    gbc->rtc_s = snapshot.rtc_s;
    gbc->rtc_m = snapshot.rtc_m;
    gbc->rtc_h = snapshot.rtc_h;
    gbc->rtc_dl = snapshot.rtc_dl;
    gbc->rtc_dh = snapshot.rtc_dh;
    gbc->rtc_latch_s = snapshot.rtc_latch_s;
    gbc->rtc_latch_m = snapshot.rtc_latch_m;
    gbc->rtc_latch_h = snapshot.rtc_latch_h;
    gbc->rtc_latch_dl = snapshot.rtc_latch_dl;
    gbc->rtc_latch_dh = snapshot.rtc_latch_dh;
    gbc->rtc_latch_ready = snapshot.rtc_latch_ready;
    gbc->rtc_cycle_accum = snapshot.rtc_cycle_accum;

    pyemu_gbc_refresh_rom_mapping(gbc);
    pyemu_gbc_update_demo_frame(gbc);
    return 1;
}

static int pyemu_gbc_execute_opcode(pyemu_gbc_system* gbc, uint8_t opcode) {
    int cycles = 4;
    gbc->last_opcode = opcode;

    if (opcode >= 0x40 && opcode <= 0x7F && opcode != 0x76) {
        int dst = (opcode >> 3) & 0x07;
        int src = opcode & 0x07;
        uint8_t value = pyemu_gbc_read_r8(gbc, src);
        pyemu_gbc_write_r8(gbc, dst, value);
        return (src == 6 || dst == 6) ? 8 : 4;
    }

    cycles = pyemu_gbc_execute_load_store(gbc, opcode);
    if (cycles >= 0) {
        return cycles;
    }
    cycles = pyemu_gbc_execute_control_flow(gbc, opcode);
    if (cycles >= 0) {
        return cycles;
    }
    cycles = pyemu_gbc_execute_alu(gbc, opcode);
    if (cycles >= 0) {
        return cycles;
    }

    switch (opcode) {
        case 0x00:
            cycles = 4;
            break;
        case 0x10: {
            /* STOP: on CGB, if KEY1 bit 0 is set (speed switch armed), toggle
               double-speed mode and clear the arm bit, then resume execution.
               Otherwise treat as a low-power halt (same as HALT for emulation). */
            uint8_t key1 = gbc->memory[PYEMU_GBC_SPEED];
            pyemu_gbc_fetch_u8(gbc); /* consume the mandatory 0x00 operand byte */
            if (key1 & 0x01) {
                gbc->double_speed = !gbc->double_speed;
                gbc->memory[PYEMU_GBC_SPEED] = (uint8_t)(gbc->double_speed ? 0x80 : 0x00);
            } else {
                gbc->cpu.halted = 1;
            }
            cycles = 4;
            break;
        }
        case 0x76: {
            uint8_t pending_interrupts = pyemu_gbc_pending_interrupts(gbc);
            if (!gbc->ime && pending_interrupts != 0) {
                gbc->halt_bug = 1;
                gbc->cpu.halted = 0;
            } else {
                gbc->cpu.halted = 1;
            }
            cycles = 4;
            break;
        }
        case 0xE0: {
            uint16_t address = (uint16_t)(0xFF00 | pyemu_gbc_fetch_u8(gbc));
            pyemu_gbc_write_memory(gbc, address, gbc->cpu.a);
            cycles = 12;
            break;
        }
        case 0xE2:
            pyemu_gbc_write_memory(gbc, (uint16_t)(0xFF00 | gbc->cpu.c), gbc->cpu.a);
            cycles = 8;
            break;
        case 0xE8:
            pyemu_gbc_add_sp_signed(gbc, (int8_t)pyemu_gbc_fetch_u8(gbc));
            cycles = 16;
            break;
        case 0xEA: {
            uint16_t address = pyemu_gbc_fetch_u16(gbc);
            pyemu_gbc_write_memory(gbc, address, gbc->cpu.a);
            cycles = 16;
            break;
        }
        case 0xF0: {
            uint16_t address = (uint16_t)(0xFF00 | pyemu_gbc_fetch_u8(gbc));
            gbc->cpu.a = pyemu_gbc_read_memory(gbc, address);
            cycles = 12;
            break;
        }
        case 0xF2:
            gbc->cpu.a = pyemu_gbc_read_memory(gbc, (uint16_t)(0xFF00 | gbc->cpu.c));
            cycles = 8;
            break;
        case 0xCB:
            cycles = pyemu_gbc_execute_cb(gbc);
            break;
        case 0xF3:
            gbc->ime = 0;
            gbc->ime_pending = 0;
            gbc->ime_delay = 0;
            cycles = 4;
            break;
        case 0xF8:
            pyemu_gbc_set_hl(gbc, pyemu_gbc_sp_plus_signed(gbc, (int8_t)pyemu_gbc_fetch_u8(gbc)));
            cycles = 12;
            break;
        case 0xFA: {
            uint16_t address = pyemu_gbc_fetch_u16(gbc);
            gbc->cpu.a = pyemu_gbc_read_memory(gbc, address);
            cycles = 16;
            break;
        }
        case 0xFB:
            gbc->ime_pending = 1;
            gbc->ime_delay = 1;
            cycles = 4;
            break;
        default:
            gbc->cpu.halted = 1;
            gbc->faulted = 1;
            gbc->cpu.pc = (uint16_t)(gbc->cpu.pc - 1);
            cycles = 4;
            break;
    }

    return cycles;
}

static uint8_t pyemu_gbc_current_bank_for_address(const pyemu_gbc_system* gbc, uint16_t address) {
    if (address < 0x4000) {
        if (pyemu_gbc_uses_mbc1(gbc) && pyemu_gbc_mbc1_mode(gbc)) {
            uint8_t bank = (uint8_t)(pyemu_gbc_mbc1_upper_bits(gbc) << 5);
            if (gbc->rom_bank_count > 0) {
                bank = (uint8_t)(bank % gbc->rom_bank_count);
            }
            return bank;
        }
        return 0;
    }
    if (address < 0x8000) {
        return pyemu_gbc_current_rom_bank(gbc);
    }
    return 0xFF;
}

static pyemu_gbc_block_cache_entry* pyemu_gbc_get_block_cache_entry(pyemu_gbc_system* gbc, uint16_t pc, uint8_t bank) {
    uint32_t index = ((((uint32_t)bank) << 16) ^ pc) & (PYEMU_GBC_BLOCK_CACHE_SIZE - 1);
    return &gbc->block_cache[index];
}

static int pyemu_gbc_decode_block(pyemu_gbc_system* gbc, pyemu_gbc_block_cache_entry* entry, uint16_t pc, uint8_t bank) {
    uint16_t cursor = pc;
    uint8_t count = 0;

    memset(entry, 0, sizeof(*entry));
    entry->pc = pc;
    entry->bank = bank;

    while (count < PYEMU_GBC_BLOCK_MAX_INSNS && cursor < 0x8000) {
        pyemu_gbc_block_insn* insn = &entry->insns[count];
        uint8_t opcode = pyemu_gbc_peek_memory(gbc, cursor);
        insn->length = 1;

        switch (opcode) {
            case 0x00: insn->type = PYEMU_GBC_BLOCK_OP_NOP; break;
            case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E:
                insn->type = PYEMU_GBC_BLOCK_OP_LD_R_D8;
                insn->operand8 = pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1));
                insn->length = 2;
                break;
            case 0x11:
                insn->type = PYEMU_GBC_BLOCK_OP_LD_DE_D16;
                insn->operand16 = (uint16_t)(pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0x21:
                insn->type = PYEMU_GBC_BLOCK_OP_LD_HL_D16;
                insn->operand16 = (uint16_t)(pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0x18: insn->type = PYEMU_GBC_BLOCK_OP_JR; insn->relative = (int8_t)pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x20: insn->type = PYEMU_GBC_BLOCK_OP_JR_NZ; insn->relative = (int8_t)pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x28: insn->type = PYEMU_GBC_BLOCK_OP_JR_Z; insn->relative = (int8_t)pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x30: insn->type = PYEMU_GBC_BLOCK_OP_JR_NC; insn->relative = (int8_t)pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x38: insn->type = PYEMU_GBC_BLOCK_OP_JR_C; insn->relative = (int8_t)pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x05: insn->type = PYEMU_GBC_BLOCK_OP_DEC_B; break;
            case 0x22: insn->type = PYEMU_GBC_BLOCK_OP_LD_HL_INC_A; break;
            case 0x2A: insn->type = PYEMU_GBC_BLOCK_OP_LD_A_HL_INC; break;
            case 0x32: insn->type = PYEMU_GBC_BLOCK_OP_LD_HL_DEC_A; break;
            case 0x3A: insn->type = PYEMU_GBC_BLOCK_OP_LD_A_HL_DEC; break;
            case 0x3C: insn->type = PYEMU_GBC_BLOCK_OP_INC_A; break;
            case 0x6F: insn->type = PYEMU_GBC_BLOCK_OP_LD_L_A; break;
            case 0x7D: insn->type = PYEMU_GBC_BLOCK_OP_LD_A_L; break;
            case 0x76: insn->type = PYEMU_GBC_BLOCK_OP_HALT; entry->terminates = 1; break;
            case 0xA7: insn->type = PYEMU_GBC_BLOCK_OP_AND_A; break;
            case 0xAF: insn->type = PYEMU_GBC_BLOCK_OP_XOR_A; break;
            case 0xC9: insn->type = PYEMU_GBC_BLOCK_OP_RET; entry->terminates = 1; break;
            case 0xE0:
                insn->type = PYEMU_GBC_BLOCK_OP_LDH_A8_A;
                insn->operand8 = pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1));
                insn->length = 2;
                break;
            case 0xEA:
                insn->type = PYEMU_GBC_BLOCK_OP_LD_A16_A;
                insn->operand16 = (uint16_t)(pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0xF0:
                insn->type = PYEMU_GBC_BLOCK_OP_LDH_A_A8;
                insn->operand8 = pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1));
                insn->length = 2;
                break;
            case 0xFA:
                insn->type = PYEMU_GBC_BLOCK_OP_LD_A_A16;
                insn->operand16 = (uint16_t)(pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0xFE:
                insn->type = PYEMU_GBC_BLOCK_OP_CP_D8;
                insn->operand8 = pyemu_gbc_peek_memory(gbc, (uint16_t)(cursor + 1));
                insn->length = 2;
                break;
            default:
                entry->valid = 0;
                return 0;
        }

        count = (uint8_t)(count + 1);
        cursor = (uint16_t)(cursor + insn->length);
        if (entry->terminates) {
            break;
        }
    }

    entry->insn_count = count;
    entry->valid = count > 0 ? 1 : 0;
    return entry->valid;
}

static int pyemu_gbc_execute_block(pyemu_gbc_system* gbc, const pyemu_gbc_block_cache_entry* entry, int* out_cycles) {
    int cycles = 0;
    uint8_t index;

    gbc->cpu.pc = entry->pc;
    for (index = 0; index < entry->insn_count; ++index) {
        const pyemu_gbc_block_insn* insn = &entry->insns[index];
        uint16_t opcode_pc = gbc->cpu.pc;
        uint16_t next_pc = (uint16_t)(opcode_pc + insn->length);
        uint8_t opcode = pyemu_gbc_peek_memory(gbc, opcode_pc);

        switch (insn->type) {
            case PYEMU_GBC_BLOCK_OP_NOP: gbc->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_GBC_BLOCK_OP_LD_R_D8:
                switch (opcode) {
                    case 0x06: gbc->cpu.b = insn->operand8; break;
                    case 0x0E: gbc->cpu.c = insn->operand8; break;
                    case 0x16: gbc->cpu.d = insn->operand8; break;
                    case 0x1E: gbc->cpu.e = insn->operand8; break;
                    case 0x26: gbc->cpu.h = insn->operand8; break;
                    case 0x2E: gbc->cpu.l = insn->operand8; break;
                    default: gbc->cpu.a = insn->operand8; break;
                }
                gbc->cpu.pc = next_pc; cycles += 8; break;
            case PYEMU_GBC_BLOCK_OP_LD_HL_D16: pyemu_gbc_set_hl(gbc, insn->operand16); gbc->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_GBC_BLOCK_OP_LD_DE_D16: pyemu_gbc_set_de(gbc, insn->operand16); gbc->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_GBC_BLOCK_OP_LDH_A_A8: gbc->cpu.a = pyemu_gbc_read_memory(gbc, (uint16_t)(0xFF00 | insn->operand8)); gbc->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_GBC_BLOCK_OP_LDH_A8_A: pyemu_gbc_write_memory(gbc, (uint16_t)(0xFF00 | insn->operand8), gbc->cpu.a); gbc->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_GBC_BLOCK_OP_LD_A_A16: gbc->cpu.a = pyemu_gbc_read_memory(gbc, insn->operand16); gbc->cpu.pc = next_pc; cycles += 16; break;
            case PYEMU_GBC_BLOCK_OP_LD_A16_A: pyemu_gbc_write_memory(gbc, insn->operand16, gbc->cpu.a); gbc->cpu.pc = next_pc; cycles += 16; break;
            case PYEMU_GBC_BLOCK_OP_XOR_A: pyemu_gbc_xor_a(gbc, gbc->cpu.a); gbc->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_GBC_BLOCK_OP_AND_A: pyemu_gbc_and_a(gbc, gbc->cpu.a); gbc->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_GBC_BLOCK_OP_INC_A: gbc->cpu.a = pyemu_gbc_inc8(gbc, gbc->cpu.a); gbc->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_GBC_BLOCK_OP_CP_D8: pyemu_gbc_cp_a(gbc, insn->operand8); gbc->cpu.pc = next_pc; cycles += 8; break;
            case PYEMU_GBC_BLOCK_OP_LD_HL_INC_A: { uint16_t hl = pyemu_gbc_get_hl(gbc); pyemu_gbc_write_memory(gbc, hl, gbc->cpu.a); pyemu_gbc_set_hl(gbc, (uint16_t)(hl + 1)); gbc->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_GBC_BLOCK_OP_LD_A_HL_INC: { uint16_t hl = pyemu_gbc_get_hl(gbc); gbc->cpu.a = pyemu_gbc_read_memory(gbc, hl); pyemu_gbc_set_hl(gbc, (uint16_t)(hl + 1)); gbc->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_GBC_BLOCK_OP_LD_A_HL_DEC: { uint16_t hl = pyemu_gbc_get_hl(gbc); gbc->cpu.a = pyemu_gbc_read_memory(gbc, hl); pyemu_gbc_set_hl(gbc, (uint16_t)(hl - 1)); gbc->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_GBC_BLOCK_OP_LD_HL_DEC_A: { uint16_t hl = pyemu_gbc_get_hl(gbc); pyemu_gbc_write_memory(gbc, hl, gbc->cpu.a); pyemu_gbc_set_hl(gbc, (uint16_t)(hl - 1)); gbc->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_GBC_BLOCK_OP_DEC_B: gbc->cpu.b = pyemu_gbc_dec8(gbc, gbc->cpu.b); gbc->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_GBC_BLOCK_OP_LD_A_L: gbc->cpu.a = gbc->cpu.l; gbc->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_GBC_BLOCK_OP_LD_L_A: gbc->cpu.l = gbc->cpu.a; gbc->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_GBC_BLOCK_OP_JR: gbc->cpu.pc = (uint16_t)(next_pc + insn->relative); cycles += 12; *out_cycles = cycles; return 1;
            case PYEMU_GBC_BLOCK_OP_JR_NZ: { int cond = !pyemu_gbc_get_flag(gbc, PYEMU_FLAG_Z); gbc->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_GBC_BLOCK_OP_JR_Z: { int cond = pyemu_gbc_get_flag(gbc, PYEMU_FLAG_Z); gbc->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_GBC_BLOCK_OP_JR_NC: { int cond = !pyemu_gbc_get_flag(gbc, PYEMU_FLAG_C); gbc->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_GBC_BLOCK_OP_JR_C: { int cond = pyemu_gbc_get_flag(gbc, PYEMU_FLAG_C); gbc->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_GBC_BLOCK_OP_RET: gbc->cpu.pc = pyemu_gbc_pop_u16(gbc); cycles += 16; *out_cycles = cycles; return 1;
            case PYEMU_GBC_BLOCK_OP_HALT: {
                uint8_t pending_interrupts = pyemu_gbc_pending_interrupts(gbc);
                if (!gbc->ime && pending_interrupts != 0) {
                    gbc->halt_bug = 1;
                    gbc->cpu.halted = 0;
                } else {
                    gbc->cpu.halted = 1;
                }
                gbc->cpu.pc = next_pc;
                cycles += 4;
                *out_cycles = cycles;
                return 1;
            }
            default: return 0;
        }
    }

    *out_cycles = cycles;
    return 1;
}

static int pyemu_gbc_execute_hotpath(pyemu_gbc_system* gbc, int* out_cycles) {
    uint16_t pc = gbc->cpu.pc;

    if (pc <= 0xFFF6U) {
        uint8_t b0 = pyemu_gbc_peek_memory(gbc, pc);
        uint8_t b1 = pyemu_gbc_peek_memory(gbc, (uint16_t)(pc + 1));
        uint8_t b2 = pyemu_gbc_peek_memory(gbc, (uint16_t)(pc + 2));
        uint8_t b3 = pyemu_gbc_peek_memory(gbc, (uint16_t)(pc + 3));
        uint8_t b4 = pyemu_gbc_peek_memory(gbc, (uint16_t)(pc + 4));
        uint8_t b5 = pyemu_gbc_peek_memory(gbc, (uint16_t)(pc + 5));
        uint8_t b6 = pyemu_gbc_peek_memory(gbc, (uint16_t)(pc + 6));
        uint8_t b7 = pyemu_gbc_peek_memory(gbc, (uint16_t)(pc + 7));
        uint8_t b8 = pyemu_gbc_peek_memory(gbc, (uint16_t)(pc + 8));
        uint8_t b9 = pyemu_gbc_peek_memory(gbc, (uint16_t)(pc + 9));

        if (b0 == 0x21 && b3 == 0x06 && b5 == 0x22 && b6 == 0x05 && b7 == 0x20 && b8 == 0xFC && b9 == 0xC9) {
            uint16_t address = (uint16_t)(b1 | ((uint16_t)b2 << 8));
            uint8_t count = b4;
            uint8_t value = gbc->cpu.a;
            uint16_t index;

            for (index = 0; index < count; ++index) {
                pyemu_gbc_write_memory(gbc, (uint16_t)(address + index), value);
            }

            pyemu_gbc_set_hl(gbc, (uint16_t)(address + count));
            gbc->cpu.b = 0;
            gbc->cpu.f = (uint8_t)((gbc->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gbc->cpu.pc = pyemu_gbc_pop_u16(gbc);
            *out_cycles = (int)(24U * (uint32_t)count + 32U);
            return 1;
        }

        if (b0 == 0xAF && b1 == 0x22 && b2 == 0x7D && b3 == 0xFE && b5 == 0x38 && b6 == 0xF9 && b7 == 0xC9) {
            uint8_t limit = b4;
            uint16_t address = pyemu_gbc_get_hl(gbc);
            uint8_t start_low = gbc->cpu.l;
            uint8_t count = limit > start_low ? (uint8_t)(limit - start_low) : 0;
            uint16_t index;

            gbc->cpu.a = 0;
            if (count == 0) {
                gbc->cpu.f = (uint8_t)(PYEMU_FLAG_Z | PYEMU_FLAG_N);
                gbc->cpu.pc = pyemu_gbc_pop_u16(gbc);
                *out_cycles = 24;
                return 1;
            }

            for (index = 0; index < count; ++index) {
                pyemu_gbc_write_memory(gbc, (uint16_t)(address + index), 0);
            }

            pyemu_gbc_set_hl(gbc, (uint16_t)(address + count));
            gbc->cpu.a = limit;
            gbc->cpu.f = (uint8_t)(PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gbc->cpu.pc = pyemu_gbc_pop_u16(gbc);
            *out_cycles = (int)(32U * (uint32_t)count + 16U);
            return 1;
        }

        if (b0 == 0x22 && b1 == 0x05 && b2 == 0x20 && b3 == 0xFC && b4 == 0xC9) {
            uint16_t address = pyemu_gbc_get_hl(gbc);
            uint8_t count = gbc->cpu.b;
            uint8_t value = gbc->cpu.a;
            uint16_t index;

            if (count == 0) {
                gbc->cpu.pc = pyemu_gbc_pop_u16(gbc);
                *out_cycles = 16;
                return 1;
            }

            for (index = 0; index < count; ++index) {
                pyemu_gbc_write_memory(gbc, (uint16_t)(address + index), value);
            }

            pyemu_gbc_set_hl(gbc, (uint16_t)(address + count));
            gbc->cpu.b = 0;
            gbc->cpu.f = (uint8_t)((gbc->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gbc->cpu.pc = pyemu_gbc_pop_u16(gbc);
            *out_cycles = (int)(20U * (uint32_t)count + 16U);
            return 1;
        }

        if (b0 == 0x3D && b1 == 0x20 && b2 == 0xFD && b3 == 0xC9) {
            uint8_t count = gbc->cpu.a;
            if (count == 0) {
                gbc->cpu.f = (uint8_t)((gbc->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
                gbc->cpu.pc = pyemu_gbc_pop_u16(gbc);
                *out_cycles = 16;
                return 1;
            }
            gbc->cpu.a = 0;
            gbc->cpu.f = (uint8_t)((gbc->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gbc->cpu.pc = pyemu_gbc_pop_u16(gbc);
            *out_cycles = (int)(12U * (uint32_t)count + 4U);
            return 1;
        }
    }

    return 0;
}

static void pyemu_gbc_step_instruction_internal(pyemu_gbc_system* gbc, int render_frame) {
    uint8_t opcode;
    int cycles;

    if (gbc->faulted) {
        if (render_frame) {
            pyemu_gbc_update_demo_frame(gbc);
        }
        return;
    }

    if (pyemu_gbc_service_interrupts(gbc) > 0) {
        if (render_frame) {
            pyemu_gbc_update_demo_frame(gbc);
        }
        return;
    }

    if (gbc->cpu.halted) {
        int halted_cycles = 4;
        if (!render_frame) {
            uint8_t enabled_interrupts = (uint8_t)(gbc->memory[PYEMU_IO_IE] & 0x1F);
            uint8_t pending_interrupts = pyemu_gbc_pending_interrupts(gbc);
            if (pending_interrupts == 0 && enabled_interrupts == PYEMU_INTERRUPT_VBLANK) {
                uint32_t skip_cycles = pyemu_gbc_cycles_until_vblank(gbc);
                if ((gbc->memory[PYEMU_IO_TAC] & 0x04) == 0 && skip_cycles > 4U) {
                    pyemu_gbc_fast_forward_to_vblank(gbc);
                    if (render_frame) {
                        pyemu_gbc_update_demo_frame(gbc);
                    }
                    return;
                }
                if (skip_cycles > 4U) {
                    halted_cycles = (int)skip_cycles;
                }
            }
        }
        pyemu_gbc_tick(gbc, halted_cycles);
        if (render_frame) {
            pyemu_gbc_update_demo_frame(gbc);
        }
        return;
    }

    if (gbc->cpu.pc < 0x8000) {
        uint8_t bank = pyemu_gbc_current_bank_for_address(gbc, gbc->cpu.pc);
        if (bank != 0xFF) {
            pyemu_gbc_block_cache_entry* entry = pyemu_gbc_get_block_cache_entry(gbc, gbc->cpu.pc, bank);
            if ((!entry->valid || entry->pc != gbc->cpu.pc || entry->bank != bank) && !pyemu_gbc_decode_block(gbc, entry, gbc->cpu.pc, bank)) {
                entry->valid = 0;
            }
            if (entry->valid && entry->pc == gbc->cpu.pc && entry->bank == bank && pyemu_gbc_execute_block(gbc, entry, &cycles)) {
                pyemu_gbc_tick(gbc, cycles);
            } else if (pyemu_gbc_execute_hotpath(gbc, &cycles)) {
                pyemu_gbc_tick(gbc, cycles);
            } else {
                opcode = pyemu_gbc_fetch_u8(gbc);
                cycles = pyemu_gbc_execute_opcode(gbc, opcode);
                pyemu_gbc_tick(gbc, cycles);
            }
        } else if (pyemu_gbc_execute_hotpath(gbc, &cycles)) {
            pyemu_gbc_tick(gbc, cycles);
        } else {
            opcode = pyemu_gbc_fetch_u8(gbc);
            cycles = pyemu_gbc_execute_opcode(gbc, opcode);
            pyemu_gbc_tick(gbc, cycles);
        }
    } else if (pyemu_gbc_execute_hotpath(gbc, &cycles)) {
        pyemu_gbc_tick(gbc, cycles);
    } else {
        opcode = pyemu_gbc_fetch_u8(gbc);
        cycles = pyemu_gbc_execute_opcode(gbc, opcode);
        pyemu_gbc_tick(gbc, cycles);
    }

    if (gbc->ime_pending) {
        if (gbc->ime_delay > 0) {
            gbc->ime_delay -= 1;
        } else {
            gbc->ime = 1;
            gbc->ime_pending = 0;
        }
    }

    if (render_frame) {
        pyemu_gbc_update_demo_frame(gbc);
    }
}

static void pyemu_gbc_step_instruction(pyemu_system* system) {
    pyemu_gbc_system* gbc = (pyemu_gbc_system*)system;
    pyemu_gbc_step_instruction_internal(gbc, 1);
}

static void pyemu_gbc_step_frame(pyemu_system* system) {
    pyemu_gbc_system* gbc = (pyemu_gbc_system*)system;
    uint64_t cycles_per_frame = gbc->double_speed ? (PYEMU_GBC_CYCLES_PER_FRAME * 2) : PYEMU_GBC_CYCLES_PER_FRAME;
    uint64_t target_cycles = gbc->cycle_count + cycles_per_frame;

    while (gbc->cycle_count < target_cycles) {
        uint8_t enabled_interrupts = (uint8_t)(gbc->memory[PYEMU_IO_IE] & 0x1F);
        uint8_t pending_interrupts = pyemu_gbc_pending_interrupts(gbc);
        uint64_t remaining_cycles = target_cycles - gbc->cycle_count;

        if (gbc->cpu.halted && !gbc->faulted && pending_interrupts == 0 && enabled_interrupts == PYEMU_INTERRUPT_VBLANK) {
            uint32_t skip_cycles = pyemu_gbc_cycles_until_vblank(gbc);
            if (skip_cycles > 4U) {
                if ((gbc->memory[PYEMU_IO_TAC] & 0x04) == 0 && (uint64_t)skip_cycles <= remaining_cycles) {
                    pyemu_gbc_fast_forward_to_vblank(gbc);
                    continue;
                }
                if ((uint64_t)skip_cycles > remaining_cycles) {
                    skip_cycles = (uint32_t)remaining_cycles;
                }
                pyemu_gbc_tick(gbc, (int)skip_cycles);
                continue;
            }
        }

        pyemu_gbc_step_instruction_internal(gbc, 0);
        if (gbc->faulted) {
            break;
        }
    }
    pyemu_gbc_update_audio_frame(gbc);

    /* MBC3 RTC: advance by one frame's worth of cycles when not halted */
    if (pyemu_gbc_uses_mbc3(gbc) && !(gbc->rtc_dh & 0x40)) {
        gbc->rtc_cycle_accum += PYEMU_GBC_CYCLES_PER_FRAME;
        while (gbc->rtc_cycle_accum >= 4194304U) {
            gbc->rtc_cycle_accum -= 4194304U;
            gbc->rtc_s++;
            if (gbc->rtc_s >= 60) { gbc->rtc_s = 0; gbc->rtc_m++; }
            if (gbc->rtc_m >= 60) { gbc->rtc_m = 0; gbc->rtc_h++; }
            if (gbc->rtc_h >= 24) {
                gbc->rtc_h = 0;
                {
                    uint16_t days = (uint16_t)(gbc->rtc_dl | ((gbc->rtc_dh & 0x01) << 8));
                    days++;
                    gbc->rtc_dl = (uint8_t)(days & 0xFF);
                    gbc->rtc_dh = (uint8_t)((gbc->rtc_dh & 0xFE) | ((days >> 8) & 0x01));
                    if (days >= 512) {
                        gbc->rtc_dh |= 0x80; /* day carry */
                        gbc->rtc_dl = 0;
                        gbc->rtc_dh &= ~0x01;
                    }
                }
            }
        }
    }
}

static void pyemu_gbc_destroy(pyemu_system* system) {
    pyemu_gbc_system* gbc = (pyemu_gbc_system*)system;
    if (gbc != NULL) {
        if (gbc->battery_dirty) {
            pyemu_gbc_save_battery_ram(gbc);
        }
        free(gbc->rom_data);
    }
    free(system);
}

static void pyemu_gbc_get_cpu_state(const pyemu_system* system, pyemu_cpu_state* out_state) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    if (out_state != NULL) {
        *out_state = gbc->cpu;
        out_state->ime = (uint8_t)(gbc->ime ? 1 : 0);
    }
}

static void pyemu_gbc_get_frame_buffer(const pyemu_system* system, pyemu_frame_buffer* out_frame) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    if (out_frame == NULL) {
        return;
    }
    out_frame->width = PYEMU_GBC_WIDTH;
    out_frame->height = PYEMU_GBC_HEIGHT;
    out_frame->rgba = gbc->frame;
    out_frame->rgba_size = PYEMU_GBC_RGBA_SIZE;
}

static const uint8_t* pyemu_gbc_get_memory(const pyemu_system* system, size_t* size) {
    /* Cast away const to refresh the ERAM window cache before returning the
       flat memory array. This is a read-only cache sync, not a real mutation. */
    pyemu_gbc_system* gbc = (pyemu_gbc_system*)system;
    if (size != NULL) {
        *size = PYEMU_GBC_MEMORY_SIZE;
    }
    pyemu_gbc_refresh_eram_window(gbc);
    return gbc->memory;
}

static void pyemu_gbc_poke_memory(pyemu_system* system, uint16_t address, uint8_t value) {
    pyemu_gbc_system* gbc = (pyemu_gbc_system*)system;
    if (gbc == NULL) {
        return;
    }
    /* For ERAM addresses, bypass ram_enabled check so debugger/tool pokes always
       work regardless of MBC state. Write to both eram[] and the memory[] mirror. */
    if (address >= 0xA000 && address <= 0xBFFF && pyemu_gbc_has_external_ram(gbc)) {
        size_t offset = pyemu_gbc_current_eram_offset(gbc) + (size_t)(address - 0xA000);
        if (offset < gbc->eram_size) {
            gbc->eram[offset] = value;
            gbc->memory[address] = value;
            gbc->battery_dirty = 1;
        }
        return;
    }
    pyemu_gbc_write_memory(gbc, address, value);
}

static uint8_t pyemu_gbc_peek_memory_vtable(pyemu_system* system, uint16_t address) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    if (gbc == NULL) {
        return 0xFF;
    }
    return pyemu_gbc_peek_memory(gbc, address);
}

static int pyemu_gbc_has_rom_loaded(const pyemu_system* system) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    return gbc->rom_loaded;
}

static const char* pyemu_gbc_get_rom_path(const pyemu_system* system) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    return gbc->loaded_rom;
}

static const char* pyemu_gbc_get_cartridge_title(const pyemu_system* system) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    return gbc->cartridge_title;
}

static size_t pyemu_gbc_get_rom_size(const pyemu_system* system) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    return gbc->rom_size;
}

static void pyemu_gbc_get_last_bus_access(const pyemu_system* system, pyemu_last_bus_access* out_access) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    if (out_access != NULL) {
        *out_access = gbc->last_access;
    }
}

static void pyemu_gbc_get_cartridge_debug_info(const pyemu_system* system, pyemu_cartridge_debug_info* out_info) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;

    if (out_info == NULL) {
        return;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->cartridge_type = gbc->cartridge_type;
    out_info->rom_size_code = (gbc->rom_data != NULL && gbc->rom_size > 0x0148) ? gbc->rom_data[0x0148] : 0;
    out_info->ram_size_code = gbc->ram_size_code;
    out_info->ram_enabled = gbc->ram_enabled;
    out_info->rom_bank = pyemu_gbc_current_rom_bank(gbc);
    out_info->ram_bank = (uint8_t)(pyemu_gbc_current_eram_offset(gbc) / PYEMU_GBC_ERAM_BANK_SIZE);
    out_info->banking_mode = pyemu_gbc_uses_mbc1(gbc) ? pyemu_gbc_mbc1_mode(gbc) : 0;
    out_info->has_battery = (uint8_t)(pyemu_gbc_has_battery(gbc) ? 1 : 0);
    out_info->save_file_present = (uint8_t)(pyemu_gbc_save_file_present(gbc) ? 1 : 0);
    out_info->last_mapper_value = gbc->last_mapper_access.value;
    out_info->last_mapper_valid = gbc->last_mapper_access.valid;
    out_info->last_mapper_address = gbc->last_mapper_access.address;
    out_info->rom_bank_count = (uint32_t)gbc->rom_bank_count;
    out_info->ram_bank_count = (uint32_t)gbc->eram_bank_count;
}

static uint64_t pyemu_gbc_get_cycle_count(const pyemu_system* system) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    return gbc->cycle_count;
}

static int pyemu_gbc_is_faulted(const pyemu_system* system) {
    const pyemu_gbc_system* gbc = (const pyemu_gbc_system*)system;
    return gbc->faulted;
}

static const pyemu_system_vtable pyemu_gbc_vtable = {
    pyemu_gbc_name,
    pyemu_gbc_reset,
    pyemu_gbc_load_rom,
    pyemu_gbc_save_state,
    pyemu_gbc_load_state,
    pyemu_gbc_step_instruction,
    pyemu_gbc_step_frame,
    pyemu_gbc_destroy,
    pyemu_gbc_get_cpu_state,
    pyemu_gbc_get_frame_buffer,
    pyemu_gbc_get_audio_buffer,
    pyemu_gbc_get_memory,
    pyemu_gbc_poke_memory,
    pyemu_gbc_peek_memory_vtable,
    pyemu_gbc_has_rom_loaded,
    pyemu_gbc_get_rom_path,
    pyemu_gbc_get_cartridge_title,
    pyemu_gbc_get_rom_size,
    pyemu_gbc_get_cycle_count,
    pyemu_gbc_get_last_bus_access,
    pyemu_gbc_get_cartridge_debug_info,
    pyemu_gbc_is_faulted
};

pyemu_system* pyemu_gbc_create(void) {
    pyemu_gbc_system* gbc = (pyemu_gbc_system*)calloc(1, sizeof(pyemu_gbc_system));
    if (gbc == NULL) {
        return NULL;
    }

    gbc->base.vtable = &pyemu_gbc_vtable;
    pyemu_gbc_reset((pyemu_system*)gbc);
    return (pyemu_system*)gbc;
}
