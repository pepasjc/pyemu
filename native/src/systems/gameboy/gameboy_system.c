#include "gameboy_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void pyemu_gameboy_step_instruction_internal(pyemu_gameboy_system* gb, int render_frame);
static void pyemu_gameboy_get_cartridge_debug_info(const pyemu_system* system, pyemu_cartridge_debug_info* out_info);
static int pyemu_gameboy_save_state(const pyemu_system* system, const char* path);
static int pyemu_gameboy_load_state(pyemu_system* system, const char* path);
static int pyemu_gameboy_execute_hotpath(pyemu_gameboy_system* gb, int* out_cycles);
static uint8_t pyemu_gameboy_current_bank_for_address(const pyemu_gameboy_system* gb, uint16_t address);
static pyemu_block_cache_entry* pyemu_gameboy_get_block_cache_entry(pyemu_gameboy_system* gb, uint16_t pc, uint8_t bank);
static int pyemu_gameboy_decode_block(pyemu_gameboy_system* gb, pyemu_block_cache_entry* entry, uint16_t pc, uint8_t bank);
static int pyemu_gameboy_execute_block(pyemu_gameboy_system* gb, const pyemu_block_cache_entry* entry, int* out_cycles);
static int pyemu_gameboy_execute_opcode(pyemu_gameboy_system* gb, uint8_t opcode);
static const char* pyemu_gameboy_name(const pyemu_system* system);
static void pyemu_gameboy_reset(pyemu_system* system);
static int pyemu_gameboy_load_rom(pyemu_system* system, const char* path);
static void pyemu_gameboy_step_instruction(pyemu_system* system);
static void pyemu_gameboy_step_frame(pyemu_system* system);
static void pyemu_gameboy_destroy(pyemu_system* system);

static int pyemu_gameboy_is_tracked_access(uint16_t address) {
    if (address < 0x8000) {
        return 1;
    }
    if ((address >= 0x8000 && address <= 0x9FFF) || (address >= 0xFE00 && address <= 0xFE9F)) {
        return 1;
    }
    return address >= 0xFF00;
}

