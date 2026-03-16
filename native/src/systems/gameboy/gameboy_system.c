#include "pyemu/systems/gameboy/gameboy_system.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PYEMU_GAMEBOY_MEMORY_SIZE 0x10000
#define PYEMU_GAMEBOY_WIDTH 160
#define PYEMU_GAMEBOY_HEIGHT 144
#define PYEMU_GAMEBOY_RGBA_SIZE (PYEMU_GAMEBOY_WIDTH * PYEMU_GAMEBOY_HEIGHT * 4)
#define PYEMU_GAMEBOY_ROM_BANK0_SIZE 0x4000
#define PYEMU_GAMEBOY_ROM_BANKX_SIZE 0x4000
#define PYEMU_GAMEBOY_VRAM_SIZE 0x2000
#define PYEMU_GAMEBOY_WRAM_SIZE 0x2000
#define PYEMU_GAMEBOY_HRAM_SIZE 0x007F
#define PYEMU_GAMEBOY_OAM_SIZE 0x00A0
#define PYEMU_GAMEBOY_ERAM_BANK_SIZE 0x2000
#define PYEMU_GAMEBOY_MAX_ERAM_SIZE 0x8000
#define PYEMU_GAMEBOY_TITLE_START 0x0134
#define PYEMU_GAMEBOY_TITLE_END 0x0143
#define PYEMU_GAMEBOY_CYCLES_PER_FRAME 70224
#define PYEMU_GAMEBOY_CYCLES_PER_SCANLINE 456
#define PYEMU_IO_DIV 0xFF04
#define PYEMU_IO_TIMA 0xFF05
#define PYEMU_IO_TMA 0xFF06
#define PYEMU_IO_TAC 0xFF07
#define PYEMU_IO_IF 0xFF0F
#define PYEMU_IO_LY 0xFF44
#define PYEMU_IO_LYC 0xFF45
#define PYEMU_IO_STAT 0xFF41
#define PYEMU_IO_LCDC 0xFF40
#define PYEMU_IO_DMA 0xFF46
#define PYEMU_IO_BGP 0xFF47
#define PYEMU_IO_OBP0 0xFF48
#define PYEMU_IO_OBP1 0xFF49
#define PYEMU_IO_WY 0xFF4A
#define PYEMU_IO_WX 0xFF4B
#define PYEMU_IO_IE 0xFFFF
#define PYEMU_INTERRUPT_VBLANK 0x01
#define PYEMU_INTERRUPT_LCD 0x02
#define PYEMU_INTERRUPT_TIMER 0x04
#define PYEMU_INTERRUPT_SERIAL 0x08
#define PYEMU_INTERRUPT_JOYPAD 0x10
#define PYEMU_FLAG_Z 0x80
#define PYEMU_FLAG_N 0x40
#define PYEMU_FLAG_H 0x20
#define PYEMU_FLAG_C 0x10
#define PYEMU_GAMEBOY_STATE_MAGIC 0x50595354U
#define PYEMU_GAMEBOY_STATE_VERSION 1U
#define PYEMU_GAMEBOY_BLOCK_CACHE_SIZE 8192
#define PYEMU_GAMEBOY_BLOCK_MAX_INSNS 16

typedef enum pyemu_block_op_type {
    PYEMU_BLOCK_OP_INVALID = 0,
    PYEMU_BLOCK_OP_NOP,
    PYEMU_BLOCK_OP_LD_R_D8,
    PYEMU_BLOCK_OP_LD_HL_D16,
    PYEMU_BLOCK_OP_LD_DE_D16,
    PYEMU_BLOCK_OP_LDH_A_A8,
    PYEMU_BLOCK_OP_LDH_A8_A,
    PYEMU_BLOCK_OP_LD_A_A16,
    PYEMU_BLOCK_OP_LD_A16_A,
    PYEMU_BLOCK_OP_XOR_A,
    PYEMU_BLOCK_OP_AND_A,
    PYEMU_BLOCK_OP_INC_A,
    PYEMU_BLOCK_OP_CP_D8,
    PYEMU_BLOCK_OP_LD_HL_INC_A,
    PYEMU_BLOCK_OP_LD_A_HL_INC,
    PYEMU_BLOCK_OP_LD_A_HL_DEC,
    PYEMU_BLOCK_OP_LD_HL_DEC_A,
    PYEMU_BLOCK_OP_DEC_B,
    PYEMU_BLOCK_OP_LD_A_L,
    PYEMU_BLOCK_OP_LD_L_A,
    PYEMU_BLOCK_OP_JR,
    PYEMU_BLOCK_OP_JR_NZ,
    PYEMU_BLOCK_OP_JR_Z,
    PYEMU_BLOCK_OP_JR_NC,
    PYEMU_BLOCK_OP_JR_C,
    PYEMU_BLOCK_OP_RET,
    PYEMU_BLOCK_OP_HALT
} pyemu_block_op_type;

typedef struct pyemu_block_insn {
    uint8_t type;
    uint8_t length;
    uint8_t operand8;
    int8_t relative;
    uint16_t operand16;
} pyemu_block_insn;

typedef struct pyemu_block_cache_entry {
    uint8_t valid;
    uint8_t terminates;
    uint8_t bank;
    uint16_t pc;
    uint8_t insn_count;
    pyemu_block_insn insns[PYEMU_GAMEBOY_BLOCK_MAX_INSNS];
} pyemu_block_cache_entry;

