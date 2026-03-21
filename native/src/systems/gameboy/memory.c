#include "gameboy_internal.h"

/* Memory and bus glue for the DMG core. These helpers are the authoritative way to expose the mirrored 64K view while still enforcing mapper, PPU, and debugger-side rules. */

#include <string.h>

/* Return whether a bus access is interesting enough to surface in debugger hardware traces. */
static int pyemu_gameboy_is_tracked_access(uint16_t address) {
    if (address < 0x8000) {
        return 1;
    }
    if ((address >= 0x8000 && address <= 0x9FFF) || (address >= 0xFE00 && address <= 0xFE9F)) {
        return 1;
    }
    return address >= 0xFF00;
}

/* Capture the last visible bus access for debugger UI without paying that cost when tracking is disabled. */
void pyemu_gameboy_record_access(pyemu_gameboy_system* gb, uint16_t address, uint8_t value, int is_write) {
    if (!gb->bus_tracking_enabled || !pyemu_gameboy_is_tracked_access(address)) {
        return;
    }
    gb->last_access.address = address;
    gb->last_access.value = value;
    gb->last_access.is_write = (uint8_t)(is_write ? 1 : 0);
    gb->last_access.valid = 1;
    if (is_write && address < 0x8000) {
        gb->last_mapper_access = gb->last_access;
    }
}

/* Rebuild the flat 64K memory image from the canonical subsystem buffers after a reset, load, or state restore. */
void pyemu_gameboy_sync_memory(pyemu_gameboy_system* gb) {
    pyemu_gameboy_refresh_rom_mapping(gb);
    memcpy(gb->memory + 0x0000, gb->rom_bank0, sizeof(gb->rom_bank0));
    memcpy(gb->memory + 0x4000, gb->rom_bankx, sizeof(gb->rom_bankx));
    memcpy(gb->memory + 0x8000, gb->vram, sizeof(gb->vram));
    pyemu_gameboy_refresh_eram_window(gb);
    memcpy(gb->memory + 0xC000, gb->wram, sizeof(gb->wram));
    memcpy(gb->memory + 0xE000, gb->wram, sizeof(gb->wram));
    memcpy(gb->memory + 0xFF80, gb->hram, sizeof(gb->hram));
}

/* Read memory without applying CPU bus restrictions or debugger side effects. Used by fetch/decode and internal helpers. */
uint8_t pyemu_gameboy_peek_memory(const pyemu_gameboy_system* gb, uint16_t address) {
    if (address == 0xFF00) {
        return pyemu_gameboy_current_joypad_value(gb);
    }
    if (address >= 0xA000 && address <= 0xBFFF) {
        if (!gb->ram_enabled || !pyemu_gameboy_has_external_ram(gb)) {
            return 0xFF;
        }
        {
            size_t offset = pyemu_gameboy_current_eram_offset(gb) + (size_t)(address - 0xA000);
            return offset < gb->eram_size ? gb->eram[offset] : 0xFF;
        }
    }
    return gb->memory[address];
}

/* Read memory as the CPU would see it, including VRAM/OAM access blocking and trace bookkeeping. */
uint8_t pyemu_gameboy_read_memory(const pyemu_gameboy_system* gb, uint16_t address) {
    uint8_t value;

    if (address >= 0x8000 && address <= 0x9FFF && !pyemu_gameboy_cpu_can_access_vram(gb)) {
        value = 0xFF;
    } else if (address >= 0xFE00 && address <= 0xFE9F && !pyemu_gameboy_cpu_can_access_oam(gb)) {
        value = 0xFF;
    } else {
        value = pyemu_gameboy_peek_memory(gb, address);
    }

    pyemu_gameboy_record_access((pyemu_gameboy_system*)gb, address, value, 0);
    return value;
}