static void pyemu_gameboy_record_access(pyemu_gameboy_system* gb, uint16_t address, uint8_t value, int is_write) {
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

static void pyemu_gameboy_sync_memory(pyemu_gameboy_system* gb) {
    pyemu_gameboy_refresh_rom_mapping(gb);
    memcpy(gb->memory + 0x0000, gb->rom_bank0, sizeof(gb->rom_bank0));
    memcpy(gb->memory + 0x4000, gb->rom_bankx, sizeof(gb->rom_bankx));
    memcpy(gb->memory + 0x8000, gb->vram, sizeof(gb->vram));
    pyemu_gameboy_refresh_eram_window(gb);
    memcpy(gb->memory + 0xC000, gb->wram, sizeof(gb->wram));
    memcpy(gb->memory + 0xE000, gb->wram, sizeof(gb->wram));
    memcpy(gb->memory + 0xFF80, gb->hram, sizeof(gb->hram));
}

uint8_t pyemu_gameboy_peek_memory(const pyemu_gameboy_system* gb, uint16_t address) {
    if (address == 0xFF00) {
        return pyemu_gameboy_current_joypad_value(gb);
    }
    if (address >= 0xA000 && address <= 0xBFFF) {
        if (!gb->ram_enabled || !pyemu_gameboy_has_external_ram(gb)) {
            return 0xFF;
        }
        size_t offset = pyemu_gameboy_current_eram_offset(gb) + (size_t)(address - 0xA000);
        return offset < gb->eram_size ? gb->eram[offset] : 0xFF;
    }
    return gb->memory[address];
}

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

/*
 * All CPU-visible writes funnel through this helper so mapper changes, IO side
 * effects, save-RAM updates and LCD access restrictions stay centralized.
 */
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

static void pyemu_gameboy_extract_title(pyemu_gameboy_system* gb) {
    size_t title_length = 0;
    size_t index;

    memset(gb->cartridge_title, 0, sizeof(gb->cartridge_title));
    for (index = PYEMU_GAMEBOY_TITLE_START; index <= PYEMU_GAMEBOY_TITLE_END; ++index) {
        uint8_t value = gb->rom_bank0[index];
        if (value == 0) {
            break;
        }
        if (title_length + 1 >= sizeof(gb->cartridge_title)) {
            break;
        }
        gb->cartridge_title[title_length] = (char)value;
        title_length += 1;
    }

    if (title_length == 0) {
        strncpy(gb->cartridge_title, "Unknown", sizeof(gb->cartridge_title) - 1);
    }
}

static void pyemu_gameboy_set_post_boot_state(pyemu_gameboy_system* gb) {
    pyemu_gameboy_set_af(gb, 0x01B0);
    pyemu_gameboy_set_bc(gb, 0x0013);
    pyemu_gameboy_set_de(gb, 0x00D8);
    pyemu_gameboy_set_hl(gb, 0x014D);
    gb->cpu.sp = 0xFFFE;
    gb->cpu.pc = 0x0100;
    gb->cpu.halted = 0;
    gb->faulted = 0;
    gb->ime = 0;
    gb->ime_pending = 0;
    gb->cycle_count = 0;
    gb->div_counter = 0;
    gb->timer_counter = 0;
    gb->ppu_counter = 0;
    gb->joypad_buttons = 0x0F;
    gb->joypad_directions = 0x0F;
    gb->ram_enabled = 0;

    gb->memory[PYEMU_IO_DIV] = 0xAB;
    pyemu_gameboy_write_memory(gb, 0xFF00, 0xCF);
    pyemu_gameboy_write_memory(gb, PYEMU_IO_TIMA, 0x00);
    pyemu_gameboy_write_memory(gb, PYEMU_IO_TMA, 0x00);
    pyemu_gameboy_write_memory(gb, PYEMU_IO_TAC, 0x00);
    pyemu_gameboy_write_memory(gb, PYEMU_IO_IF, 0xE1);
    pyemu_gameboy_write_memory(gb, 0xFF10, 0x80);
    pyemu_gameboy_write_memory(gb, 0xFF11, 0xBF);
    pyemu_gameboy_write_memory(gb, 0xFF12, 0xF3);
    pyemu_gameboy_write_memory(gb, 0xFF14, 0xBF);
    pyemu_gameboy_write_memory(gb, 0xFF16, 0x3F);
    pyemu_gameboy_write_memory(gb, 0xFF17, 0x00);
    pyemu_gameboy_write_memory(gb, 0xFF19, 0xBF);
    pyemu_gameboy_write_memory(gb, 0xFF1A, 0x7F);
    pyemu_gameboy_write_memory(gb, 0xFF1B, 0xFF);
    pyemu_gameboy_write_memory(gb, 0xFF1C, 0x9F);
    pyemu_gameboy_write_memory(gb, 0xFF1E, 0xBF);
    pyemu_gameboy_write_memory(gb, 0xFF20, 0xFF);
    pyemu_gameboy_write_memory(gb, 0xFF21, 0x00);
    pyemu_gameboy_write_memory(gb, 0xFF22, 0x00);
    pyemu_gameboy_write_memory(gb, 0xFF23, 0xBF);
    pyemu_gameboy_write_memory(gb, 0xFF24, 0x77);
    pyemu_gameboy_write_memory(gb, 0xFF25, 0xF3);
    pyemu_gameboy_write_memory(gb, 0xFF26, 0xF1);
    pyemu_gameboy_write_memory(gb, 0xFF40, 0x91);
    pyemu_gameboy_write_memory(gb, 0xFF41, 0x85);
    pyemu_gameboy_update_stat(gb);
    pyemu_gameboy_write_memory(gb, 0xFF42, 0x00);
    pyemu_gameboy_write_memory(gb, 0xFF43, 0x00);
    pyemu_gameboy_write_memory(gb, PYEMU_IO_LY, 0x00);
    pyemu_gameboy_write_memory(gb, 0xFF45, 0x00);
    pyemu_gameboy_write_memory(gb, 0xFF47, 0xFC);
    pyemu_gameboy_write_memory(gb, 0xFF48, 0xFF);
    pyemu_gameboy_write_memory(gb, 0xFF49, 0xFF);
    pyemu_gameboy_write_memory(gb, 0xFF4A, 0x00);
    pyemu_gameboy_write_memory(gb, 0xFF4B, 0x00);
    pyemu_gameboy_write_memory(gb, PYEMU_IO_IE, 0x00);
}

static const char* pyemu_gameboy_name(const pyemu_system* system) {
    (void)system;
    return "gameboy";
}

static void pyemu_gameboy_reset(pyemu_system* system) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    memset(gb->memory, 0, sizeof(gb->memory));
    memset(gb->frame, 0, sizeof(gb->frame));
    memset(gb->vram, 0, sizeof(gb->vram));
    memset(gb->wram, 0, sizeof(gb->wram));
    memset(gb->hram, 0, sizeof(gb->hram));
    memset(gb->line_scx, 0, sizeof(gb->line_scx));
    memset(gb->line_scy, 0, sizeof(gb->line_scy));
    memset(gb->line_wx, 0, sizeof(gb->line_wx));
    memset(gb->line_wy, 0, sizeof(gb->line_wy));
    memset(gb->line_lcdc, 0, sizeof(gb->line_lcdc));
    memset(gb->line_bgp, 0, sizeof(gb->line_bgp));
    memset(gb->line_obp0, 0, sizeof(gb->line_obp0));
    memset(gb->line_obp1, 0, sizeof(gb->line_obp1));
    memset(gb->block_cache, 0, sizeof(gb->block_cache));
    memset(&gb->cpu, 0, sizeof(gb->cpu));
    gb->last_opcode = 0;
    gb->stat_irq_line = 0;
    memset(&gb->last_access, 0, sizeof(gb->last_access));
    memset(&gb->last_mapper_access, 0, sizeof(gb->last_mapper_access));
    gb->bus_tracking_enabled = 1;

    if (!gb->rom_loaded) {
        memset(gb->rom_bank0, 0, sizeof(gb->rom_bank0));
        memset(gb->rom_bankx, 0, sizeof(gb->rom_bankx));
        memset(gb->loaded_rom, 0, sizeof(gb->loaded_rom));
        memset(gb->cartridge_title, 0, sizeof(gb->cartridge_title));
        memset(gb->eram, 0, sizeof(gb->eram));
        gb->rom_size = 0;
        gb->rom_bank_count = 0;
        gb->cartridge_type = 0;
        gb->ram_size_code = 0;
        gb->eram_size = 0;
        gb->eram_bank_count = 0;
        gb->ram_enabled = 0;
        gb->mbc1_rom_bank = 1;
        gb->mbc3_rom_bank = 0;
        gb->mbc3_ram_bank = 0;
    } else {
        gb->ram_enabled = 0;
        gb->mbc1_rom_bank = 1;
        gb->mbc3_rom_bank = pyemu_gameboy_uses_mbc1(gb) ? 0 : 1;
        gb->mbc3_ram_bank = 0;
    }

    pyemu_gameboy_sync_memory(gb);
    pyemu_gameboy_set_post_boot_state(gb);
    pyemu_gameboy_update_demo_frame(gb);
}