typedef struct pyemu_gameboy_system {
    pyemu_system base;
    pyemu_cpu_state cpu;
    uint8_t memory[PYEMU_GAMEBOY_MEMORY_SIZE];
    uint8_t frame[PYEMU_GAMEBOY_RGBA_SIZE];
    uint8_t line_scx[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_scy[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_wx[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_wy[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_lcdc[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_bgp[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_obp0[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_obp1[PYEMU_GAMEBOY_HEIGHT];
    uint8_t rom_bank0[PYEMU_GAMEBOY_ROM_BANK0_SIZE];
    uint8_t rom_bankx[PYEMU_GAMEBOY_ROM_BANKX_SIZE];
    uint8_t* rom_data;
    uint8_t vram[PYEMU_GAMEBOY_VRAM_SIZE];
    uint8_t wram[PYEMU_GAMEBOY_WRAM_SIZE];
    uint8_t hram[PYEMU_GAMEBOY_HRAM_SIZE];
    uint8_t eram[PYEMU_GAMEBOY_MAX_ERAM_SIZE];
    char loaded_rom[260];
    char cartridge_title[17];
    size_t rom_size;
    size_t rom_bank_count;
    int rom_loaded;
    uint8_t cartridge_type;
    uint8_t ram_size_code;
    size_t eram_size;
    size_t eram_bank_count;
    uint8_t ram_enabled;
    uint8_t mbc1_rom_bank;
    uint8_t mbc3_rom_bank;
    uint8_t mbc3_ram_bank;
    int ime;
    int ime_pending;
    int ime_delay;
    int halt_bug;
    uint8_t last_opcode;
    uint64_t cycle_count;
    uint32_t div_counter;
    uint32_t timer_counter;
    uint32_t ppu_counter;
    uint8_t joypad_buttons;
    uint8_t joypad_directions;
    uint8_t stat_coincidence;
    uint8_t stat_mode;
    uint8_t stat_irq_line;
    pyemu_last_bus_access last_access;
    pyemu_last_bus_access last_mapper_access;
    uint8_t bus_tracking_enabled;
    uint8_t battery_dirty;
    pyemu_block_cache_entry block_cache[PYEMU_GAMEBOY_BLOCK_CACHE_SIZE];
    int faulted;
} pyemu_gameboy_system;

typedef struct pyemu_gameboy_state_file {
    uint32_t magic;
    uint32_t version;
    pyemu_cpu_state cpu;
    uint8_t memory[PYEMU_GAMEBOY_MEMORY_SIZE];
    uint8_t vram[PYEMU_GAMEBOY_VRAM_SIZE];
    uint8_t wram[PYEMU_GAMEBOY_WRAM_SIZE];
    uint8_t hram[PYEMU_GAMEBOY_HRAM_SIZE];
    uint8_t eram[PYEMU_GAMEBOY_MAX_ERAM_SIZE];
    uint8_t line_scx[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_scy[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_wx[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_wy[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_lcdc[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_bgp[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_obp0[PYEMU_GAMEBOY_HEIGHT];
    uint8_t line_obp1[PYEMU_GAMEBOY_HEIGHT];
    char loaded_rom[260];
    char cartridge_title[17];
    size_t rom_size;
    size_t rom_bank_count;
    int rom_loaded;
    uint8_t cartridge_type;
    uint8_t ram_size_code;
    size_t eram_size;
    size_t eram_bank_count;
    uint8_t ram_enabled;
    uint8_t mbc1_rom_bank;
    uint8_t mbc3_rom_bank;
    uint8_t mbc3_ram_bank;
    int ime;
    int ime_pending;
    uint8_t last_opcode;
    uint64_t cycle_count;
    uint32_t div_counter;
    uint32_t timer_counter;
    uint32_t ppu_counter;
    uint8_t joypad_buttons;
    uint8_t joypad_directions;
    uint8_t stat_coincidence;
    uint8_t stat_mode;
    uint8_t stat_irq_line;
    pyemu_last_bus_access last_access;
    int faulted;
} pyemu_gameboy_state_file;

static uint16_t pyemu_gameboy_get_af(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.a << 8) | (gb->cpu.f & 0xF0));
}

static uint16_t pyemu_gameboy_get_bc(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.b << 8) | gb->cpu.c);
}

static uint16_t pyemu_gameboy_get_de(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.d << 8) | gb->cpu.e);
}

static uint16_t pyemu_gameboy_get_hl(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.h << 8) | gb->cpu.l);
}

static void pyemu_gameboy_set_af(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.a = (uint8_t)(value >> 8);
    gb->cpu.f = (uint8_t)(value & 0xF0);
}

static void pyemu_gameboy_set_bc(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.b = (uint8_t)(value >> 8);
    gb->cpu.c = (uint8_t)(value & 0xFF);
}

static void pyemu_gameboy_set_de(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.d = (uint8_t)(value >> 8);
    gb->cpu.e = (uint8_t)(value & 0xFF);
}

static void pyemu_gameboy_set_hl(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.h = (uint8_t)(value >> 8);
    gb->cpu.l = (uint8_t)(value & 0xFF);
}

static void pyemu_gameboy_request_interrupt(pyemu_gameboy_system* gb, uint8_t mask);
static void pyemu_gameboy_tick(pyemu_gameboy_system* gb, int cycles);
static void pyemu_gameboy_update_stat(pyemu_gameboy_system* gb);
static uint8_t pyemu_gameboy_stat_irq_signal(uint8_t stat, uint8_t coincidence, uint8_t mode);
static void pyemu_gameboy_latch_scanline_registers(pyemu_gameboy_system* gb, uint8_t ly);
static void pyemu_gameboy_update_demo_frame(pyemu_gameboy_system* gb);
static void pyemu_gameboy_step_instruction_internal(pyemu_gameboy_system* gb, int render_frame);
static void pyemu_gameboy_get_cartridge_debug_info(const pyemu_system* system, pyemu_cartridge_debug_info* out_info);
static int pyemu_gameboy_save_state(const pyemu_system* system, const char* path);
static int pyemu_gameboy_load_state(pyemu_system* system, const char* path);
static int pyemu_gameboy_timer_signal(const pyemu_gameboy_system* gb);
static void pyemu_gameboy_apply_timer_edge(pyemu_gameboy_system* gb, int old_signal, int new_signal);
static uint8_t pyemu_gameboy_normalize_mbc1_bank(const pyemu_gameboy_system* gb, uint8_t value);
static uint8_t pyemu_gameboy_mbc1_upper_bits(const pyemu_gameboy_system* gb);
static uint8_t pyemu_gameboy_mbc1_mode(const pyemu_gameboy_system* gb);
static int pyemu_gameboy_lcd_enabled(const pyemu_gameboy_system* gb);
static uint8_t pyemu_gameboy_pending_interrupts(const pyemu_gameboy_system* gb);
static uint8_t pyemu_gameboy_peek_memory(const pyemu_gameboy_system* gb, uint16_t address);
static uint32_t pyemu_gameboy_cycles_until_vblank(const pyemu_gameboy_system* gb);
static void pyemu_gameboy_fast_forward_to_vblank(pyemu_gameboy_system* gb);
static int pyemu_gameboy_execute_hotpath(pyemu_gameboy_system* gb, int* out_cycles);
static uint8_t pyemu_gameboy_current_bank_for_address(const pyemu_gameboy_system* gb, uint16_t address);
static pyemu_block_cache_entry* pyemu_gameboy_get_block_cache_entry(pyemu_gameboy_system* gb, uint16_t pc, uint8_t bank);
static int pyemu_gameboy_decode_block(pyemu_gameboy_system* gb, pyemu_block_cache_entry* entry, uint16_t pc, uint8_t bank);
static int pyemu_gameboy_execute_block(pyemu_gameboy_system* gb, const pyemu_block_cache_entry* entry, int* out_cycles);

static int pyemu_gameboy_is_tracked_access(uint16_t address) {
    if (address < 0x8000) {
        return 1;
    }
    if ((address >= 0x8000 && address <= 0x9FFF) || (address >= 0xFE00 && address <= 0xFE9F)) {
        return 1;
    }
    return address >= 0xFF00;
}

static int pyemu_gameboy_lcd_enabled(const pyemu_gameboy_system* gb) {
    return (gb->memory[PYEMU_IO_LCDC] & 0x80) != 0;
}

static uint8_t pyemu_gameboy_pending_interrupts(const pyemu_gameboy_system* gb) {
    uint8_t pending = (uint8_t)(gb->memory[PYEMU_IO_IF] & gb->memory[PYEMU_IO_IE] & 0x1F);
    if (!pyemu_gameboy_lcd_enabled(gb)) {
        pending = (uint8_t)(pending & (uint8_t)~(PYEMU_INTERRUPT_VBLANK | PYEMU_INTERRUPT_LCD));
    }
    return pending;
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

static int pyemu_gameboy_uses_mbc1(const pyemu_gameboy_system* gb) {
    return gb->cartridge_type >= 0x01 && gb->cartridge_type <= 0x03;
}

static int pyemu_gameboy_uses_mbc3(const pyemu_gameboy_system* gb) {
    return gb->cartridge_type >= 0x0F && gb->cartridge_type <= 0x13;
}

static int pyemu_gameboy_has_battery(const pyemu_gameboy_system* gb) {
    switch (gb->cartridge_type) {
        case 0x03:
        case 0x06:
        case 0x09:
        case 0x0F:
        case 0x10:
        case 0x13:
        case 0x1B:
        case 0x1E:
            return 1;
        default:
            return 0;
    }
}

static int pyemu_gameboy_save_file_present(const pyemu_gameboy_system* gb) {
    char save_path[520];
    const char* dot;
    FILE* save_file;
    size_t base_length;

    if (!pyemu_gameboy_has_battery(gb) || gb->loaded_rom[0] == 0) {
        return 0;
    }

    dot = strrchr(gb->loaded_rom, '.');
    base_length = dot != NULL ? (size_t)(dot - gb->loaded_rom) : strlen(gb->loaded_rom);
    if (base_length + 5 >= sizeof(save_path)) {
        return 0;
    }
    memcpy(save_path, gb->loaded_rom, base_length);
    memcpy(save_path + base_length, ".sav", 5);

    save_file = fopen(save_path, "rb");
    if (save_file == NULL) {
        return 0;
    }
    fclose(save_file);
    return 1;
}

static int pyemu_gameboy_has_external_ram(const pyemu_gameboy_system* gb) {
    return gb->eram_size > 0;
}

static size_t pyemu_gameboy_eram_size_from_code(uint8_t ram_size_code) {
    switch (ram_size_code) {
        case 0x02:
            return 0x2000;
        case 0x03:
            return 0x8000;
        case 0x04:
            return 0x20000;
        case 0x05:
            return 0x10000;
        default:
            return 0;
    }
}

static uint8_t pyemu_gameboy_mbc1_upper_bits(const pyemu_gameboy_system* gb) {
    return (uint8_t)(gb->mbc3_ram_bank & 0x03);
}

static uint8_t pyemu_gameboy_mbc1_mode(const pyemu_gameboy_system* gb) {
    return (uint8_t)(gb->mbc3_rom_bank & 0x01);
}

static uint8_t pyemu_gameboy_current_rom_bank(const pyemu_gameboy_system* gb) {
    if (pyemu_gameboy_uses_mbc3(gb)) {
        uint8_t bank = (uint8_t)(gb->mbc3_rom_bank & 0x7F);
        if (bank == 0) {
            bank = 1;
        }
        if (gb->rom_bank_count > 1) {
            bank = (uint8_t)(bank % gb->rom_bank_count);
            if (bank == 0) {
                bank = 1;
            }
        }
        return bank;
    }
    if (pyemu_gameboy_uses_mbc1(gb)) {
        uint8_t lower = pyemu_gameboy_normalize_mbc1_bank(gb, gb->mbc1_rom_bank);
        uint8_t bank = (uint8_t)(lower | (pyemu_gameboy_mbc1_upper_bits(gb) << 5));
        if (gb->rom_bank_count > 1) {
            bank = (uint8_t)(bank % gb->rom_bank_count);
            if (bank == 0) {
                bank = 1;
            }
        }
        return bank;
    }
    return 1;
}

static size_t pyemu_gameboy_current_eram_offset(const pyemu_gameboy_system* gb) {
    uint8_t bank = 0;
    if (!pyemu_gameboy_has_external_ram(gb) || gb->eram_bank_count == 0) {
        return 0;
    }
    if (pyemu_gameboy_uses_mbc3(gb)) {
        bank = (uint8_t)(gb->mbc3_ram_bank % gb->eram_bank_count);
    } else if (pyemu_gameboy_uses_mbc1(gb) && pyemu_gameboy_mbc1_mode(gb)) {
        bank = (uint8_t)(pyemu_gameboy_mbc1_upper_bits(gb) % gb->eram_bank_count);
    }
    return (size_t)bank * PYEMU_GAMEBOY_ERAM_BANK_SIZE;
}

static uint8_t pyemu_gameboy_current_joypad_value(const pyemu_gameboy_system* gb) {
    uint8_t select = (uint8_t)(gb->memory[0xFF00] & 0x30);
    uint8_t low = 0x0F;

    if ((select & 0x10) == 0) {
        low &= gb->joypad_directions;
    }
    if ((select & 0x20) == 0) {
        low &= gb->joypad_buttons;
    }

    return (uint8_t)(0xC0 | select | low);
}

static void pyemu_gameboy_refresh_joypad(pyemu_gameboy_system* gb, uint8_t previous_value) {
    uint8_t current_value = pyemu_gameboy_current_joypad_value(gb);
    uint8_t falling_edges = (uint8_t)((previous_value ^ current_value) & previous_value & 0x0F);

    gb->memory[0xFF00] = (uint8_t)(0xC0 | (gb->memory[0xFF00] & 0x30) | 0x0F);
    if (falling_edges != 0) {
        pyemu_gameboy_request_interrupt(gb, PYEMU_INTERRUPT_JOYPAD);
    }
}

static uint8_t pyemu_gameboy_normalize_mbc1_bank(const pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t bank = (uint8_t)(value & 0x1F);
    if (bank == 0) {
        bank = 1;
    }
    if (gb->rom_bank_count > 1) {
        bank = (uint8_t)(bank % gb->rom_bank_count);
        if (bank == 0) {
            bank = 1;
        }
    }
    return bank;
}

static int pyemu_gameboy_save_battery_ram(const pyemu_gameboy_system* gb) {
    char save_path[520];
    const char* dot;
    FILE* save_file;
    size_t base_length;

    if (!pyemu_gameboy_has_battery(gb) || !pyemu_gameboy_has_external_ram(gb) || gb->loaded_rom[0] == 0) {
        return 1;
    }
    if (!gb->battery_dirty) {
        return 1;
    }

    dot = strrchr(gb->loaded_rom, '.');
    base_length = dot != NULL ? (size_t)(dot - gb->loaded_rom) : strlen(gb->loaded_rom);
    if (base_length + 5 >= sizeof(save_path)) {
        return 0;
    }
    memcpy(save_path, gb->loaded_rom, base_length);
    memcpy(save_path + base_length, ".sav", 5);

    save_file = fopen(save_path, "wb");
    if (save_file == NULL) {
        return 0;
    }
    if (fwrite(gb->eram, 1, gb->eram_size, save_file) != gb->eram_size) {
        fclose(save_file);
        return 0;
    }
    fclose(save_file);
    ((pyemu_gameboy_system*)gb)->battery_dirty = 0;
    return 1;
}

static void pyemu_gameboy_load_battery_ram(pyemu_gameboy_system* gb) {
    char save_path[520];
    const char* dot;
    FILE* save_file;
    size_t base_length;
    size_t read_size;

    if (!pyemu_gameboy_has_battery(gb) || !pyemu_gameboy_has_external_ram(gb) || gb->loaded_rom[0] == 0) {
        return;
    }

    dot = strrchr(gb->loaded_rom, '.');
    base_length = dot != NULL ? (size_t)(dot - gb->loaded_rom) : strlen(gb->loaded_rom);
    if (base_length + 5 >= sizeof(save_path)) {
        return;
    }
    memcpy(save_path, gb->loaded_rom, base_length);
    memcpy(save_path + base_length, ".sav", 5);

    save_file = fopen(save_path, "rb");
    if (save_file == NULL) {
        return;
    }
    read_size = fread(gb->eram, 1, gb->eram_size, save_file);
    if (read_size < gb->eram_size) {
        memset(gb->eram + read_size, 0, gb->eram_size - read_size);
    }
    fclose(save_file);
    gb->battery_dirty = 0;
}

static void pyemu_gameboy_refresh_rom_mapping(pyemu_gameboy_system* gb) {
    size_t bank0_size = 0;
    size_t bankx_size = 0;
    size_t bank0_offset = 0;

    memset(gb->rom_bank0, 0, sizeof(gb->rom_bank0));
    memset(gb->rom_bankx, 0, sizeof(gb->rom_bankx));

    if (gb->rom_data != NULL && gb->rom_size > 0) {
        if (pyemu_gameboy_uses_mbc1(gb) && pyemu_gameboy_mbc1_mode(gb)) {
            bank0_offset = (size_t)(pyemu_gameboy_mbc1_upper_bits(gb) << 5) * PYEMU_GAMEBOY_ROM_BANKX_SIZE;
            if (bank0_offset >= gb->rom_size) {
                bank0_offset = 0;
            }
        }
        bank0_size = gb->rom_size - bank0_offset;
        if (bank0_size > sizeof(gb->rom_bank0)) {
            bank0_size = sizeof(gb->rom_bank0);
        }
        memcpy(gb->rom_bank0, gb->rom_data + bank0_offset, bank0_size);

        if (gb->rom_bank_count > 1) {
            size_t bank_offset = (size_t)pyemu_gameboy_current_rom_bank(gb) * PYEMU_GAMEBOY_ROM_BANKX_SIZE;
            if (bank_offset < gb->rom_size) {
                size_t remaining = gb->rom_size - bank_offset;
                bankx_size = remaining < sizeof(gb->rom_bankx) ? remaining : sizeof(gb->rom_bankx);
                memcpy(gb->rom_bankx, gb->rom_data + bank_offset, bankx_size);
            }
        }
    }
}

static uint8_t pyemu_gameboy_stat_irq_signal(uint8_t stat, uint8_t coincidence, uint8_t mode) {
    if (coincidence && (stat & 0x40)) {
        return 1;
    }
    if (mode == 0 && (stat & 0x08)) {
        return 1;
    }
    if (mode == 1 && (stat & 0x10)) {
        return 1;
    }
    if (mode == 2 && (stat & 0x20)) {
        return 1;
    }
    return 0;
}

static void pyemu_gameboy_update_stat(pyemu_gameboy_system* gb) {
    uint8_t stat = gb->memory[PYEMU_IO_STAT];
    uint8_t ly = gb->memory[PYEMU_IO_LY];
    uint8_t lyc = gb->memory[PYEMU_IO_LYC];
    uint8_t coincidence = (uint8_t)(ly == lyc ? 1 : 0);
    uint8_t mode;
    uint8_t stat_signal;

    if (!pyemu_gameboy_lcd_enabled(gb)) {
        stat = (uint8_t)((stat & (uint8_t)~0x07) | 0x00);
        gb->memory[PYEMU_IO_STAT] = stat;
        gb->stat_mode = 0;
        gb->stat_coincidence = 0;
        gb->stat_irq_line = 0;
        return;
    }

    if (ly >= 144) {
        mode = 1;
    } else if (gb->ppu_counter < 80U) {
        mode = 2;
    } else if (gb->ppu_counter < 252U) {
        mode = 3;
    } else {
        mode = 0;
    }

    stat = (uint8_t)((stat & (uint8_t)~0x07) | (coincidence ? 0x04 : 0x00) | mode);
    gb->memory[PYEMU_IO_STAT] = stat;

    stat_signal = pyemu_gameboy_stat_irq_signal(stat, coincidence, mode);
    if (stat_signal && !gb->stat_irq_line) {
        pyemu_gameboy_request_interrupt(gb, PYEMU_INTERRUPT_LCD);
    }

    gb->stat_mode = mode;
    gb->stat_coincidence = coincidence;
    gb->stat_irq_line = stat_signal;
}

static void pyemu_gameboy_latch_scanline_registers(pyemu_gameboy_system* gb, uint8_t ly) {
    if (ly >= PYEMU_GAMEBOY_HEIGHT) {
        return;
    }
    gb->line_scx[ly] = gb->memory[0xFF43];
    gb->line_scy[ly] = gb->memory[0xFF42];
    gb->line_wx[ly] = gb->memory[PYEMU_IO_WX];
    gb->line_wy[ly] = gb->memory[PYEMU_IO_WY];
    gb->line_lcdc[ly] = gb->memory[0xFF40];
    gb->line_bgp[ly] = gb->memory[PYEMU_IO_BGP];
    gb->line_obp0[ly] = gb->memory[PYEMU_IO_OBP0];
    gb->line_obp1[ly] = gb->memory[PYEMU_IO_OBP1];
}

static void pyemu_gameboy_sync_memory(pyemu_gameboy_system* gb) {
    pyemu_gameboy_refresh_rom_mapping(gb);
    memcpy(gb->memory + 0x0000, gb->rom_bank0, sizeof(gb->rom_bank0));
    memcpy(gb->memory + 0x4000, gb->rom_bankx, sizeof(gb->rom_bankx));
    memcpy(gb->memory + 0x8000, gb->vram, sizeof(gb->vram));
    if (gb->eram_size > 0) {
        size_t eram_window = gb->eram_size < PYEMU_GAMEBOY_ERAM_BANK_SIZE ? gb->eram_size : PYEMU_GAMEBOY_ERAM_BANK_SIZE;
        memset(gb->memory + 0xA000, 0xFF, PYEMU_GAMEBOY_ERAM_BANK_SIZE);
        memcpy(gb->memory + 0xA000, gb->eram + pyemu_gameboy_current_eram_offset(gb), eram_window);
    }
    memcpy(gb->memory + 0xC000, gb->wram, sizeof(gb->wram));
    memcpy(gb->memory + 0xE000, gb->wram, sizeof(gb->wram));
    memcpy(gb->memory + 0xFF80, gb->hram, sizeof(gb->hram));
}

static uint8_t pyemu_gameboy_peek_memory(const pyemu_gameboy_system* gb, uint16_t address) {
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

static uint8_t pyemu_gameboy_read_memory(const pyemu_gameboy_system* gb, uint16_t address) {
    uint8_t value = pyemu_gameboy_peek_memory(gb, address);
    pyemu_gameboy_record_access((pyemu_gameboy_system*)gb, address, value, 0);
    return value;
}

static void pyemu_gameboy_write_memory(pyemu_gameboy_system* gb, uint16_t address, uint8_t value) {
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
                if (gb->eram_bank_count > 0) {
                    memset(gb->memory + 0xA000, 0xFF, PYEMU_GAMEBOY_ERAM_BANK_SIZE);
                    memcpy(gb->memory + 0xA000, gb->eram + pyemu_gameboy_current_eram_offset(gb), gb->eram_size < PYEMU_GAMEBOY_ERAM_BANK_SIZE ? gb->eram_size : PYEMU_GAMEBOY_ERAM_BANK_SIZE);
                }
            } else if (pyemu_gameboy_uses_mbc3(gb) && gb->eram_bank_count > 0) {
                gb->mbc3_ram_bank = (uint8_t)(value & 0x03);
                memset(gb->memory + 0xA000, 0xFF, PYEMU_GAMEBOY_ERAM_BANK_SIZE);
                memcpy(gb->memory + 0xA000, gb->eram + pyemu_gameboy_current_eram_offset(gb), gb->eram_size < PYEMU_GAMEBOY_ERAM_BANK_SIZE ? gb->eram_size : PYEMU_GAMEBOY_ERAM_BANK_SIZE);
            }
        } else if (address <= 0x7FFF) {
            if (pyemu_gameboy_uses_mbc1(gb)) {
                gb->mbc3_rom_bank = (uint8_t)(value & 0x01);
                pyemu_gameboy_refresh_rom_mapping(gb);
                memcpy(gb->memory + 0x0000, gb->rom_bank0, sizeof(gb->rom_bank0));
                memcpy(gb->memory + 0x4000, gb->rom_bankx, sizeof(gb->rom_bankx));
                if (gb->eram_bank_count > 0) {
                    memset(gb->memory + 0xA000, 0xFF, PYEMU_GAMEBOY_ERAM_BANK_SIZE);
                    memcpy(gb->memory + 0xA000, gb->eram + pyemu_gameboy_current_eram_offset(gb), gb->eram_size < PYEMU_GAMEBOY_ERAM_BANK_SIZE ? gb->eram_size : PYEMU_GAMEBOY_ERAM_BANK_SIZE);
                }
            }
        }
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

    if (address >= 0x8000 && address <= 0x9FFF) {
        gb->vram[address - 0x8000] = value;
    } else if (address >= 0xC000 && address <= 0xDFFF) {
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

static uint8_t pyemu_gameboy_fetch_u8(pyemu_gameboy_system* gb) {
    uint8_t value = pyemu_gameboy_peek_memory(gb, gb->cpu.pc);
    if (gb->halt_bug) {
        gb->halt_bug = 0;
    } else {
        gb->cpu.pc = (uint16_t)(gb->cpu.pc + 1);
    }
    return value;
}

static uint16_t pyemu_gameboy_fetch_u16(pyemu_gameboy_system* gb) {
    uint8_t low = pyemu_gameboy_fetch_u8(gb);
    uint8_t high = pyemu_gameboy_fetch_u8(gb);
    return (uint16_t)(low | ((uint16_t)high << 8));
}

static void pyemu_gameboy_push_u16(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.sp = (uint16_t)(gb->cpu.sp - 1);
    pyemu_gameboy_write_memory(gb, gb->cpu.sp, (uint8_t)(value >> 8));
    gb->cpu.sp = (uint16_t)(gb->cpu.sp - 1);
    pyemu_gameboy_write_memory(gb, gb->cpu.sp, (uint8_t)(value & 0xFF));
}

static uint16_t pyemu_gameboy_pop_u16(pyemu_gameboy_system* gb) {
    uint8_t low = pyemu_gameboy_read_memory(gb, gb->cpu.sp);
    uint8_t high;
    gb->cpu.sp = (uint16_t)(gb->cpu.sp + 1);
    high = pyemu_gameboy_read_memory(gb, gb->cpu.sp);
    gb->cpu.sp = (uint16_t)(gb->cpu.sp + 1);
    return (uint16_t)(low | ((uint16_t)high << 8));
}

static void pyemu_gameboy_set_flag(pyemu_gameboy_system* gb, uint8_t mask, int enabled) {
    if (enabled) {
        gb->cpu.f = (uint8_t)(gb->cpu.f | mask);
    } else {
        gb->cpu.f = (uint8_t)(gb->cpu.f & (uint8_t)~mask);
    }
    gb->cpu.f &= 0xF0;
}

static int pyemu_gameboy_get_flag(const pyemu_gameboy_system* gb, uint8_t mask) {
    return (gb->cpu.f & mask) != 0;
}

static void pyemu_gameboy_set_flags_znhc(pyemu_gameboy_system* gb, int z, int n, int h, int c) {
    gb->cpu.f = 0;
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, z);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, n);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, h);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, c);
}

static uint8_t pyemu_gameboy_inc8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t result = (uint8_t)(value + 1);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, result == 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, ((value & 0x0F) + 1) > 0x0F);
    return result;
}

static uint8_t pyemu_gameboy_dec8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t result = (uint8_t)(value - 1);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, result == 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 1);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, (value & 0x0F) == 0);
    return result;
}