/* Central write path for the DMG core. All stateful writes flow through here so mapper, DMA, timers, PPU, APU, and mirrors stay coherent. */
void pyemu_gameboy_write_memory(pyemu_gameboy_system* gb, uint16_t address, uint8_t value) {
    if (address < 0x8000) {
        pyemu_gameboy_record_access(gb, address, value, 1);
        if (address <= 0x1FFF) {
            if (pyemu_gameboy_uses_mbc1(gb) || pyemu_gameboy_uses_mbc3(gb)) {
                gb->ram_enabled = ((value & 0x0F) == 0x0A) ? 1 : 0;
            }
        } else if (address <= 0x3FFF) {
            if (pyemu_gameboy_uses_mbc1(gb)) {
                gb->mbc1_rom_bank = pyemu_gameboy_normalize_mbc1_bank(gb, value);
            } else if (pyemu_gameboy_uses_mbc3(gb)) {
                gb->mbc3_rom_bank = (uint8_t)(value & 0x7F);
                if (gb->mbc3_rom_bank == 0) {
                    gb->mbc3_rom_bank = 1;
                }
            }
            pyemu_gameboy_refresh_rom_mapping(gb);
            memcpy(gb->memory + 0x0000, gb->rom_bank0, sizeof(gb->rom_bank0));
            memcpy(gb->memory + 0x4000, gb->rom_bankx, sizeof(gb->rom_bankx));
        } else if (address <= 0x5FFF) {
            if (pyemu_gameboy_uses_mbc1(gb)) {
                gb->mbc3_ram_bank = (uint8_t)(value & 0x03);
                pyemu_gameboy_refresh_rom_mapping(gb);
                memcpy(gb->memory + 0x0000, gb->rom_bank0, sizeof(gb->rom_bank0));
                memcpy(gb->memory + 0x4000, gb->rom_bankx, sizeof(gb->rom_bankx));
                pyemu_gameboy_refresh_eram_window(gb);
            } else if (pyemu_gameboy_uses_mbc3(gb) && gb->eram_bank_count > 0) {
                gb->mbc3_ram_bank = (uint8_t)(value & 0x03);
                pyemu_gameboy_refresh_eram_window(gb);
            }
        } else if (address <= 0x7FFF) {
            if (pyemu_gameboy_uses_mbc1(gb)) {
                gb->mbc3_rom_bank = (uint8_t)(value & 0x01);
                pyemu_gameboy_refresh_rom_mapping(gb);
                memcpy(gb->memory + 0x0000, gb->rom_bank0, sizeof(gb->rom_bank0));
                memcpy(gb->memory + 0x4000, gb->rom_bankx, sizeof(gb->rom_bankx));
                pyemu_gameboy_refresh_eram_window(gb);
            }
        }
        return;
    }

    if (address >= 0x8000 && address <= 0x9FFF) {
        pyemu_gameboy_record_access(gb, address, value, 1);
        if (!pyemu_gameboy_cpu_can_access_vram(gb)) {
            return;
        }
        gb->memory[address] = value;
        gb->vram[address - 0x8000] = value;
        return;
    }

    if (address >= 0xFE00 && address <= 0xFE9F) {
        pyemu_gameboy_record_access(gb, address, value, 1);
        if (!pyemu_gameboy_cpu_can_access_oam(gb)) {
            return;
        }
        gb->memory[address] = value;
        return;
    }

    if (address >= 0xA000 && address <= 0xBFFF) {
        if (gb->ram_enabled && pyemu_gameboy_has_external_ram(gb)) {
            size_t offset = pyemu_gameboy_current_eram_offset(gb) + (size_t)(address - 0xA000);
            if (offset < gb->eram_size) {
                gb->eram[offset] = value;
                gb->memory[address] = value;
                gb->battery_dirty = 1;
            }
        }
        pyemu_gameboy_record_access(gb, address, value, 1);
        return;
    }

    if (address == 0xFF00) {
        uint8_t previous_value = pyemu_gameboy_current_joypad_value(gb);
        gb->memory[address] = (uint8_t)(0xC0 | (value & 0x30) | 0x0F);
        pyemu_gameboy_refresh_joypad(gb, previous_value);
        pyemu_gameboy_record_access(gb, address, gb->memory[address], 1);
        pyemu_gameboy_apu_handle_write(gb, address, gb->memory[address]);
        return;
    }
    if (address == PYEMU_IO_DIV) {
        int old_signal = pyemu_gameboy_timer_signal(gb);
        gb->div_counter = 0;
        gb->memory[address] = 0;
        pyemu_gameboy_apply_timer_edge(gb, old_signal, pyemu_gameboy_timer_signal(gb));
        pyemu_gameboy_record_access(gb, address, 0, 1);
        return;
    }
    if (address == PYEMU_IO_LY) {
        gb->ppu_counter = 0;
        gb->memory[address] = 0;
        pyemu_gameboy_update_stat(gb);
        pyemu_gameboy_record_access(gb, address, 0, 1);
        return;
    }
    if (address == PYEMU_IO_TAC) {
        int old_signal = pyemu_gameboy_timer_signal(gb);
        gb->memory[address] = (uint8_t)(value & 0x07);
        pyemu_gameboy_apply_timer_edge(gb, old_signal, pyemu_gameboy_timer_signal(gb));
        pyemu_gameboy_record_access(gb, address, gb->memory[address], 1);
        return;
    }
    if (address == PYEMU_IO_IF) {
        gb->memory[address] = (uint8_t)(0xE0 | (value & 0x1F));
        pyemu_gameboy_record_access(gb, address, gb->memory[address], 1);
        return;
    }
    if (address == PYEMU_IO_STAT) {
        gb->memory[address] = (uint8_t)((value & 0x78) | (gb->memory[address] & 0x07));
        pyemu_gameboy_update_stat(gb);
        pyemu_gameboy_record_access(gb, address, gb->memory[address], 1);
        return;
    }
    if (address == PYEMU_IO_LYC) {
        gb->memory[address] = value;
        pyemu_gameboy_update_stat(gb);
        pyemu_gameboy_record_access(gb, address, gb->memory[address], 1);
        return;
    }
    if (address == PYEMU_IO_LCDC) {
        uint8_t previous = gb->memory[address];
        gb->memory[address] = value;
        if ((previous & 0x80) && !(value & 0x80)) {
            gb->ppu_counter = 0;
            gb->memory[PYEMU_IO_LY] = 0;
            gb->memory[PYEMU_IO_IF] = (uint8_t)(0xE0 | ((gb->memory[PYEMU_IO_IF] & (uint8_t)~(PYEMU_INTERRUPT_VBLANK | PYEMU_INTERRUPT_LCD)) & 0x1F));
            gb->stat_irq_line = 0;
            gb->stat_mode = 0;
            gb->stat_coincidence = 0;
        } else if (!(previous & 0x80) && (value & 0x80)) {
            gb->ppu_counter = 0;
            gb->memory[PYEMU_IO_LY] = 0;
            gb->stat_irq_line = 0;
            gb->stat_mode = 2;
            gb->stat_coincidence = 0;
        }
        pyemu_gameboy_update_stat(gb);
        pyemu_gameboy_record_access(gb, address, value, 1);
        return;
    }
    if (address == PYEMU_IO_DMA) {
        uint16_t source = (uint16_t)(value << 8);
        uint16_t index;
        gb->memory[address] = value;
        pyemu_gameboy_record_access(gb, address, value, 1);
        for (index = 0; index < PYEMU_GAMEBOY_OAM_SIZE; ++index) {
            gb->memory[0xFE00 + index] = pyemu_gameboy_read_memory(gb, (uint16_t)(source + index));
            pyemu_gameboy_record_access(gb, (uint16_t)(0xFE00 + index), gb->memory[0xFE00 + index], 1);
        }
        return;
    }

    gb->memory[address] = value;
    pyemu_gameboy_record_access(gb, address, value, 1);
    pyemu_gameboy_apu_handle_write(gb, address, value);

    if (address >= 0xC000 && address <= 0xDFFF) {
        gb->wram[address - 0xC000] = value;
        if (address <= 0xDDFF) {
            gb->memory[address + 0x2000] = value;
        }
    } else if (address >= 0xE000 && address <= 0xFDFF) {
        if (address <= 0xFDFF) {
            gb->wram[address - 0xE000] = value;
        }
        if (address <= 0xFDFF && address - 0x2000 <= 0xDDFF) {
            gb->memory[address - 0x2000] = value;
        }
    } else if (address >= 0xFF80 && address <= 0xFFFE) {
        gb->hram[address - 0xFF80] = value;
    }
}