static int pyemu_gameboy_load_rom(pyemu_system* system, const char* path) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
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

    free(gb->rom_data);
    gb->rom_data = rom_data;
    gb->rom_size = (size_t)file_size;
    gb->rom_bank_count = (gb->rom_size + (PYEMU_GAMEBOY_ROM_BANKX_SIZE - 1)) / PYEMU_GAMEBOY_ROM_BANKX_SIZE;
    gb->cartridge_type = gb->rom_size > 0x0147 ? gb->rom_data[0x0147] : 0;
    gb->ram_size_code = gb->rom_size > 0x0149 ? gb->rom_data[0x0149] : 0;
    gb->eram_size = pyemu_gameboy_eram_size_from_code(gb->ram_size_code);
    if (gb->eram_size > sizeof(gb->eram)) {
        gb->eram_size = sizeof(gb->eram);
    }
    gb->eram_bank_count = gb->eram_size == 0 ? 0 : ((gb->eram_size + PYEMU_GAMEBOY_ERAM_BANK_SIZE - 1) / PYEMU_GAMEBOY_ERAM_BANK_SIZE);
    gb->ram_enabled = 0;
    gb->mbc1_rom_bank = 1;
    gb->mbc3_rom_bank = pyemu_gameboy_uses_mbc1(gb) ? 0 : 1;
    gb->mbc3_ram_bank = 0;
    gb->rom_loaded = 1;
    strncpy(gb->loaded_rom, path, sizeof(gb->loaded_rom) - 1);
    gb->loaded_rom[sizeof(gb->loaded_rom) - 1] = '\0';
    memset(gb->eram, 0, sizeof(gb->eram));
    pyemu_gameboy_load_battery_ram(gb);
    pyemu_gameboy_refresh_rom_mapping(gb);
    pyemu_gameboy_extract_title(gb);
    pyemu_gameboy_reset(system);
    return 1;
}