static uint8_t pyemu_gameboy_swap8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t result = (uint8_t)((value << 4) | (value >> 4));
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, 0);
    return result;
}

static void pyemu_gameboy_rlca(pyemu_gameboy_system* gb) {
    uint8_t carry = (uint8_t)(gb->cpu.a >> 7);
    gb->cpu.a = (uint8_t)((gb->cpu.a << 1) | carry);
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry != 0);
}

static void pyemu_gameboy_rrca(pyemu_gameboy_system* gb) {
    uint8_t carry = (uint8_t)(gb->cpu.a & 0x01);
    gb->cpu.a = (uint8_t)((gb->cpu.a >> 1) | (carry << 7));
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry != 0);
}

static void pyemu_gameboy_rla(pyemu_gameboy_system* gb) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1U : 0U;
    uint8_t carry_out = (uint8_t)(gb->cpu.a >> 7);
    gb->cpu.a = (uint8_t)((gb->cpu.a << 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry_out != 0);
}

static void pyemu_gameboy_rra(pyemu_gameboy_system* gb) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 0x80U : 0U;
    uint8_t carry_out = (uint8_t)(gb->cpu.a & 0x01);
    gb->cpu.a = (uint8_t)((gb->cpu.a >> 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry_out != 0);
}

static uint8_t pyemu_gameboy_rlc8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value >> 7);
    uint8_t result = (uint8_t)((value << 1) | carry);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

