#pragma once

#include "pyemu/systems/gameboy/gameboy_system.h"

#include <stddef.h>
#include <stdint.h>

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
#define PYEMU_GAMEBOY_AUDIO_SAMPLE_RATE 48000
#define PYEMU_GAMEBOY_AUDIO_CHANNELS 2
#define PYEMU_GAMEBOY_AUDIO_SAMPLES_PER_FRAME 804
#define PYEMU_GAMEBOY_AUDIO_SAMPLE_COUNT (PYEMU_GAMEBOY_AUDIO_SAMPLES_PER_FRAME * PYEMU_GAMEBOY_AUDIO_CHANNELS)

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

typedef struct pyemu_gameboy_pulse_channel {
    uint8_t enabled;
    uint8_t duty;
    uint8_t volume;
    uint16_t frequency_raw;
    float phase;
} pyemu_gameboy_pulse_channel;

typedef struct pyemu_gameboy_wave_channel {
    uint8_t enabled;
    uint8_t volume_code;
    uint16_t frequency_raw;
    float phase;
} pyemu_gameboy_wave_channel;

typedef struct pyemu_gameboy_noise_channel {
    uint8_t enabled;
    uint8_t volume;
    uint8_t width_mode;
    uint8_t divisor_code;
    uint8_t clock_shift;
    uint8_t envelope_period;
    uint8_t envelope_increase;
    uint8_t length_counter;
    uint8_t length_enabled;
    uint32_t envelope_cycles;
    uint32_t length_cycles;
    uint16_t lfsr;
    float phase;
} pyemu_gameboy_noise_channel;

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
    pyemu_gameboy_pulse_channel pulse1;
    pyemu_gameboy_pulse_channel pulse2;
    pyemu_gameboy_wave_channel wave;
    pyemu_gameboy_noise_channel noise;
    int16_t audio_frame[PYEMU_GAMEBOY_AUDIO_SAMPLE_COUNT];
    int16_t audio_channels[4][PYEMU_GAMEBOY_AUDIO_SAMPLE_COUNT];
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

void pyemu_gameboy_update_audio_frame(pyemu_gameboy_system* gb);
void pyemu_gameboy_apu_handle_write(pyemu_gameboy_system* gb, uint16_t address, uint8_t value);
void pyemu_gameboy_get_audio_buffer(const pyemu_system* system, pyemu_audio_buffer* out_audio);
int pyemu_gameboy_uses_mbc1(const pyemu_gameboy_system* gb);
int pyemu_gameboy_uses_mbc3(const pyemu_gameboy_system* gb);
int pyemu_gameboy_has_battery(const pyemu_gameboy_system* gb);
int pyemu_gameboy_save_file_present(const pyemu_gameboy_system* gb);
int pyemu_gameboy_has_external_ram(const pyemu_gameboy_system* gb);
size_t pyemu_gameboy_eram_size_from_code(uint8_t ram_size_code);
uint8_t pyemu_gameboy_mbc1_upper_bits(const pyemu_gameboy_system* gb);
uint8_t pyemu_gameboy_mbc1_mode(const pyemu_gameboy_system* gb);
uint8_t pyemu_gameboy_current_rom_bank(const pyemu_gameboy_system* gb);
size_t pyemu_gameboy_current_eram_offset(const pyemu_gameboy_system* gb);
uint8_t pyemu_gameboy_normalize_mbc1_bank(const pyemu_gameboy_system* gb, uint8_t value);
int pyemu_gameboy_save_battery_ram(const pyemu_gameboy_system* gb);
void pyemu_gameboy_load_battery_ram(pyemu_gameboy_system* gb);
void pyemu_gameboy_refresh_rom_mapping(pyemu_gameboy_system* gb);
void pyemu_gameboy_refresh_eram_window(pyemu_gameboy_system* gb);
void pyemu_gameboy_request_interrupt(pyemu_gameboy_system* gb, uint8_t mask);
int pyemu_gameboy_lcd_enabled(const pyemu_gameboy_system* gb);
uint8_t pyemu_gameboy_pending_interrupts(const pyemu_gameboy_system* gb);
void pyemu_gameboy_update_stat(pyemu_gameboy_system* gb);
void pyemu_gameboy_latch_scanline_registers(pyemu_gameboy_system* gb, uint8_t ly);
int pyemu_gameboy_cpu_can_access_vram(const pyemu_gameboy_system* gb);
int pyemu_gameboy_cpu_can_access_oam(const pyemu_gameboy_system* gb);
int pyemu_gameboy_timer_signal(const pyemu_gameboy_system* gb);
void pyemu_gameboy_apply_timer_edge(pyemu_gameboy_system* gb, int old_signal, int new_signal);
void pyemu_gameboy_update_demo_frame(pyemu_gameboy_system* gb);
void pyemu_gameboy_tick(pyemu_gameboy_system* gb, int cycles);
uint32_t pyemu_gameboy_cycles_until_vblank(const pyemu_gameboy_system* gb);
void pyemu_gameboy_fast_forward_to_vblank(pyemu_gameboy_system* gb);