static int pyemu_gameboy_save_state(const pyemu_system* system, const char* path) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    pyemu_gameboy_state_file snapshot;
    FILE* state_file;

    if (gb == NULL || !gb->rom_loaded || path == NULL || path[0] == 0) {
        return 0;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.magic = PYEMU_GAMEBOY_STATE_MAGIC;
    snapshot.version = PYEMU_GAMEBOY_STATE_VERSION;
    snapshot.cpu = gb->cpu;
    memcpy(snapshot.memory, gb->memory, sizeof(snapshot.memory));
    memcpy(snapshot.vram, gb->vram, sizeof(snapshot.vram));
    memcpy(snapshot.wram, gb->wram, sizeof(snapshot.wram));
    memcpy(snapshot.hram, gb->hram, sizeof(snapshot.hram));
    memcpy(snapshot.eram, gb->eram, sizeof(snapshot.eram));
    memcpy(snapshot.line_scx, gb->line_scx, sizeof(snapshot.line_scx));
    memcpy(snapshot.line_scy, gb->line_scy, sizeof(snapshot.line_scy));
    memcpy(snapshot.line_wx, gb->line_wx, sizeof(snapshot.line_wx));
    memcpy(snapshot.line_wy, gb->line_wy, sizeof(snapshot.line_wy));
    memcpy(snapshot.line_lcdc, gb->line_lcdc, sizeof(snapshot.line_lcdc));
    memcpy(snapshot.line_bgp, gb->line_bgp, sizeof(snapshot.line_bgp));
    memcpy(snapshot.line_obp0, gb->line_obp0, sizeof(snapshot.line_obp0));
    memcpy(snapshot.line_obp1, gb->line_obp1, sizeof(snapshot.line_obp1));
    memcpy(snapshot.loaded_rom, gb->loaded_rom, sizeof(snapshot.loaded_rom));
    memcpy(snapshot.cartridge_title, gb->cartridge_title, sizeof(snapshot.cartridge_title));
    snapshot.rom_size = gb->rom_size;
    snapshot.rom_bank_count = gb->rom_bank_count;
    snapshot.rom_loaded = gb->rom_loaded;
    snapshot.cartridge_type = gb->cartridge_type;
    snapshot.ram_size_code = gb->ram_size_code;
    snapshot.eram_size = gb->eram_size;
    snapshot.eram_bank_count = gb->eram_bank_count;
    snapshot.ram_enabled = gb->ram_enabled;
    snapshot.mbc1_rom_bank = gb->mbc1_rom_bank;
    snapshot.mbc3_rom_bank = gb->mbc3_rom_bank;
    snapshot.mbc3_ram_bank = gb->mbc3_ram_bank;
    snapshot.ime = gb->ime;
    snapshot.ime_pending = gb->ime_pending;
    snapshot.last_opcode = gb->last_opcode;
    snapshot.cycle_count = gb->cycle_count;
    snapshot.div_counter = gb->div_counter;
    snapshot.timer_counter = gb->timer_counter;
    snapshot.ppu_counter = gb->ppu_counter;
    snapshot.joypad_buttons = gb->joypad_buttons;
    snapshot.joypad_directions = gb->joypad_directions;
    snapshot.stat_coincidence = gb->stat_coincidence;
    snapshot.stat_mode = gb->stat_mode;
    snapshot.last_access = gb->last_access;
    snapshot.faulted = gb->faulted;

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

static int pyemu_gameboy_load_state(pyemu_system* system, const char* path) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    pyemu_gameboy_state_file snapshot;
    FILE* state_file;

    if (gb == NULL || !gb->rom_loaded || path == NULL || path[0] == 0) {
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

    if (snapshot.magic != PYEMU_GAMEBOY_STATE_MAGIC || snapshot.version != PYEMU_GAMEBOY_STATE_VERSION) {
        return 0;
    }
    if (!snapshot.rom_loaded || strcmp(snapshot.loaded_rom, gb->loaded_rom) != 0 || strcmp(snapshot.cartridge_title, gb->cartridge_title) != 0) {
        return 0;
    }

    gb->cpu = snapshot.cpu;
    memcpy(gb->memory, snapshot.memory, sizeof(gb->memory));
    memcpy(gb->vram, snapshot.vram, sizeof(gb->vram));
    memcpy(gb->wram, snapshot.wram, sizeof(gb->wram));
    memcpy(gb->hram, snapshot.hram, sizeof(gb->hram));
    memcpy(gb->eram, snapshot.eram, sizeof(gb->eram));
    memcpy(gb->line_scx, snapshot.line_scx, sizeof(gb->line_scx));
    memcpy(gb->line_scy, snapshot.line_scy, sizeof(gb->line_scy));
    memcpy(gb->line_wx, snapshot.line_wx, sizeof(gb->line_wx));
    memcpy(gb->line_wy, snapshot.line_wy, sizeof(gb->line_wy));
    memcpy(gb->line_lcdc, snapshot.line_lcdc, sizeof(gb->line_lcdc));
    memcpy(gb->line_bgp, snapshot.line_bgp, sizeof(gb->line_bgp));
    memcpy(gb->line_obp0, snapshot.line_obp0, sizeof(gb->line_obp0));
    memcpy(gb->line_obp1, snapshot.line_obp1, sizeof(gb->line_obp1));
    gb->rom_size = snapshot.rom_size;
    gb->rom_bank_count = snapshot.rom_bank_count;
    gb->rom_loaded = snapshot.rom_loaded;
    gb->cartridge_type = snapshot.cartridge_type;
    gb->ram_size_code = snapshot.ram_size_code;
    gb->eram_size = snapshot.eram_size;
    gb->eram_bank_count = snapshot.eram_bank_count;
    gb->ram_enabled = snapshot.ram_enabled;
    gb->mbc1_rom_bank = snapshot.mbc1_rom_bank;
    gb->mbc3_rom_bank = snapshot.mbc3_rom_bank;
    gb->mbc3_ram_bank = snapshot.mbc3_ram_bank;
    gb->ime = snapshot.ime;
    gb->ime_pending = snapshot.ime_pending;
    gb->ime_delay = 0;
    gb->halt_bug = 0;
    gb->last_opcode = snapshot.last_opcode;
    gb->cycle_count = snapshot.cycle_count;
    gb->div_counter = snapshot.div_counter;
    gb->timer_counter = snapshot.timer_counter;
    gb->ppu_counter = snapshot.ppu_counter;
    gb->joypad_buttons = snapshot.joypad_buttons;
    gb->joypad_directions = snapshot.joypad_directions;
    gb->stat_coincidence = snapshot.stat_coincidence;
    gb->stat_mode = snapshot.stat_mode;
    gb->stat_irq_line = 0;
    gb->last_access = snapshot.last_access;
    memset(&gb->last_mapper_access, 0, sizeof(gb->last_mapper_access));
    gb->faulted = snapshot.faulted;

    pyemu_gameboy_refresh_rom_mapping(gb);
    pyemu_gameboy_update_stat(gb);
    pyemu_gameboy_update_demo_frame(gb);
    return 1;
}

static int pyemu_gameboy_execute_opcode(pyemu_gameboy_system* gb, uint8_t opcode) {
    int cycles = 4;
    gb->last_opcode = opcode;

    if (opcode >= 0x40 && opcode <= 0x7F && opcode != 0x76) {
        int dst = (opcode >> 3) & 0x07;
        int src = opcode & 0x07;
        uint8_t value = pyemu_gameboy_read_r8(gb, src);
        pyemu_gameboy_write_r8(gb, dst, value);
        return (src == 6 || dst == 6) ? 8 : 4;
    }

    cycles = pyemu_gameboy_execute_load_store(gb, opcode);
    if (cycles >= 0) {
        return cycles;
    }
    cycles = pyemu_gameboy_execute_control_flow(gb, opcode);
    if (cycles >= 0) {
        return cycles;
    }
    cycles = pyemu_gameboy_execute_alu(gb, opcode);
    if (cycles >= 0) {
        return cycles;
    }

    switch (opcode) {
        case 0x00:
            cycles = 4;
            break;
        case 0x76: {
            uint8_t pending_interrupts = pyemu_gameboy_pending_interrupts(gb);
            if (!gb->ime && pending_interrupts != 0) {
                gb->halt_bug = 1;
                gb->cpu.halted = 0;
            } else {
                gb->cpu.halted = 1;
            }
            cycles = 4;
            break;
        }
        case 0xE0: {
            uint16_t address = (uint16_t)(0xFF00 | pyemu_gameboy_fetch_u8(gb));
            pyemu_gameboy_write_memory(gb, address, gb->cpu.a);
            cycles = 12;
            break;
        }
        case 0xE2:
            pyemu_gameboy_write_memory(gb, (uint16_t)(0xFF00 | gb->cpu.c), gb->cpu.a);
            cycles = 8;
            break;
        case 0xE8:
            pyemu_gameboy_add_sp_signed(gb, (int8_t)pyemu_gameboy_fetch_u8(gb));
            cycles = 16;
            break;
        case 0xEA: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            pyemu_gameboy_write_memory(gb, address, gb->cpu.a);
            cycles = 16;
            break;
        }
        case 0xF0: {
            uint16_t address = (uint16_t)(0xFF00 | pyemu_gameboy_fetch_u8(gb));
            gb->cpu.a = pyemu_gameboy_read_memory(gb, address);
            cycles = 12;
            break;
        }
        case 0xF2:
            gb->cpu.a = pyemu_gameboy_read_memory(gb, (uint16_t)(0xFF00 | gb->cpu.c));
            cycles = 8;
            break;
        case 0xCB:
            cycles = pyemu_gameboy_execute_cb(gb);
            break;
        case 0xF3:
            gb->ime = 0;
            gb->ime_pending = 0;
            gb->ime_delay = 0;
            cycles = 4;
            break;
        case 0xF8:
            pyemu_gameboy_set_hl(gb, pyemu_gameboy_sp_plus_signed(gb, (int8_t)pyemu_gameboy_fetch_u8(gb)));
            cycles = 12;
            break;
        case 0xFA: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            gb->cpu.a = pyemu_gameboy_read_memory(gb, address);
            cycles = 16;
            break;
        }
        case 0xFB:
            gb->ime_pending = 1;
            gb->ime_delay = 1;
            cycles = 4;
            break;
        default:
            gb->cpu.halted = 1;
            gb->faulted = 1;
            gb->cpu.pc = (uint16_t)(gb->cpu.pc - 1);
            cycles = 4;
            break;
    }

    return cycles;
}