static uint8_t pyemu_gameboy_rrc8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)((value >> 1) | (carry << 7));
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

static uint8_t pyemu_gameboy_rl8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1U : 0U;
    uint8_t carry_out = (uint8_t)(value >> 7);
    uint8_t result = (uint8_t)((value << 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry_out != 0);
    return result;
}

static uint8_t pyemu_gameboy_rr8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 0x80U : 0U;
    uint8_t carry_out = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)((value >> 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry_out != 0);
    return result;
}

static uint8_t pyemu_gameboy_sla8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value >> 7);
    uint8_t result = (uint8_t)(value << 1);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

static uint8_t pyemu_gameboy_sra8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)((value >> 1) | (value & 0x80));
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

static uint8_t pyemu_gameboy_srl8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)(value >> 1);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

static void pyemu_gameboy_add_a(pyemu_gameboy_system* gb, uint8_t value, int with_carry) {
    uint8_t a = gb->cpu.a;
    uint8_t carry = with_carry && pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1 : 0;
    uint16_t result = (uint16_t)a + value + carry;
    gb->cpu.a = (uint8_t)result;
    pyemu_gameboy_set_flags_znhc(
        gb,
        gb->cpu.a == 0,
        0,
        ((a & 0x0F) + (value & 0x0F) + carry) > 0x0F,
        result > 0xFF
    );
}

static void pyemu_gameboy_and_a(pyemu_gameboy_system* gb, uint8_t value) {
    gb->cpu.a = (uint8_t)(gb->cpu.a & value);
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 0, 1, 0);
}

static void pyemu_gameboy_xor_a(pyemu_gameboy_system* gb, uint8_t value) {
    gb->cpu.a = (uint8_t)(gb->cpu.a ^ value);
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 0, 0, 0);
}

static void pyemu_gameboy_or_a(pyemu_gameboy_system* gb, uint8_t value) {
    gb->cpu.a = (uint8_t)(gb->cpu.a | value);
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 0, 0, 0);
}

static void pyemu_gameboy_cp_a(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t a = gb->cpu.a;
    uint8_t result = (uint8_t)(a - value);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 1, (a & 0x0F) < (value & 0x0F), a < value);
}

static void pyemu_gameboy_sub_a(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t a = gb->cpu.a;
    uint8_t result = (uint8_t)(a - value);
    gb->cpu.a = result;
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 1, (a & 0x0F) < (value & 0x0F), a < value);
}

static void pyemu_gameboy_sbc_a(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t a = gb->cpu.a;
    uint8_t carry = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1U : 0U;
    uint16_t subtrahend = (uint16_t)value + carry;
    uint16_t result = (uint16_t)a - subtrahend;
    gb->cpu.a = (uint8_t)result;
    pyemu_gameboy_set_flags_znhc(
        gb,
        gb->cpu.a == 0,
        1,
        (a & 0x0F) < ((value & 0x0F) + carry),
        (uint16_t)a < subtrahend
    );
}

static void pyemu_gameboy_add_sp_signed(pyemu_gameboy_system* gb, int8_t value) {
    uint16_t sp = gb->cpu.sp;
    uint16_t result = (uint16_t)(sp + value);
    uint16_t unsigned_value = (uint16_t)(uint8_t)value;

    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, ((sp & 0x0F) + (unsigned_value & 0x0F)) > 0x0F);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, ((sp & 0xFF) + (unsigned_value & 0xFF)) > 0xFF);
    gb->cpu.sp = result;
}

static uint16_t pyemu_gameboy_sp_plus_signed(pyemu_gameboy_system* gb, int8_t value) {
    uint16_t sp = gb->cpu.sp;
    uint16_t result = (uint16_t)(sp + value);
    uint16_t unsigned_value = (uint16_t)(uint8_t)value;

    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, ((sp & 0x0F) + (unsigned_value & 0x0F)) > 0x0F);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, ((sp & 0xFF) + (unsigned_value & 0xFF)) > 0xFF);
    return result;
}

static void pyemu_gameboy_bit_test(pyemu_gameboy_system* gb, uint8_t value, uint8_t bit) {
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, (value & (uint8_t)(1U << bit)) == 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, 1);
}

static uint8_t pyemu_gameboy_read_r8(pyemu_gameboy_system* gb, int index) {
    switch (index & 0x07) {
        case 0: return gb->cpu.b;
        case 1: return gb->cpu.c;
        case 2: return gb->cpu.d;
        case 3: return gb->cpu.e;
        case 4: return gb->cpu.h;
        case 5: return gb->cpu.l;
        case 6: return pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb));
        default: return gb->cpu.a;
    }
}

static void pyemu_gameboy_write_r8(pyemu_gameboy_system* gb, int index, uint8_t value) {
    switch (index & 0x07) {
        case 0: gb->cpu.b = value; break;
        case 1: gb->cpu.c = value; break;
        case 2: gb->cpu.d = value; break;
        case 3: gb->cpu.e = value; break;
        case 4: gb->cpu.h = value; break;
        case 5: gb->cpu.l = value; break;
        case 6: pyemu_gameboy_write_memory(gb, pyemu_gameboy_get_hl(gb), value); break;
        default: gb->cpu.a = value; break;
    }
}

static void pyemu_gameboy_add_hl(pyemu_gameboy_system* gb, uint16_t value) {
    uint16_t hl = pyemu_gameboy_get_hl(gb);
    uint32_t result = (uint32_t)hl + value;
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, ((hl & 0x0FFF) + (value & 0x0FFF)) > 0x0FFF);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, result > 0xFFFF);
    pyemu_gameboy_set_hl(gb, (uint16_t)result);
}

static void pyemu_gameboy_daa(pyemu_gameboy_system* gb) {
    uint8_t correction = 0;
    int carry = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C);

    if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_N)) {
        if (carry || gb->cpu.a > 0x99) {
            correction |= 0x60;
            carry = 1;
        }
        if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_H) || (gb->cpu.a & 0x0F) > 0x09) {
            correction |= 0x06;
        }
        gb->cpu.a = (uint8_t)(gb->cpu.a + correction);
    } else {
        if (carry) {
            correction |= 0x60;
        }
        if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_H)) {
            correction |= 0x06;
        }
        gb->cpu.a = (uint8_t)(gb->cpu.a - correction);
    }

    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, gb->cpu.a == 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, carry);
}

static uint8_t pyemu_gameboy_apply_palette(uint8_t palette, uint8_t color_id) {
    static const uint8_t dmg_shades[4] = {255, 170, 85, 0};
    uint8_t shade_index = (uint8_t)((palette >> (color_id * 2)) & 0x03);
    return dmg_shades[shade_index];
}

static uint8_t pyemu_gameboy_tile_pixel(
    const pyemu_gameboy_system* gb,
    uint8_t lcdc,
    uint8_t tile_index,
    uint8_t pixel_x,
    uint8_t pixel_y,
    int sprite_mode
) {
    uint16_t tile_address;
    uint8_t low;
    uint8_t high;
    uint8_t bit = (uint8_t)(7 - (pixel_x & 0x07));

    if (sprite_mode || (lcdc & 0x10)) {
        tile_address = (uint16_t)(0x8000 + (tile_index * 16) + ((pixel_y & 0x07) * 2));
    } else {
        int8_t signed_index = (int8_t)tile_index;
        tile_address = (uint16_t)(0x9000 + (signed_index * 16) + ((pixel_y & 0x07) * 2));
    }

    low = pyemu_gameboy_read_memory(gb, tile_address);
    high = pyemu_gameboy_read_memory(gb, (uint16_t)(tile_address + 1));
    return (uint8_t)((((high >> bit) & 0x01) << 1) | ((low >> bit) & 0x01));
}

static uint16_t pyemu_gameboy_timer_bit_mask_from_tac(uint8_t tac) {
    switch (tac & 0x03) {
        case 0x00:
            return 1U << 9;
        case 0x01:
            return 1U << 3;
        case 0x02:
            return 1U << 5;
        default:
            return 1U << 7;
    }
}

static int pyemu_gameboy_timer_signal_from_state(uint32_t div_counter, uint8_t tac) {
    uint16_t mask;
    if ((tac & 0x04) == 0) {
        return 0;
    }
    mask = pyemu_gameboy_timer_bit_mask_from_tac(tac);
    return (div_counter & mask) != 0;
}

static int pyemu_gameboy_timer_signal(const pyemu_gameboy_system* gb) {
    return pyemu_gameboy_timer_signal_from_state(gb->div_counter, gb->memory[PYEMU_IO_TAC]);
}

