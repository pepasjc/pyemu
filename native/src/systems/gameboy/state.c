#include "gameboy_internal.h"

/* ROM loading, reset, and snapshot helpers for the DMG core. New cores should keep these lifecycle operations isolated from CPU execution logic as early as possible. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy the printable cartridge title from header bytes into a UI-friendly null-terminated buffer. */
void pyemu_gameboy_extract_title(pyemu_gameboy_system* gb) {
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

/* Seed the machine with the post-BIOS hardware state expected by commercial games when no boot ROM is emulated. */
void pyemu_gameboy_set_post_boot_state(pyemu_gameboy_system* gb) {
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
    gb->div_counter = 0xABCC;
    gb->timer_counter = 0;
    gb->ppu_counter = 0;
    gb->joypad_buttons = 0x0F;
    gb->joypad_directions = 0x0F;
    gb->ram_enabled = 0;

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

/* Reset transient machine state while preserving the loaded cartridge and battery-backed RAM when appropriate. */
void pyemu_gameboy_reset_state(pyemu_gameboy_system* gb) {
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

/* Load a ROM image from disk, derive cartridge metadata, restore battery RAM, and then reset into the new cart. */
int pyemu_gameboy_load_rom_file(pyemu_gameboy_system* gb, const char* path) {
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
    if (file_size <= 0) {
        fclose(rom_file);
        return 0;
    }
    if (fseek(rom_file, 0, SEEK_SET) != 0) {
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

    if (gb->rom_data != NULL) {
        free(gb->rom_data);
        gb->rom_data = NULL;
    }

    gb->rom_data = rom_data;
    gb->rom_size = (size_t)file_size;
    gb->rom_bank_count = (gb->rom_size + (PYEMU_GAMEBOY_ROM_BANKX_SIZE - 1)) / PYEMU_GAMEBOY_ROM_BANKX_SIZE;
    gb->cartridge_type = gb->rom_size > 0x147 ? gb->rom_data[0x147] : 0;
    gb->ram_size_code = gb->rom_size > 0x149 ? gb->rom_data[0x149] : 0;
    gb->eram_size = pyemu_gameboy_eram_size_from_code(gb->ram_size_code);
    gb->eram_bank_count = gb->eram_size / PYEMU_GAMEBOY_ERAM_BANK_SIZE;
    gb->rom_loaded = 1;
    gb->ram_enabled = 0;
    gb->mbc1_rom_bank = 1;
    gb->mbc3_rom_bank = pyemu_gameboy_uses_mbc1(gb) ? 0 : 1;
    gb->mbc3_ram_bank = 0;
    gb->battery_dirty = 0;
    strncpy(gb->loaded_rom, path, sizeof(gb->loaded_rom) - 1);
    gb->loaded_rom[sizeof(gb->loaded_rom) - 1] = '\0';

    memset(gb->eram, 0, sizeof(gb->eram));
    pyemu_gameboy_load_battery_ram(gb);
    pyemu_gameboy_refresh_rom_mapping(gb);
    pyemu_gameboy_extract_title(gb);
    pyemu_gameboy_reset_state(gb);
    return 1;
}

/* Serialize the full emulator snapshot needed for rewind/save-state round trips. */
int pyemu_gameboy_save_state_file(const pyemu_gameboy_system* gb, const char* path) {
    pyemu_gameboy_state_file snapshot;
    FILE* state_file;

    if (path == NULL || path[0] == '\0') {
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
    snapshot.stat_irq_line = gb->stat_irq_line;
    snapshot.last_access = gb->last_access;
    snapshot.faulted = gb->faulted;

    state_file = fopen(path, "wb");
    if (state_file == NULL) {
        return 0;
    }
    if (fwrite(&snapshot, 1, sizeof(snapshot), state_file) != sizeof(snapshot)) {
        fclose(state_file);
        return 0;
    }
    fclose(state_file);
    pyemu_gameboy_save_battery_ram(gb);
    return 1;
}

/* Restore a previously captured emulator snapshot and rebuild derived memory mappings afterward. */
int pyemu_gameboy_load_state_file(pyemu_gameboy_system* gb, const char* path) {
    pyemu_gameboy_state_file snapshot;
    FILE* state_file;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    state_file = fopen(path, "rb");
    if (state_file == NULL) {
        return 0;
    }
    if (fread(&snapshot, 1, sizeof(snapshot), state_file) != sizeof(snapshot)) {
        fclose(state_file);
        return 0;
    }
    fclose(state_file);

    if (snapshot.magic != PYEMU_GAMEBOY_STATE_MAGIC || snapshot.version != PYEMU_GAMEBOY_STATE_VERSION) {
        return 0;
    }

    if (gb->rom_data == NULL || snapshot.loaded_rom[0] == '\0' || strcmp(gb->loaded_rom, snapshot.loaded_rom) != 0) {
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
    memcpy(gb->loaded_rom, snapshot.loaded_rom, sizeof(gb->loaded_rom));
    memcpy(gb->cartridge_title, snapshot.cartridge_title, sizeof(gb->cartridge_title));
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
    gb->last_opcode = snapshot.last_opcode;
    gb->cycle_count = snapshot.cycle_count;
    gb->div_counter = snapshot.div_counter;
    gb->timer_counter = snapshot.timer_counter;
    gb->ppu_counter = snapshot.ppu_counter;
    gb->joypad_buttons = snapshot.joypad_buttons;
    gb->joypad_directions = snapshot.joypad_directions;
    gb->stat_coincidence = snapshot.stat_coincidence;
    gb->stat_mode = snapshot.stat_mode;
    gb->stat_irq_line = snapshot.stat_irq_line;
    gb->last_access = snapshot.last_access;
    gb->faulted = snapshot.faulted;
    gb->battery_dirty = 0;

    pyemu_gameboy_refresh_rom_mapping(gb);
    pyemu_gameboy_refresh_eram_window(gb);
    memset(gb->block_cache, 0, sizeof(gb->block_cache));
    pyemu_gameboy_update_demo_frame(gb);
    return 1;
}