static uint8_t pyemu_gameboy_current_bank_for_address(const pyemu_gameboy_system* gb, uint16_t address) {
    if (address < 0x4000) {
        if (pyemu_gameboy_uses_mbc1(gb) && pyemu_gameboy_mbc1_mode(gb)) {
            uint8_t bank = (uint8_t)(pyemu_gameboy_mbc1_upper_bits(gb) << 5);
            if (gb->rom_bank_count > 0) {
                bank = (uint8_t)(bank % gb->rom_bank_count);
            }
            return bank;
        }
        return 0;
    }
    if (address < 0x8000) {
        return pyemu_gameboy_current_rom_bank(gb);
    }
    return 0xFF;
}

static pyemu_block_cache_entry* pyemu_gameboy_get_block_cache_entry(pyemu_gameboy_system* gb, uint16_t pc, uint8_t bank) {
    uint32_t index = ((((uint32_t)bank) << 16) ^ pc) & (PYEMU_GAMEBOY_BLOCK_CACHE_SIZE - 1);
    return &gb->block_cache[index];
}

static int pyemu_gameboy_decode_block(pyemu_gameboy_system* gb, pyemu_block_cache_entry* entry, uint16_t pc, uint8_t bank) {
    uint16_t cursor = pc;
    uint8_t count = 0;

    memset(entry, 0, sizeof(*entry));
    entry->pc = pc;
    entry->bank = bank;

    while (count < PYEMU_GAMEBOY_BLOCK_MAX_INSNS && cursor < 0x8000) {
        pyemu_block_insn* insn = &entry->insns[count];
        uint8_t opcode = pyemu_gameboy_peek_memory(gb, cursor);
        insn->length = 1;

        switch (opcode) {
            case 0x00: insn->type = PYEMU_BLOCK_OP_NOP; break;
            case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E:
                insn->type = PYEMU_BLOCK_OP_LD_R_D8;
                insn->operand8 = pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1));
                insn->length = 2;
                break;
            case 0x11:
                insn->type = PYEMU_BLOCK_OP_LD_DE_D16;
                insn->operand16 = (uint16_t)(pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0x21:
                insn->type = PYEMU_BLOCK_OP_LD_HL_D16;
                insn->operand16 = (uint16_t)(pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0x18: insn->type = PYEMU_BLOCK_OP_JR; insn->relative = (int8_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x20: insn->type = PYEMU_BLOCK_OP_JR_NZ; insn->relative = (int8_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x28: insn->type = PYEMU_BLOCK_OP_JR_Z; insn->relative = (int8_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x30: insn->type = PYEMU_BLOCK_OP_JR_NC; insn->relative = (int8_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x38: insn->type = PYEMU_BLOCK_OP_JR_C; insn->relative = (int8_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x05: insn->type = PYEMU_BLOCK_OP_DEC_B; break;
            case 0x22: insn->type = PYEMU_BLOCK_OP_LD_HL_INC_A; break;
            case 0x2A: insn->type = PYEMU_BLOCK_OP_LD_A_HL_INC; break;
            case 0x32: insn->type = PYEMU_BLOCK_OP_LD_HL_DEC_A; break;
            case 0x3A: insn->type = PYEMU_BLOCK_OP_LD_A_HL_DEC; break;
            case 0x3C: insn->type = PYEMU_BLOCK_OP_INC_A; break;
            case 0x6F: insn->type = PYEMU_BLOCK_OP_LD_L_A; break;
            case 0x7D: insn->type = PYEMU_BLOCK_OP_LD_A_L; break;
            case 0x76: insn->type = PYEMU_BLOCK_OP_HALT; entry->terminates = 1; break;
            case 0xA7: insn->type = PYEMU_BLOCK_OP_AND_A; break;
            case 0xAF: insn->type = PYEMU_BLOCK_OP_XOR_A; break;
            case 0xC9: insn->type = PYEMU_BLOCK_OP_RET; entry->terminates = 1; break;
            case 0xE0:
                insn->type = PYEMU_BLOCK_OP_LDH_A8_A;
                insn->operand8 = pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1));
                insn->length = 2;
                break;
            case 0xEA:
                insn->type = PYEMU_BLOCK_OP_LD_A16_A;
                insn->operand16 = (uint16_t)(pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0xF0:
                insn->type = PYEMU_BLOCK_OP_LDH_A_A8;
                insn->operand8 = pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1));
                insn->length = 2;
                break;
            case 0xFA:
                insn->type = PYEMU_BLOCK_OP_LD_A_A16;
                insn->operand16 = (uint16_t)(pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0xFE:
                insn->type = PYEMU_BLOCK_OP_CP_D8;
                insn->operand8 = pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1));
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

static int pyemu_gameboy_execute_block(pyemu_gameboy_system* gb, const pyemu_block_cache_entry* entry, int* out_cycles) {
    int cycles = 0;
    uint8_t index;

    gb->cpu.pc = entry->pc;
    for (index = 0; index < entry->insn_count; ++index) {
        const pyemu_block_insn* insn = &entry->insns[index];
        uint16_t opcode_pc = gb->cpu.pc;
        uint16_t next_pc = (uint16_t)(opcode_pc + insn->length);
        uint8_t opcode = pyemu_gameboy_peek_memory(gb, opcode_pc);

        switch (insn->type) {
            case PYEMU_BLOCK_OP_NOP: gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_LD_R_D8:
                switch (opcode) {
                    case 0x06: gb->cpu.b = insn->operand8; break;
                    case 0x0E: gb->cpu.c = insn->operand8; break;
                    case 0x16: gb->cpu.d = insn->operand8; break;
                    case 0x1E: gb->cpu.e = insn->operand8; break;
                    case 0x26: gb->cpu.h = insn->operand8; break;
                    case 0x2E: gb->cpu.l = insn->operand8; break;
                    default: gb->cpu.a = insn->operand8; break;
                }
                gb->cpu.pc = next_pc; cycles += 8; break;
            case PYEMU_BLOCK_OP_LD_HL_D16: pyemu_gameboy_set_hl(gb, insn->operand16); gb->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_BLOCK_OP_LD_DE_D16: pyemu_gameboy_set_de(gb, insn->operand16); gb->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_BLOCK_OP_LDH_A_A8: gb->cpu.a = pyemu_gameboy_read_memory(gb, (uint16_t)(0xFF00 | insn->operand8)); gb->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_BLOCK_OP_LDH_A8_A: pyemu_gameboy_write_memory(gb, (uint16_t)(0xFF00 | insn->operand8), gb->cpu.a); gb->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_BLOCK_OP_LD_A_A16: gb->cpu.a = pyemu_gameboy_read_memory(gb, insn->operand16); gb->cpu.pc = next_pc; cycles += 16; break;
            case PYEMU_BLOCK_OP_LD_A16_A: pyemu_gameboy_write_memory(gb, insn->operand16, gb->cpu.a); gb->cpu.pc = next_pc; cycles += 16; break;
            case PYEMU_BLOCK_OP_XOR_A: pyemu_gameboy_xor_a(gb, gb->cpu.a); gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_AND_A: pyemu_gameboy_and_a(gb, gb->cpu.a); gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_INC_A: gb->cpu.a = pyemu_gameboy_inc8(gb, gb->cpu.a); gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_CP_D8: pyemu_gameboy_cp_a(gb, insn->operand8); gb->cpu.pc = next_pc; cycles += 8; break;
            case PYEMU_BLOCK_OP_LD_HL_INC_A: { uint16_t hl = pyemu_gameboy_get_hl(gb); pyemu_gameboy_write_memory(gb, hl, gb->cpu.a); pyemu_gameboy_set_hl(gb, (uint16_t)(hl + 1)); gb->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_BLOCK_OP_LD_A_HL_INC: { uint16_t hl = pyemu_gameboy_get_hl(gb); gb->cpu.a = pyemu_gameboy_read_memory(gb, hl); pyemu_gameboy_set_hl(gb, (uint16_t)(hl + 1)); gb->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_BLOCK_OP_LD_A_HL_DEC: { uint16_t hl = pyemu_gameboy_get_hl(gb); gb->cpu.a = pyemu_gameboy_read_memory(gb, hl); pyemu_gameboy_set_hl(gb, (uint16_t)(hl - 1)); gb->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_BLOCK_OP_LD_HL_DEC_A: { uint16_t hl = pyemu_gameboy_get_hl(gb); pyemu_gameboy_write_memory(gb, hl, gb->cpu.a); pyemu_gameboy_set_hl(gb, (uint16_t)(hl - 1)); gb->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_BLOCK_OP_DEC_B: gb->cpu.b = pyemu_gameboy_dec8(gb, gb->cpu.b); gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_LD_A_L: gb->cpu.a = gb->cpu.l; gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_LD_L_A: gb->cpu.l = gb->cpu.a; gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_JR: gb->cpu.pc = (uint16_t)(next_pc + insn->relative); cycles += 12; *out_cycles = cycles; return 1;
            case PYEMU_BLOCK_OP_JR_NZ: { int cond = !pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z); gb->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_BLOCK_OP_JR_Z: { int cond = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z); gb->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_BLOCK_OP_JR_NC: { int cond = !pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C); gb->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_BLOCK_OP_JR_C: { int cond = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C); gb->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_BLOCK_OP_RET: gb->cpu.pc = pyemu_gameboy_pop_u16(gb); cycles += 16; *out_cycles = cycles; return 1;
            case PYEMU_BLOCK_OP_HALT: {
                uint8_t pending_interrupts = pyemu_gameboy_pending_interrupts(gb);
                if (!gb->ime && pending_interrupts != 0) {
                    gb->halt_bug = 1;
                    gb->cpu.halted = 0;
                } else {
                    gb->cpu.halted = 1;
                }
                gb->cpu.pc = next_pc;
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

static int pyemu_gameboy_execute_hotpath(pyemu_gameboy_system* gb, int* out_cycles) {
    uint16_t pc = gb->cpu.pc;

    if (pc <= 0xFFF6U) {
        uint8_t b0 = pyemu_gameboy_peek_memory(gb, pc);
        uint8_t b1 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 1));
        uint8_t b2 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 2));
        uint8_t b3 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 3));
        uint8_t b4 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 4));
        uint8_t b5 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 5));
        uint8_t b6 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 6));
        uint8_t b7 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 7));
        uint8_t b8 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 8));
        uint8_t b9 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 9));

        if (b0 == 0x21 && b3 == 0x06 && b5 == 0x22 && b6 == 0x05 && b7 == 0x20 && b8 == 0xFC && b9 == 0xC9) {
            uint16_t address = (uint16_t)(b1 | ((uint16_t)b2 << 8));
            uint8_t count = b4;
            uint8_t value = gb->cpu.a;
            uint16_t index;

            for (index = 0; index < count; ++index) {
                pyemu_gameboy_write_memory(gb, (uint16_t)(address + index), value);
            }

            pyemu_gameboy_set_hl(gb, (uint16_t)(address + count));
            gb->cpu.b = 0;
            gb->cpu.f = (uint8_t)((gb->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            *out_cycles = (int)(24U * (uint32_t)count + 32U);
            return 1;
        }

        if (b0 == 0xAF && b1 == 0x22 && b2 == 0x7D && b3 == 0xFE && b5 == 0x38 && b6 == 0xF9 && b7 == 0xC9) {
            uint8_t limit = b4;
            uint16_t address = pyemu_gameboy_get_hl(gb);
            uint8_t start_low = gb->cpu.l;
            uint8_t count = limit > start_low ? (uint8_t)(limit - start_low) : 0;
            uint16_t index;

            gb->cpu.a = 0;
            if (count == 0) {
                gb->cpu.f = (uint8_t)(PYEMU_FLAG_Z | PYEMU_FLAG_N);
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                *out_cycles = 24;
                return 1;
            }

            for (index = 0; index < count; ++index) {
                pyemu_gameboy_write_memory(gb, (uint16_t)(address + index), 0);
            }

            pyemu_gameboy_set_hl(gb, (uint16_t)(address + count));
            gb->cpu.a = limit;
            gb->cpu.f = (uint8_t)(PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            *out_cycles = (int)(32U * (uint32_t)count + 16U);
            return 1;
        }

        if (b0 == 0x22 && b1 == 0x05 && b2 == 0x20 && b3 == 0xFC && b4 == 0xC9) {
            uint16_t address = pyemu_gameboy_get_hl(gb);
            uint8_t count = gb->cpu.b;
            uint8_t value = gb->cpu.a;
            uint16_t index;

            if (count == 0) {
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                *out_cycles = 16;
                return 1;
            }

            for (index = 0; index < count; ++index) {
                pyemu_gameboy_write_memory(gb, (uint16_t)(address + index), value);
            }

            pyemu_gameboy_set_hl(gb, (uint16_t)(address + count));
            gb->cpu.b = 0;
            gb->cpu.f = (uint8_t)((gb->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            *out_cycles = (int)(20U * (uint32_t)count + 16U);
            return 1;
        }

        if (b0 == 0x3D && b1 == 0x20 && b2 == 0xFD && b3 == 0xC9) {
            uint8_t count = gb->cpu.a;
            if (count == 0) {
                gb->cpu.f = (uint8_t)((gb->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                *out_cycles = 16;
                return 1;
            }
            gb->cpu.a = 0;
            gb->cpu.f = (uint8_t)((gb->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            *out_cycles = (int)(12U * (uint32_t)count + 4U);
            return 1;
        }
    }

    return 0;
}

static void pyemu_gameboy_step_instruction_internal(pyemu_gameboy_system* gb, int render_frame) {
    uint8_t opcode;
    int cycles;

    if (gb->faulted) {
        if (render_frame) {
            pyemu_gameboy_update_demo_frame(gb);
        }
        return;
    }

    if (pyemu_gameboy_service_interrupts(gb) > 0) {
        if (render_frame) {
            pyemu_gameboy_update_demo_frame(gb);
        }
        return;
    }

    if (gb->cpu.halted) {
        int halted_cycles = 4;
        if (!render_frame) {
            uint8_t enabled_interrupts = (uint8_t)(gb->memory[PYEMU_IO_IE] & 0x1F);
            uint8_t pending_interrupts = pyemu_gameboy_pending_interrupts(gb);
            if (pending_interrupts == 0 && enabled_interrupts == PYEMU_INTERRUPT_VBLANK) {
                uint32_t skip_cycles = pyemu_gameboy_cycles_until_vblank(gb);
                if ((gb->memory[PYEMU_IO_TAC] & 0x04) == 0 && skip_cycles > 4U) {
                    pyemu_gameboy_fast_forward_to_vblank(gb);
                    if (render_frame) {
                        pyemu_gameboy_update_demo_frame(gb);
                    }
                    return;
                }
                if (skip_cycles > 4U) {
                    halted_cycles = (int)skip_cycles;
                }
            }
        }
        pyemu_gameboy_tick(gb, halted_cycles);
        if (render_frame) {
            pyemu_gameboy_update_demo_frame(gb);
        }
        return;
    }

    if (gb->cpu.pc < 0x8000) {
        uint8_t bank = pyemu_gameboy_current_bank_for_address(gb, gb->cpu.pc);
        if (bank != 0xFF) {
            pyemu_block_cache_entry* entry = pyemu_gameboy_get_block_cache_entry(gb, gb->cpu.pc, bank);
            if ((!entry->valid || entry->pc != gb->cpu.pc || entry->bank != bank) && !pyemu_gameboy_decode_block(gb, entry, gb->cpu.pc, bank)) {
                entry->valid = 0;
            }
            if (entry->valid && entry->pc == gb->cpu.pc && entry->bank == bank && pyemu_gameboy_execute_block(gb, entry, &cycles)) {
                pyemu_gameboy_tick(gb, cycles);
            } else if (pyemu_gameboy_execute_hotpath(gb, &cycles)) {
                pyemu_gameboy_tick(gb, cycles);
            } else {
                opcode = pyemu_gameboy_fetch_u8(gb);
                cycles = pyemu_gameboy_execute_opcode(gb, opcode);
                pyemu_gameboy_tick(gb, cycles);
            }
        } else if (pyemu_gameboy_execute_hotpath(gb, &cycles)) {
            pyemu_gameboy_tick(gb, cycles);
        } else {
            opcode = pyemu_gameboy_fetch_u8(gb);
            cycles = pyemu_gameboy_execute_opcode(gb, opcode);
            pyemu_gameboy_tick(gb, cycles);
        }
    } else if (pyemu_gameboy_execute_hotpath(gb, &cycles)) {
        pyemu_gameboy_tick(gb, cycles);
    } else {
        opcode = pyemu_gameboy_fetch_u8(gb);
        cycles = pyemu_gameboy_execute_opcode(gb, opcode);
        pyemu_gameboy_tick(gb, cycles);
    }

    if (gb->ime_pending) {
        if (gb->ime_delay > 0) {
            gb->ime_delay -= 1;
        } else {
            gb->ime = 1;
            gb->ime_pending = 0;
        }
    }

    if (render_frame) {
        pyemu_gameboy_update_demo_frame(gb);
    }
}

static void pyemu_gameboy_step_instruction(pyemu_system* system) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    pyemu_gameboy_step_instruction_internal(gb, 1);
}

static void pyemu_gameboy_step_frame(pyemu_system* system) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    uint64_t target_cycles = gb->cycle_count + PYEMU_GAMEBOY_CYCLES_PER_FRAME;

    while (gb->cycle_count < target_cycles) {
        uint8_t enabled_interrupts = (uint8_t)(gb->memory[PYEMU_IO_IE] & 0x1F);
        uint8_t pending_interrupts = pyemu_gameboy_pending_interrupts(gb);
        uint64_t remaining_cycles = target_cycles - gb->cycle_count;

        if (gb->cpu.halted && !gb->faulted && pending_interrupts == 0 && enabled_interrupts == PYEMU_INTERRUPT_VBLANK) {
            uint32_t skip_cycles = pyemu_gameboy_cycles_until_vblank(gb);
            if (skip_cycles > 4U) {
                if ((gb->memory[PYEMU_IO_TAC] & 0x04) == 0 && (uint64_t)skip_cycles <= remaining_cycles) {
                    pyemu_gameboy_fast_forward_to_vblank(gb);
                    continue;
                }
                if ((uint64_t)skip_cycles > remaining_cycles) {
                    skip_cycles = (uint32_t)remaining_cycles;
                }
                pyemu_gameboy_tick(gb, (int)skip_cycles);
                continue;
            }
        }

        pyemu_gameboy_step_instruction_internal(gb, 0);
        if (gb->faulted) {
            break;
        }
    }

    pyemu_gameboy_update_audio_frame(gb);
}

static void pyemu_gameboy_destroy(pyemu_system* system) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    if (gb != NULL) {
        if (gb->battery_dirty) {
            pyemu_gameboy_save_battery_ram(gb);
        }
        free(gb->rom_data);
    }
    free(system);
}

static void pyemu_gameboy_get_cpu_state(const pyemu_system* system, pyemu_cpu_state* out_state) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (out_state != NULL) {
        *out_state = gb->cpu;
        out_state->ime = (uint8_t)(gb->ime ? 1 : 0);
    }
}

static void pyemu_gameboy_get_frame_buffer(const pyemu_system* system, pyemu_frame_buffer* out_frame) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (out_frame == NULL) {
        return;
    }
    out_frame->width = PYEMU_GAMEBOY_WIDTH;
    out_frame->height = PYEMU_GAMEBOY_HEIGHT;
    out_frame->rgba = gb->frame;
    out_frame->rgba_size = PYEMU_GAMEBOY_RGBA_SIZE;
}

static const uint8_t* pyemu_gameboy_get_memory(const pyemu_system* system, size_t* size) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (size != NULL) {
        *size = PYEMU_GAMEBOY_MEMORY_SIZE;
    }
    return gb->memory;
}

static void pyemu_gameboy_poke_memory(pyemu_system* system, uint16_t address, uint8_t value) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    if (gb == NULL) {
        return;
    }
    pyemu_gameboy_write_memory(gb, address, value);
}