static void pyemu_gameboy_increment_tima(pyemu_gameboy_system* gb) {
    uint8_t tima = gb->memory[PYEMU_IO_TIMA];
    if (tima == 0xFF) {
        gb->memory[PYEMU_IO_TIMA] = gb->memory[PYEMU_IO_TMA];
        pyemu_gameboy_request_interrupt(gb, PYEMU_INTERRUPT_TIMER);
    } else {
        gb->memory[PYEMU_IO_TIMA] = (uint8_t)(tima + 1);
    }
}

static void pyemu_gameboy_apply_timer_edge(pyemu_gameboy_system* gb, int old_signal, int new_signal) {
    if (old_signal && !new_signal) {
        pyemu_gameboy_increment_tima(gb);
    }
}

static void pyemu_gameboy_update_demo_frame(pyemu_gameboy_system* gb) {
    int x;
    int y;
    int sprite_index;

    for (y = 0; y < PYEMU_GAMEBOY_HEIGHT; ++y) {
        uint8_t lcdc = gb->line_lcdc[y];
        uint8_t scy = gb->line_scy[y];
        uint8_t scx = gb->line_scx[y];
        uint8_t wy = gb->line_wy[y];
        uint8_t wx = gb->line_wx[y];
        uint8_t bgp = gb->line_bgp[y];
        uint8_t obp0 = gb->line_obp0[y];
        uint8_t obp1 = gb->line_obp1[y];
        for (x = 0; x < PYEMU_GAMEBOY_WIDTH; ++x) {
            size_t pixel = (size_t)(y * PYEMU_GAMEBOY_WIDTH + x) * 4U;
            uint8_t shade = 0xCC;
            uint8_t bg_color_id = 0;

            if (gb->rom_loaded && (lcdc & 0x01)) {
                uint8_t map_x = (uint8_t)(x + scx);
                uint8_t map_y = (uint8_t)(y + scy);
                uint16_t bg_map_base = (lcdc & 0x08) ? 0x9C00 : 0x9800;
                uint16_t tile_map_address = (uint16_t)(bg_map_base + ((map_y / 8) * 32) + (map_x / 8));
                uint8_t tile_index = pyemu_gameboy_read_memory(gb, tile_map_address);

                bg_color_id = pyemu_gameboy_tile_pixel(gb, lcdc, tile_index, map_x, map_y, 0);
                shade = pyemu_gameboy_apply_palette(bgp, bg_color_id);

                if ((lcdc & 0x20) && y >= wy && (int)x >= ((int)wx - 7)) {
                    uint8_t win_x = (uint8_t)(x - ((int)wx - 7));
                    uint8_t win_y = (uint8_t)(y - wy);
                    uint16_t win_map_base = (lcdc & 0x40) ? 0x9C00 : 0x9800;
                    uint16_t win_tile_map_address = (uint16_t)(win_map_base + ((win_y / 8) * 32) + (win_x / 8));
                    uint8_t win_tile_index = pyemu_gameboy_read_memory(gb, win_tile_map_address);
                    bg_color_id = pyemu_gameboy_tile_pixel(gb, lcdc, win_tile_index, win_x, win_y, 0);
                    shade = pyemu_gameboy_apply_palette(bgp, bg_color_id);
                }
            } else if (gb->rom_loaded) {
                shade = gb->memory[(uint16_t)(0x9800 + ((x / 8) + ((y / 8) * 32)) % 0x0400)];
            } else {
                shade = (uint8_t)((x + y + gb->cpu.a + pyemu_gameboy_read_memory(gb, PYEMU_IO_LY)) % 255);
            }

            if (lcdc & 0x02) {
                int sprite_height = (lcdc & 0x04) ? 16 : 8;
                for (sprite_index = 0; sprite_index < 40; ++sprite_index) {
                    uint16_t oam = (uint16_t)(0xFE00 + sprite_index * 4);
                    int sprite_y = (int)pyemu_gameboy_read_memory(gb, oam) - 16;
                    int sprite_x = (int)pyemu_gameboy_read_memory(gb, (uint16_t)(oam + 1)) - 8;
                    uint8_t tile = pyemu_gameboy_read_memory(gb, (uint16_t)(oam + 2));
                    uint8_t attrs = pyemu_gameboy_read_memory(gb, (uint16_t)(oam + 3));
                    uint8_t sprite_color_id;
                    uint8_t sprite_palette;
                    uint8_t sx;
                    uint8_t sy;

                    if (x < sprite_x || x >= sprite_x + 8 || y < sprite_y || y >= sprite_y + sprite_height) {
                        continue;
                    }

                    sx = (uint8_t)(x - sprite_x);
                    sy = (uint8_t)(y - sprite_y);
                    if (attrs & 0x20) {
                        sx = (uint8_t)(7 - sx);
                    }
                    if (attrs & 0x40) {
                        sy = (uint8_t)(sprite_height - 1 - sy);
                    }

                    if (sprite_height == 16) {
                        tile &= 0xFE;
                        if (sy >= 8) {
                            tile = (uint8_t)(tile + 1);
                            sy = (uint8_t)(sy - 8);
                        }
                    }

                    sprite_color_id = pyemu_gameboy_tile_pixel(gb, lcdc, tile, sx, sy, 1);
                    if (sprite_color_id == 0) {
                        continue;
                    }
                    if ((attrs & 0x80) && bg_color_id != 0) {
                        continue;
                    }

                    sprite_palette = (attrs & 0x10) ? obp1 : obp0;
                    shade = pyemu_gameboy_apply_palette(sprite_palette, sprite_color_id);
                    break;
                }
            }

            gb->frame[pixel + 0] = shade;
            gb->frame[pixel + 1] = shade;
            gb->frame[pixel + 2] = shade;
            gb->frame[pixel + 3] = 255;
        }
    }
}

static void pyemu_gameboy_request_interrupt(pyemu_gameboy_system* gb, uint8_t mask) {
    gb->memory[PYEMU_IO_IF] = (uint8_t)(0xE0 | ((gb->memory[PYEMU_IO_IF] | mask) & 0x1F));
}

static void pyemu_gameboy_tick(pyemu_gameboy_system* gb, int cycles) {
    uint32_t old_div_counter = gb->div_counter;
    uint8_t tac = gb->memory[PYEMU_IO_TAC];
    int remaining_cycles = cycles;

    gb->cycle_count += (uint64_t)cycles;
    gb->div_counter += (uint32_t)cycles;
    gb->memory[PYEMU_IO_DIV] = (uint8_t)((gb->div_counter >> 8) & 0xFF);

    if (tac & 0x04) {
        uint32_t mask = pyemu_gameboy_timer_bit_mask_from_tac(tac);
        uint32_t period = mask << 1;
        uint32_t old_edges = old_div_counter / period;
        uint32_t new_edges = gb->div_counter / period;
        uint32_t edge_count = new_edges - old_edges;
        while (edge_count > 0) {
            pyemu_gameboy_increment_tima(gb);
            edge_count -= 1;
        }
    }

    if (!pyemu_gameboy_lcd_enabled(gb)) {
        gb->ppu_counter = 0;
        gb->memory[PYEMU_IO_LY] = 0;
        pyemu_gameboy_update_stat(gb);
        return;
    }

    while (remaining_cycles > 0) {
        int step_cycles = remaining_cycles;
        uint32_t old_ppu_counter = gb->ppu_counter;
        int until_scanline_end = (int)(PYEMU_GAMEBOY_CYCLES_PER_SCANLINE - gb->ppu_counter);
        int next_mode_boundary = until_scanline_end;
        int stat_needs_update = 0;

        if (gb->memory[PYEMU_IO_LY] < 144) {
            if (gb->ppu_counter < 80U) {
                next_mode_boundary = (int)(80U - gb->ppu_counter);
            } else if (gb->ppu_counter < 252U) {
                next_mode_boundary = (int)(252U - gb->ppu_counter);
            }
        }

        if (next_mode_boundary <= 0) {
            next_mode_boundary = until_scanline_end > 0 ? until_scanline_end : 1;
        }
        if (step_cycles > next_mode_boundary) {
            step_cycles = next_mode_boundary;
        }

        gb->ppu_counter += (uint32_t)step_cycles;
        remaining_cycles -= step_cycles;

        if (gb->memory[PYEMU_IO_LY] < 144) {
            if (old_ppu_counter < 80U && gb->ppu_counter >= 80U) {
                stat_needs_update = 1;
            } else if (old_ppu_counter < 252U && gb->ppu_counter >= 252U) {
                stat_needs_update = 1;
            }
        }

        while (gb->ppu_counter >= PYEMU_GAMEBOY_CYCLES_PER_SCANLINE) {
            uint8_t current_ly = pyemu_gameboy_read_memory(gb, PYEMU_IO_LY);
            uint8_t ly;
            if (current_ly < PYEMU_GAMEBOY_HEIGHT) {
                pyemu_gameboy_latch_scanline_registers(gb, current_ly);
            }
            gb->ppu_counter -= PYEMU_GAMEBOY_CYCLES_PER_SCANLINE;
            ly = (uint8_t)(current_ly + 1);
            if (ly == 144) {
                pyemu_gameboy_request_interrupt(gb, PYEMU_INTERRUPT_VBLANK);
            }
            if (ly >= 154) {
                ly = 0;
            }
            gb->memory[PYEMU_IO_LY] = ly;
            stat_needs_update = 1;
        }

        if (stat_needs_update) {
            pyemu_gameboy_update_stat(gb);
        }
    }
}

static int pyemu_gameboy_service_interrupts(pyemu_gameboy_system* gb) {
    uint8_t pending = pyemu_gameboy_pending_interrupts(gb);
    uint8_t mask = 0;
    uint16_t vector = 0;

    if (!pyemu_gameboy_lcd_enabled(gb)) {
        pending = (uint8_t)(pending & (uint8_t)~(PYEMU_INTERRUPT_VBLANK | PYEMU_INTERRUPT_LCD));
    }

    if (pending == 0) {
        return 0;
    }

    if (gb->cpu.halted) {
        gb->cpu.halted = 0;
    }

    if (!gb->ime) {
        return 0;
    }

    if (pending & PYEMU_INTERRUPT_VBLANK) {
        mask = PYEMU_INTERRUPT_VBLANK;
        vector = 0x0040;
    } else if (pending & PYEMU_INTERRUPT_LCD) {
        mask = PYEMU_INTERRUPT_LCD;
        vector = 0x0048;
    } else if (pending & PYEMU_INTERRUPT_TIMER) {
        mask = PYEMU_INTERRUPT_TIMER;
        vector = 0x0050;
    } else if (pending & PYEMU_INTERRUPT_SERIAL) {
        mask = PYEMU_INTERRUPT_SERIAL;
        vector = 0x0058;
    } else if (pending & PYEMU_INTERRUPT_JOYPAD) {
        mask = PYEMU_INTERRUPT_JOYPAD;
        vector = 0x0060;
    }

    if (mask == 0) {
        return 0;
    }

    gb->ime = 0;
    gb->ime_pending = 0;
    gb->ime_delay = 0;
    gb->memory[PYEMU_IO_IF] = (uint8_t)(0xE0 | ((gb->memory[PYEMU_IO_IF] & (uint8_t)~mask) & 0x1F));
    pyemu_gameboy_push_u16(gb, gb->cpu.pc);
    gb->cpu.pc = vector;
    pyemu_gameboy_tick(gb, 20);
    return 20;
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
    if (gb->battery_dirty) {
        pyemu_gameboy_save_battery_ram(gb);
    }
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
    memset(&gb->cpu, 0, sizeof(gb->cpu));
    gb->last_opcode = 0;
    gb->stat_irq_line = 0;
    memset(&gb->last_access, 0, sizeof(gb->last_access));
    memset(&gb->last_mapper_access, 0, sizeof(gb->last_mapper_access));
    gb->bus_tracking_enabled = 1;
    gb->battery_dirty = 0;

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

void pyemu_gameboy_set_joypad_state(pyemu_system* system, uint8_t buttons, uint8_t directions) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    uint8_t previous_value;
    if (gb == NULL) {
        return;
    }
    previous_value = pyemu_gameboy_current_joypad_value(gb);
    gb->joypad_buttons = (uint8_t)(buttons & 0x0F);
    gb->joypad_directions = (uint8_t)(directions & 0x0F);
    pyemu_gameboy_refresh_joypad(gb, previous_value);
}

void pyemu_gameboy_set_bus_tracking(pyemu_system* system, int enabled) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    if (gb == NULL) {
        return;
    }
    gb->bus_tracking_enabled = enabled ? 1 : 0;
    if (!gb->bus_tracking_enabled) {
        memset(&gb->last_access, 0, sizeof(gb->last_access));
    }
}