static uint8_t pyemu_gameboy_peek_memory_vtable(pyemu_system* system, uint16_t address) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (gb == NULL) {
        return 0xFF;
    }
    return pyemu_gameboy_peek_memory(gb, address);
}

static int pyemu_gameboy_has_rom_loaded(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->rom_loaded;
}

static const char* pyemu_gameboy_get_rom_path(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->loaded_rom;
}

static const char* pyemu_gameboy_get_cartridge_title(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->cartridge_title;
}

static size_t pyemu_gameboy_get_rom_size(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->rom_size;
}

static void pyemu_gameboy_get_last_bus_access(const pyemu_system* system, pyemu_last_bus_access* out_access) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (out_access != NULL) {
        *out_access = gb->last_access;
    }
}

static void pyemu_gameboy_get_cartridge_debug_info(const pyemu_system* system, pyemu_cartridge_debug_info* out_info) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;

    if (out_info == NULL) {
        return;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->cartridge_type = gb->cartridge_type;
    out_info->rom_size_code = (gb->rom_data != NULL && gb->rom_size > 0x0148) ? gb->rom_data[0x0148] : 0;
    out_info->ram_size_code = gb->ram_size_code;
    out_info->ram_enabled = gb->ram_enabled;
    out_info->rom_bank = pyemu_gameboy_current_rom_bank(gb);
    out_info->ram_bank = (uint8_t)(pyemu_gameboy_current_eram_offset(gb) / PYEMU_GAMEBOY_ERAM_BANK_SIZE);
    out_info->banking_mode = pyemu_gameboy_uses_mbc1(gb) ? pyemu_gameboy_mbc1_mode(gb) : 0;
    out_info->has_battery = (uint8_t)(pyemu_gameboy_has_battery(gb) ? 1 : 0);
    out_info->save_file_present = (uint8_t)(pyemu_gameboy_save_file_present(gb) ? 1 : 0);
    out_info->last_mapper_value = gb->last_mapper_access.value;
    out_info->last_mapper_valid = gb->last_mapper_access.valid;
    out_info->last_mapper_address = gb->last_mapper_access.address;
    out_info->rom_bank_count = (uint32_t)gb->rom_bank_count;
    out_info->ram_bank_count = (uint32_t)gb->eram_bank_count;
}

static uint64_t pyemu_gameboy_get_cycle_count(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->cycle_count;
}

static int pyemu_gameboy_is_faulted(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->faulted;
}

static const pyemu_system_vtable pyemu_gameboy_vtable = {
    pyemu_gameboy_name,
    pyemu_gameboy_reset,
    pyemu_gameboy_load_rom,
    pyemu_gameboy_save_state,
    pyemu_gameboy_load_state,
    pyemu_gameboy_step_instruction,
    pyemu_gameboy_step_frame,
    pyemu_gameboy_destroy,
    pyemu_gameboy_get_cpu_state,
    pyemu_gameboy_get_frame_buffer,
    pyemu_gameboy_get_audio_buffer,
    pyemu_gameboy_get_memory,
    pyemu_gameboy_poke_memory,
    pyemu_gameboy_peek_memory_vtable,
    pyemu_gameboy_has_rom_loaded,
    pyemu_gameboy_get_rom_path,
    pyemu_gameboy_get_cartridge_title,
    pyemu_gameboy_get_rom_size,
    pyemu_gameboy_get_cycle_count,
    pyemu_gameboy_get_last_bus_access,
    pyemu_gameboy_get_cartridge_debug_info,
    pyemu_gameboy_is_faulted
};

pyemu_system* pyemu_gameboy_create(void) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)calloc(1, sizeof(pyemu_gameboy_system));
    if (gb == NULL) {
        return NULL;
    }

    gb->base.vtable = &pyemu_gameboy_vtable;
    pyemu_gameboy_reset((pyemu_system*)gb);
    return (pyemu_system*)gb;
}