static int pyemu_gameboy_load_rom(pyemu_system* system, const char* path) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    FILE* rom_file;
    long file_size;
    size_t read_size;
    uint8_t* rom_data;

    if (path == NULL || path[0] == 0) {
        return 0;
    }

    if (gb->battery_dirty) {
        pyemu_gameboy_save_battery_ram(gb);
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
    gb->battery_dirty = 0;
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

    if (gb->battery_dirty) {
        pyemu_gameboy_save_battery_ram(gb);
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
    gb->stat_irq_line = pyemu_gameboy_stat_irq_signal(gb->memory[PYEMU_IO_STAT], gb->stat_coincidence, gb->stat_mode);
    gb->last_access = snapshot.last_access;
    memset(&gb->last_mapper_access, 0, sizeof(gb->last_mapper_access));
    gb->faulted = snapshot.faulted;
    gb->battery_dirty = 0;

    pyemu_gameboy_refresh_rom_mapping(gb);
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

    switch (opcode) {
        case 0x00:
            cycles = 4;
            break;
        case 0x01:
            pyemu_gameboy_set_bc(gb, pyemu_gameboy_fetch_u16(gb));
            cycles = 12;
            break;
        case 0x02:
            pyemu_gameboy_write_memory(gb, pyemu_gameboy_get_bc(gb), gb->cpu.a);
            cycles = 8;
            break;
        case 0x03:
            pyemu_gameboy_set_bc(gb, (uint16_t)(pyemu_gameboy_get_bc(gb) + 1));
            cycles = 8;
            break;
        case 0x04:
            gb->cpu.b = pyemu_gameboy_inc8(gb, gb->cpu.b);
            cycles = 4;
            break;
        case 0x05:
            gb->cpu.b = pyemu_gameboy_dec8(gb, gb->cpu.b);
            cycles = 4;
            break;
        case 0x06:
            gb->cpu.b = pyemu_gameboy_fetch_u8(gb);
            cycles = 8;
            break;
        case 0x07:
            pyemu_gameboy_rlca(gb);
            cycles = 4;
            break;
        case 0x08: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            pyemu_gameboy_write_memory(gb, address, (uint8_t)(gb->cpu.sp & 0xFF));
            pyemu_gameboy_write_memory(gb, (uint16_t)(address + 1), (uint8_t)(gb->cpu.sp >> 8));
            cycles = 20;
            break;
        }
        case 0x09:
            pyemu_gameboy_add_hl(gb, pyemu_gameboy_get_bc(gb));
            cycles = 8;
            break;
        case 0x0A:
            gb->cpu.a = pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_bc(gb));
            cycles = 8;
            break;
        case 0x0B:
            pyemu_gameboy_set_bc(gb, (uint16_t)(pyemu_gameboy_get_bc(gb) - 1));
            cycles = 8;
            break;
        case 0x0C:
            gb->cpu.c = pyemu_gameboy_inc8(gb, gb->cpu.c);
            cycles = 4;
            break;
        case 0x0D:
            gb->cpu.c = pyemu_gameboy_dec8(gb, gb->cpu.c);
            cycles = 4;
            break;
        case 0x0E:
            gb->cpu.c = pyemu_gameboy_fetch_u8(gb);
            cycles = 8;
            break;
        case 0x0F:
            pyemu_gameboy_rrca(gb);
            cycles = 4;
            break;
        case 0x11:
            pyemu_gameboy_set_de(gb, pyemu_gameboy_fetch_u16(gb));
            cycles = 12;
            break;
        case 0x12:
            pyemu_gameboy_write_memory(gb, pyemu_gameboy_get_de(gb), gb->cpu.a);
            cycles = 8;
            break;
        case 0x13:
            pyemu_gameboy_set_de(gb, (uint16_t)(pyemu_gameboy_get_de(gb) + 1));
            cycles = 8;
            break;
        case 0x19:
            pyemu_gameboy_add_hl(gb, pyemu_gameboy_get_de(gb));
            cycles = 8;
            break;
        case 0x14:
            gb->cpu.d = pyemu_gameboy_inc8(gb, gb->cpu.d);
            cycles = 4;
            break;
        case 0x15:
            gb->cpu.d = pyemu_gameboy_dec8(gb, gb->cpu.d);
            cycles = 4;
            break;
        case 0x16:
            gb->cpu.d = pyemu_gameboy_fetch_u8(gb);
            cycles = 8;
            break;
        case 0x17:
            pyemu_gameboy_rla(gb);
            cycles = 4;
            break;
        case 0x18: {
            int8_t offset = (int8_t)pyemu_gameboy_fetch_u8(gb);
            gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
            cycles = 12;
            break;
        }
        case 0x1A:
            gb->cpu.a = pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_de(gb));
            cycles = 8;
            break;
        case 0x1B:
            pyemu_gameboy_set_de(gb, (uint16_t)(pyemu_gameboy_get_de(gb) - 1));
            cycles = 8;
            break;
        case 0x1C:
            gb->cpu.e = pyemu_gameboy_inc8(gb, gb->cpu.e);
            cycles = 4;
            break;
        case 0x1D:
            gb->cpu.e = pyemu_gameboy_dec8(gb, gb->cpu.e);
            cycles = 4;
            break;
        case 0x1E:
            gb->cpu.e = pyemu_gameboy_fetch_u8(gb);
            cycles = 8;
            break;
        case 0x1F:
            pyemu_gameboy_rra(gb);
            cycles = 4;
            break;
        case 0x20: {
            int8_t offset = (int8_t)pyemu_gameboy_fetch_u8(gb);
            cycles = 8;
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                cycles = 12;
            }
            break;
        }
        case 0x21:
            pyemu_gameboy_set_hl(gb, pyemu_gameboy_fetch_u16(gb));
            cycles = 12;
            break;
        case 0x22: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            pyemu_gameboy_write_memory(gb, hl, gb->cpu.a);
            pyemu_gameboy_set_hl(gb, (uint16_t)(hl + 1));
            cycles = 8;
            break;
        }
        case 0x23:
            pyemu_gameboy_set_hl(gb, (uint16_t)(pyemu_gameboy_get_hl(gb) + 1));
            cycles = 8;
            break;
        case 0x24:
            gb->cpu.h = pyemu_gameboy_inc8(gb, gb->cpu.h);
            cycles = 4;
            break;
        case 0x25:
            gb->cpu.h = pyemu_gameboy_dec8(gb, gb->cpu.h);
            cycles = 4;
            break;
        case 0x26:
            gb->cpu.h = pyemu_gameboy_fetch_u8(gb);
            cycles = 8;
            break;
        case 0x27:
            pyemu_gameboy_daa(gb);
            cycles = 4;
            break;
        case 0x28: {
            int8_t offset = (int8_t)pyemu_gameboy_fetch_u8(gb);
            cycles = 8;
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                cycles = 12;
            }
            break;
        }
        case 0x29:
            pyemu_gameboy_add_hl(gb, pyemu_gameboy_get_hl(gb));
            cycles = 8;
            break;
        case 0x2A: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            gb->cpu.a = pyemu_gameboy_read_memory(gb, hl);
            pyemu_gameboy_set_hl(gb, (uint16_t)(hl + 1));
            cycles = 8;
            break;
        }
        case 0x2B:
            pyemu_gameboy_set_hl(gb, (uint16_t)(pyemu_gameboy_get_hl(gb) - 1));
            cycles = 8;
            break;
        case 0x2C:
            gb->cpu.l = pyemu_gameboy_inc8(gb, gb->cpu.l);
            cycles = 4;
            break;
        case 0x2D:
            gb->cpu.l = pyemu_gameboy_dec8(gb, gb->cpu.l);
            cycles = 4;
            break;
        case 0x2E:
            gb->cpu.l = pyemu_gameboy_fetch_u8(gb);
            cycles = 8;
            break;
        case 0x2F:
            gb->cpu.a = (uint8_t)~gb->cpu.a;
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 1);
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, 1);
            cycles = 4;
            break;
        case 0x30: {
            int8_t offset = (int8_t)pyemu_gameboy_fetch_u8(gb);
            cycles = 8;
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                cycles = 12;
            }
            break;
        }
        case 0x31:
            gb->cpu.sp = pyemu_gameboy_fetch_u16(gb);
            cycles = 12;
            break;
        case 0x37:
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, 0);
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, 1);
            cycles = 4;
            break;
        case 0x38: {
            int8_t offset = (int8_t)pyemu_gameboy_fetch_u8(gb);
            cycles = 8;
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                cycles = 12;
            }
            break;
        }
        case 0x32: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            pyemu_gameboy_write_memory(gb, hl, gb->cpu.a);
            pyemu_gameboy_set_hl(gb, (uint16_t)(hl - 1));
            cycles = 8;
            break;
        }
        case 0x33:
            gb->cpu.sp = (uint16_t)(gb->cpu.sp + 1);
            cycles = 8;
            break;
        case 0x34: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            uint8_t value = pyemu_gameboy_read_memory(gb, hl);
            pyemu_gameboy_write_memory(gb, hl, pyemu_gameboy_inc8(gb, value));
            cycles = 12;
            break;
        }
        case 0x35: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            uint8_t value = pyemu_gameboy_read_memory(gb, hl);
            pyemu_gameboy_write_memory(gb, hl, pyemu_gameboy_dec8(gb, value));
            cycles = 12;
            break;
        }
        case 0x39:
            pyemu_gameboy_add_hl(gb, gb->cpu.sp);
            cycles = 8;
            break;
        case 0x3A: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            gb->cpu.a = pyemu_gameboy_read_memory(gb, hl);
            pyemu_gameboy_set_hl(gb, (uint16_t)(hl - 1));
            cycles = 8;
            break;
        }
        case 0x36: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            pyemu_gameboy_write_memory(gb, hl, pyemu_gameboy_fetch_u8(gb));
            cycles = 12;
            break;
        }
        case 0x3C:
            gb->cpu.a = pyemu_gameboy_inc8(gb, gb->cpu.a);
            cycles = 4;
            break;
        case 0x3D:
            gb->cpu.a = pyemu_gameboy_dec8(gb, gb->cpu.a);
            cycles = 4;
            break;
        case 0x3F:
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, 0);
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, !pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C));
            cycles = 4;
            break;
        case 0x3E:
            gb->cpu.a = pyemu_gameboy_fetch_u8(gb);
            cycles = 8;
            break;
        case 0x46:
            gb->cpu.b = pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb));
            cycles = 8;
            break;
        case 0x47:
        case 0x4F:
        case 0x57:
        case 0x5F:
        case 0x67:
        case 0x6F:
        case 0x78:
        case 0x79:
        case 0x7A:
        case 0x7B:
        case 0x7C:
        case 0x7D:
            cycles = 4;
            switch (opcode) {
                case 0x47: gb->cpu.b = gb->cpu.a; break;
                case 0x4F: gb->cpu.c = gb->cpu.a; break;
                case 0x57: gb->cpu.d = gb->cpu.a; break;
                case 0x5F: gb->cpu.e = gb->cpu.a; break;
                case 0x67: gb->cpu.h = gb->cpu.a; break;
                case 0x6F: gb->cpu.l = gb->cpu.a; break;
                case 0x78: gb->cpu.a = gb->cpu.b; break;
                case 0x79: gb->cpu.a = gb->cpu.c; break;
                case 0x7A: gb->cpu.a = gb->cpu.d; break;
                case 0x7B: gb->cpu.a = gb->cpu.e; break;
                case 0x7C: gb->cpu.a = gb->cpu.h; break;
                case 0x7D: gb->cpu.a = gb->cpu.l; break;
            }
            break;
        case 0x77:
            pyemu_gameboy_write_memory(gb, pyemu_gameboy_get_hl(gb), gb->cpu.a);
            cycles = 8;
            break;
        case 0x7E:
            gb->cpu.a = pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb));
            cycles = 8;
            break;
        case 0x80:
            pyemu_gameboy_add_a(gb, gb->cpu.b, 0);
            cycles = 4;
            break;
        case 0x81:
            pyemu_gameboy_add_a(gb, gb->cpu.c, 0);
            cycles = 4;
            break;
        case 0x82:
            pyemu_gameboy_add_a(gb, gb->cpu.d, 0);
            cycles = 4;
            break;
        case 0x83:
            pyemu_gameboy_add_a(gb, gb->cpu.e, 0);
            cycles = 4;
            break;
        case 0x84:
            pyemu_gameboy_add_a(gb, gb->cpu.h, 0);
            cycles = 4;
            break;
        case 0x85:
            pyemu_gameboy_add_a(gb, gb->cpu.l, 0);
            cycles = 4;
            break;
        case 0x86:
            pyemu_gameboy_add_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb)), 0);
            cycles = 8;
            break;
        case 0x87:
            pyemu_gameboy_add_a(gb, gb->cpu.a, 0);
            cycles = 4;
            break;
        case 0x88:
            pyemu_gameboy_add_a(gb, gb->cpu.b, 1);
            cycles = 4;
            break;
        case 0x89:
            pyemu_gameboy_add_a(gb, gb->cpu.c, 1);
            cycles = 4;
            break;
        case 0x8A:
            pyemu_gameboy_add_a(gb, gb->cpu.d, 1);
            cycles = 4;
            break;
        case 0x8B:
            pyemu_gameboy_add_a(gb, gb->cpu.e, 1);
            cycles = 4;
            break;
        case 0x8C:
            pyemu_gameboy_add_a(gb, gb->cpu.h, 1);
            cycles = 4;
            break;
        case 0x8D:
            pyemu_gameboy_add_a(gb, gb->cpu.l, 1);
            cycles = 4;
            break;
        case 0x8E:
            pyemu_gameboy_add_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb)), 1);
            cycles = 8;
            break;
        case 0x8F:
            pyemu_gameboy_add_a(gb, gb->cpu.a, 1);
            cycles = 4;
            break;
        case 0x90:
            pyemu_gameboy_sub_a(gb, gb->cpu.b);
            cycles = 4;
            break;
        case 0x91:
            pyemu_gameboy_sub_a(gb, gb->cpu.c);
            cycles = 4;
            break;
        case 0x92:
            pyemu_gameboy_sub_a(gb, gb->cpu.d);
            cycles = 4;
            break;
        case 0x93:
            pyemu_gameboy_sub_a(gb, gb->cpu.e);
            cycles = 4;
            break;
        case 0x94:
            pyemu_gameboy_sub_a(gb, gb->cpu.h);
            cycles = 4;
            break;
        case 0x95:
            pyemu_gameboy_sub_a(gb, gb->cpu.l);
            cycles = 4;
            break;
        case 0x96:
            pyemu_gameboy_sub_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb)));
            cycles = 8;
            break;
        case 0x97:
            pyemu_gameboy_sub_a(gb, gb->cpu.a);
            cycles = 4;
            break;
        case 0x98:
            pyemu_gameboy_sbc_a(gb, gb->cpu.b);
            cycles = 4;
            break;
        case 0x99:
            pyemu_gameboy_sbc_a(gb, gb->cpu.c);
            cycles = 4;
            break;
        case 0x9A:
            pyemu_gameboy_sbc_a(gb, gb->cpu.d);
            cycles = 4;
            break;
        case 0x9B:
            pyemu_gameboy_sbc_a(gb, gb->cpu.e);
            cycles = 4;
            break;
        case 0x9C:
            pyemu_gameboy_sbc_a(gb, gb->cpu.h);
            cycles = 4;
            break;
        case 0x9D:
            pyemu_gameboy_sbc_a(gb, gb->cpu.l);
            cycles = 4;
            break;
        case 0x9E:
            pyemu_gameboy_sbc_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb)));
            cycles = 8;
            break;
        case 0x9F:
            pyemu_gameboy_sbc_a(gb, gb->cpu.a);
            cycles = 4;
            break;
        case 0xA0:
            pyemu_gameboy_and_a(gb, gb->cpu.b);
            cycles = 4;
            break;
        case 0xA1:
            pyemu_gameboy_and_a(gb, gb->cpu.c);
            cycles = 4;
            break;
        case 0xA2:
            pyemu_gameboy_and_a(gb, gb->cpu.d);
            cycles = 4;
            break;
        case 0xA3:
            pyemu_gameboy_and_a(gb, gb->cpu.e);
            cycles = 4;
            break;
        case 0xA4:
            pyemu_gameboy_and_a(gb, gb->cpu.h);
            cycles = 4;
            break;
        case 0xA5:
            pyemu_gameboy_and_a(gb, gb->cpu.l);
            cycles = 4;
            break;
        case 0xA6:
            pyemu_gameboy_and_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb)));
            cycles = 8;
            break;
        case 0xA7:
            pyemu_gameboy_and_a(gb, gb->cpu.a);
            cycles = 4;
            break;
        case 0xA8:
            pyemu_gameboy_xor_a(gb, gb->cpu.b);
            cycles = 4;
            break;
        case 0xA9:
            pyemu_gameboy_xor_a(gb, gb->cpu.c);
            cycles = 4;
            break;
        case 0xAA:
            pyemu_gameboy_xor_a(gb, gb->cpu.d);
            cycles = 4;
            break;
        case 0xAB:
            pyemu_gameboy_xor_a(gb, gb->cpu.e);
            cycles = 4;
            break;
        case 0xAC:
            pyemu_gameboy_xor_a(gb, gb->cpu.h);
            cycles = 4;
            break;
        case 0xAD:
            pyemu_gameboy_xor_a(gb, gb->cpu.l);
            cycles = 4;
            break;
        case 0xAE:
            pyemu_gameboy_xor_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb)));
            cycles = 8;
            break;
        case 0xAF:
            pyemu_gameboy_xor_a(gb, gb->cpu.a);
            cycles = 4;
            break;
        case 0xB0:
            pyemu_gameboy_or_a(gb, gb->cpu.b);
            cycles = 4;
            break;
        case 0xB1:
            pyemu_gameboy_or_a(gb, gb->cpu.c);
            cycles = 4;
            break;
        case 0xB2:
            pyemu_gameboy_or_a(gb, gb->cpu.d);
            cycles = 4;
            break;
        case 0xB3:
            pyemu_gameboy_or_a(gb, gb->cpu.e);
            cycles = 4;
            break;
        case 0xB4:
            pyemu_gameboy_or_a(gb, gb->cpu.h);
            cycles = 4;
            break;
        case 0xB5:
            pyemu_gameboy_or_a(gb, gb->cpu.l);
            cycles = 4;
            break;
        case 0xB6:
            pyemu_gameboy_or_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb)));
            cycles = 8;
            break;
        case 0xB7:
            pyemu_gameboy_or_a(gb, gb->cpu.a);
            cycles = 4;
            break;
        case 0xB8:
            pyemu_gameboy_cp_a(gb, gb->cpu.b);
            cycles = 4;
            break;
        case 0xB9:
            pyemu_gameboy_cp_a(gb, gb->cpu.c);
            cycles = 4;
            break;
        case 0xBA:
            pyemu_gameboy_cp_a(gb, gb->cpu.d);
            cycles = 4;
            break;
        case 0xBB:
            pyemu_gameboy_cp_a(gb, gb->cpu.e);
            cycles = 4;
            break;
        case 0xBC:
            pyemu_gameboy_cp_a(gb, gb->cpu.h);
            cycles = 4;
            break;
        case 0xBD:
            pyemu_gameboy_cp_a(gb, gb->cpu.l);
            cycles = 4;
            break;
        case 0xBE:
            pyemu_gameboy_cp_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb)));
            cycles = 8;
            break;
        case 0xBF:
            pyemu_gameboy_cp_a(gb, gb->cpu.a);
            cycles = 4;
            break;
        case 0xC0:
            cycles = 8;
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                cycles = 20;
            }
            break;
        case 0xC7:
        case 0xCF:
        case 0xD7:
        case 0xDF:
        case 0xE7:
        case 0xEF:
        case 0xF7:
        case 0xFF:
            pyemu_gameboy_push_u16(gb, gb->cpu.pc);
            gb->cpu.pc = (uint16_t)(opcode & 0x38);
            cycles = 16;
            break;
        case 0xC1:
            pyemu_gameboy_set_bc(gb, pyemu_gameboy_pop_u16(gb));
            cycles = 12;
            break;
        case 0xC2: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            cycles = 12;
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = address;
                cycles = 16;
            }
            break;
        }
        case 0xC3:
            gb->cpu.pc = pyemu_gameboy_fetch_u16(gb);
            cycles = 16;
            break;
        case 0xC4: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            cycles = 12;
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                pyemu_gameboy_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                cycles = 24;
            }
            break;
        }
        case 0xC8:
            cycles = 8;
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                cycles = 20;
            }
            break;
        case 0xC5:
            pyemu_gameboy_push_u16(gb, pyemu_gameboy_get_bc(gb));
            cycles = 16;
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
        case 0xC6:
            pyemu_gameboy_add_a(gb, pyemu_gameboy_fetch_u8(gb), 0);
            cycles = 8;
            break;
        case 0xC9:
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            cycles = 16;
            break;
        case 0xCA: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            cycles = 12;
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = address;
                cycles = 16;
            }
            break;
        }
        case 0xCC: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            cycles = 12;
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                pyemu_gameboy_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                cycles = 24;
            }
            break;
        }
        case 0xCD: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            pyemu_gameboy_push_u16(gb, gb->cpu.pc);
            gb->cpu.pc = address;
            cycles = 24;
            break;
        }
        case 0xCE:
            pyemu_gameboy_add_a(gb, pyemu_gameboy_fetch_u8(gb), 1);
            cycles = 8;
            break;
        case 0xD0:
            cycles = 8;
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                cycles = 20;
            }
            break;
        case 0xD1:
            pyemu_gameboy_set_de(gb, pyemu_gameboy_pop_u16(gb));
            cycles = 12;
            break;
        case 0xD2: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            cycles = 12;
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = address;
                cycles = 16;
            }
            break;
        }
        case 0xD4: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            cycles = 12;
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                pyemu_gameboy_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                cycles = 24;
            }
            break;
        }
        case 0xD5:
            pyemu_gameboy_push_u16(gb, pyemu_gameboy_get_de(gb));
            cycles = 16;
            break;
        case 0xD6:
            pyemu_gameboy_sub_a(gb, pyemu_gameboy_fetch_u8(gb));
            cycles = 8;
            break;
        case 0xD8:
            cycles = 8;
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                cycles = 20;
            }
            break;
        case 0xD9:
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            gb->ime = 1;
            gb->ime_pending = 0;
            gb->ime_delay = 0;
            cycles = 16;
            break;
        case 0xDA: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            cycles = 12;
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = address;
                cycles = 16;
            }
            break;
        }
        case 0xDC: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            cycles = 12;
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                pyemu_gameboy_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                cycles = 24;
            }
            break;
        }
        case 0xDE:
            pyemu_gameboy_sbc_a(gb, pyemu_gameboy_fetch_u8(gb));
            cycles = 8;
            break;
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
        case 0xE1:
            pyemu_gameboy_set_hl(gb, pyemu_gameboy_pop_u16(gb));
            cycles = 12;
            break;
        case 0xE5:
            pyemu_gameboy_push_u16(gb, pyemu_gameboy_get_hl(gb));
            cycles = 16;
            break;
        case 0xE6:
            pyemu_gameboy_and_a(gb, pyemu_gameboy_fetch_u8(gb));
            cycles = 8;
            break;
        case 0xE8:
            pyemu_gameboy_add_sp_signed(gb, (int8_t)pyemu_gameboy_fetch_u8(gb));
            cycles = 16;
            break;
        case 0xE9:
            gb->cpu.pc = pyemu_gameboy_get_hl(gb);
            cycles = 4;
            break;
        case 0xEA: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            pyemu_gameboy_write_memory(gb, address, gb->cpu.a);
            cycles = 16;
            break;
        }
        case 0xEE:
            pyemu_gameboy_xor_a(gb, pyemu_gameboy_fetch_u8(gb));
            cycles = 8;
            break;
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
        case 0xF1:
            pyemu_gameboy_set_af(gb, pyemu_gameboy_pop_u16(gb));
            cycles = 12;
            break;
        case 0xCB: {
            uint8_t cb_opcode = pyemu_gameboy_fetch_u8(gb);
            int reg_index = cb_opcode & 0x07;
            uint8_t value;
            cycles = 8;

            if (cb_opcode <= 0x07) {
                value = pyemu_gameboy_read_r8(gb, reg_index);
                pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_rlc8(gb, value));
                cycles = reg_index == 6 ? 16 : 8;
                break;
            }
            if (cb_opcode <= 0x0F) {
                value = pyemu_gameboy_read_r8(gb, reg_index);
                pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_rrc8(gb, value));
                cycles = reg_index == 6 ? 16 : 8;
                break;
            }
            if (cb_opcode <= 0x17) {
                value = pyemu_gameboy_read_r8(gb, reg_index);
                pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_rl8(gb, value));
                cycles = reg_index == 6 ? 16 : 8;
                break;
            }
            if (cb_opcode <= 0x1F) {
                value = pyemu_gameboy_read_r8(gb, reg_index);
                pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_rr8(gb, value));
                cycles = reg_index == 6 ? 16 : 8;
                break;
            }
            if (cb_opcode <= 0x27) {
                value = pyemu_gameboy_read_r8(gb, reg_index);
                pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_sla8(gb, value));
                cycles = reg_index == 6 ? 16 : 8;
                break;
            }
            if (cb_opcode <= 0x2F) {
                value = pyemu_gameboy_read_r8(gb, reg_index);
                pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_sra8(gb, value));
                cycles = reg_index == 6 ? 16 : 8;
                break;
            }
            if ((cb_opcode & 0xF8) == 0x30) {
                value = pyemu_gameboy_read_r8(gb, reg_index);
                pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_swap8(gb, value));
                cycles = reg_index == 6 ? 16 : 8;
                break;
            }
            if (cb_opcode <= 0x3F) {
                value = pyemu_gameboy_read_r8(gb, reg_index);
                pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_srl8(gb, value));
                cycles = reg_index == 6 ? 16 : 8;
                break;
            }
            if (cb_opcode >= 0x40 && cb_opcode <= 0x7F) {
                value = pyemu_gameboy_read_r8(gb, reg_index);
                pyemu_gameboy_bit_test(gb, value, (uint8_t)((cb_opcode - 0x40) / 8));
                cycles = reg_index == 6 ? 12 : 8;
                break;
            }
            if (cb_opcode >= 0x80 && cb_opcode <= 0xBF) {
                value = pyemu_gameboy_read_r8(gb, reg_index);
                value = (uint8_t)(value & (uint8_t)~(1U << ((cb_opcode - 0x80) / 8)));
                pyemu_gameboy_write_r8(gb, reg_index, value);
                cycles = reg_index == 6 ? 16 : 8;
                break;
            }
            if (cb_opcode >= 0xC0) {
                value = pyemu_gameboy_read_r8(gb, reg_index);
                value = (uint8_t)(value | (uint8_t)(1U << ((cb_opcode - 0xC0) / 8)));
                pyemu_gameboy_write_r8(gb, reg_index, value);
                cycles = reg_index == 6 ? 16 : 8;
                break;
            }

            gb->cpu.halted = 1;
            gb->faulted = 0;
            gb->cpu.pc = (uint16_t)(gb->cpu.pc - 2);
            cycles = 4;
            break;
        }
        case 0xF3:
            gb->ime = 0;
            gb->ime_pending = 0;
            gb->ime_delay = 0;
            cycles = 4;
            break;
        case 0xF5:
            pyemu_gameboy_push_u16(gb, pyemu_gameboy_get_af(gb));
            cycles = 16;
            break;
        case 0xF6:
            pyemu_gameboy_or_a(gb, pyemu_gameboy_fetch_u8(gb));
            cycles = 8;
            break;
        case 0xF8:
            pyemu_gameboy_set_hl(gb, pyemu_gameboy_sp_plus_signed(gb, (int8_t)pyemu_gameboy_fetch_u8(gb)));
            cycles = 12;
            break;
        case 0xF9:
            gb->cpu.sp = pyemu_gameboy_get_hl(gb);
            cycles = 8;
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
        case 0xFE:
            pyemu_gameboy_cp_a(gb, pyemu_gameboy_fetch_u8(gb));
            cycles = 8;
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

static uint32_t pyemu_gameboy_cycles_until_vblank(const pyemu_gameboy_system* gb) {
    uint8_t ly = gb->memory[PYEMU_IO_LY];
    uint32_t ppu_counter = gb->ppu_counter % PYEMU_GAMEBOY_CYCLES_PER_SCANLINE;

    if (!pyemu_gameboy_lcd_enabled(gb)) {
        return 0;
    }

    if (ly < 144) {
        return (uint32_t)((144U - (uint32_t)ly) * PYEMU_GAMEBOY_CYCLES_PER_SCANLINE - ppu_counter);
    }

    return (uint32_t)(((154U - (uint32_t)ly + 144U) * PYEMU_GAMEBOY_CYCLES_PER_SCANLINE) - ppu_counter);
}

static void pyemu_gameboy_fast_forward_to_vblank(pyemu_gameboy_system* gb) {
    uint8_t ly = gb->memory[PYEMU_IO_LY];
    uint32_t skip_cycles = pyemu_gameboy_cycles_until_vblank(gb);
    uint8_t line;

    gb->cycle_count += (uint64_t)skip_cycles;
    gb->div_counter += skip_cycles;
    gb->memory[PYEMU_IO_DIV] = (uint8_t)((gb->div_counter >> 8) & 0xFF);

    if (ly < PYEMU_GAMEBOY_HEIGHT) {
        for (line = ly; line < PYEMU_GAMEBOY_HEIGHT; ++line) {
            pyemu_gameboy_latch_scanline_registers(gb, line);
        }
    }

    gb->ppu_counter = 0;
    gb->memory[PYEMU_IO_LY] = 144;
    pyemu_gameboy_request_interrupt(gb, PYEMU_INTERRUPT_VBLANK);
    pyemu_gameboy_update_stat(gb);
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

    pyemu_gameboy_update_demo_frame(gb);
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
    pyemu_gameboy_get_memory,
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

