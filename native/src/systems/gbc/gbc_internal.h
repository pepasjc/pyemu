#pragma once

#include "pyemu/systems/gbc/gbc_system.h"

#include <stddef.h>
#include <stdint.h>

#define PYEMU_GBC_MEMORY_SIZE 0x10000
#define PYEMU_GBC_WIDTH 160
#define PYEMU_GBC_HEIGHT 144
#define PYEMU_GBC_RGBA_SIZE (PYEMU_GBC_WIDTH * PYEMU_GBC_HEIGHT * 4)
#define PYEMU_GBC_ROM_BANK0_SIZE 0x4000
#define PYEMU_GBC_ROM_BANKX_SIZE 0x4000
#define PYEMU_GBC_VRAM_SIZE 0x4000
#define PYEMU_GBC_WRAM_BANK_SIZE 0x1000
#define PYEMU_GBC_WRAM_BANK_COUNT 8
#define PYEMU_GBC_WRAM_SIZE (PYEMU_GBC_WRAM_BANK_SIZE * PYEMU_GBC_WRAM_BANK_COUNT)
#define PYEMU_GBC_HRAM_SIZE 0x007F
#define PYEMU_GBC_OAM_SIZE 0x00A0
#define PYEMU_GBC_ERAM_BANK_SIZE 0x2000
#define PYEMU_GBC_MAX_ERAM_SIZE 0x8000
#define PYEMU_GBC_TITLE_START 0x0134
#define PYEMU_GBC_TITLE_END 0x0143
#define PYEMU_GBC_CYCLES_PER_FRAME 70224
#define PYEMU_GBC_CYCLES_PER_SCANLINE 456
#define PYEMU_GBC_DOUBLE_SPEED_CYCLES_PER_FRAME 35112
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
#define PYEMU_GBC_SPEED 0xFF4D
#define PYEMU_GBC_BG_PALETTE_INDEX 0xFF68
#define PYEMU_GBC_BG_PALETTE_DATA 0xFF69
#define PYEMU_GBC_SPRITE_PALETTE_INDEX 0xFF6A
#define PYEMU_GBC_SPRITE_PALETTE_DATA 0xFF6B
#define PYEMU_GBC_VBK 0xFF4F
#define PYEMU_GBC_HDMA1 0xFF51
#define PYEMU_GBC_HDMA2 0xFF52
#define PYEMU_GBC_HDMA3 0xFF53
#define PYEMU_GBC_HDMA4 0xFF54
#define PYEMU_GBC_HDMA5 0xFF55
#define PYEMU_GBC_SVBK 0xFF70
#define PYEMU_INTERRUPT_VBLANK 0x01
#define PYEMU_INTERRUPT_LCD 0x02
#define PYEMU_INTERRUPT_TIMER 0x04
#define PYEMU_INTERRUPT_SERIAL 0x08
#define PYEMU_INTERRUPT_JOYPAD 0x10
#define PYEMU_FLAG_Z 0x80
#define PYEMU_FLAG_N 0x40
#define PYEMU_FLAG_H 0x20
#define PYEMU_FLAG_C 0x10
#define PYEMU_GBC_STATE_MAGIC 0x50474354U
#define PYEMU_GBC_STATE_VERSION 3U
#define PYEMU_GBC_BLOCK_CACHE_SIZE 8192
#define PYEMU_GBC_BLOCK_MAX_INSNS 16
#define PYEMU_GBC_AUDIO_SAMPLE_RATE 48000
#define PYEMU_GBC_AUDIO_CHANNELS 2
#define PYEMU_GBC_AUDIO_SAMPLES_PER_FRAME 804
#define PYEMU_GBC_AUDIO_SAMPLE_COUNT (PYEMU_GBC_AUDIO_SAMPLES_PER_FRAME * PYEMU_GBC_AUDIO_CHANNELS)

typedef enum pyemu_gbc_block_op_type {
    PYEMU_GBC_BLOCK_OP_INVALID = 0,
    PYEMU_GBC_BLOCK_OP_NOP,
    PYEMU_GBC_BLOCK_OP_LD_R_D8,
    PYEMU_GBC_BLOCK_OP_LD_HL_D16,
    PYEMU_GBC_BLOCK_OP_LD_DE_D16,
    PYEMU_GBC_BLOCK_OP_LDH_A_A8,
    PYEMU_GBC_BLOCK_OP_LDH_A8_A,
    PYEMU_GBC_BLOCK_OP_LD_A_A16,
    PYEMU_GBC_BLOCK_OP_LD_A16_A,
    PYEMU_GBC_BLOCK_OP_XOR_A,
    PYEMU_GBC_BLOCK_OP_AND_A,
    PYEMU_GBC_BLOCK_OP_INC_A,
    PYEMU_GBC_BLOCK_OP_CP_D8,
    PYEMU_GBC_BLOCK_OP_LD_HL_INC_A,
    PYEMU_GBC_BLOCK_OP_LD_A_HL_INC,
    PYEMU_GBC_BLOCK_OP_LD_A_HL_DEC,
    PYEMU_GBC_BLOCK_OP_LD_HL_DEC_A,
    PYEMU_GBC_BLOCK_OP_DEC_B,
    PYEMU_GBC_BLOCK_OP_LD_A_L,
    PYEMU_GBC_BLOCK_OP_LD_L_A,
    PYEMU_GBC_BLOCK_OP_JR,
    PYEMU_GBC_BLOCK_OP_JR_NZ,
    PYEMU_GBC_BLOCK_OP_JR_Z,
    PYEMU_GBC_BLOCK_OP_JR_NC,
    PYEMU_GBC_BLOCK_OP_JR_C,
    PYEMU_GBC_BLOCK_OP_RET,
    PYEMU_GBC_BLOCK_OP_HALT
} pyemu_gbc_block_op_type;

typedef struct pyemu_gbc_block_insn {
    uint8_t type;
    uint8_t length;
    uint8_t operand8;
    int8_t relative;
    uint16_t operand16;
} pyemu_gbc_block_insn;

typedef struct pyemu_gbc_block_cache_entry {
    uint8_t valid;
    uint8_t terminates;
    uint8_t bank;
    uint16_t pc;
    uint8_t insn_count;
    pyemu_gbc_block_insn insns[PYEMU_GBC_BLOCK_MAX_INSNS];
} pyemu_gbc_block_cache_entry;

typedef struct pyemu_gbc_pulse_channel {
    uint8_t enabled;
    uint8_t duty;
    uint8_t volume;
    uint16_t frequency_raw;
    float phase;
    /* Envelope */
    uint8_t envelope_period;
    uint8_t envelope_increase;
    uint32_t envelope_cycles;
    /* Sweep (channel 1 only) */
    uint16_t sweep_shadow_freq;
    uint8_t sweep_timer;
    uint8_t sweep_enabled;
    uint32_t sweep_cycles;
} pyemu_gbc_pulse_channel;

typedef struct pyemu_gbc_wave_channel {
    uint8_t enabled;
    uint8_t volume_code;
    uint16_t frequency_raw;
    float phase;
} pyemu_gbc_wave_channel;

typedef struct pyemu_gbc_noise_channel {
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
} pyemu_gbc_noise_channel;

typedef struct pyemu_gbc_palette {
    uint8_t data[8];
} pyemu_gbc_palette;

typedef struct pyemu_gbc_system {
    pyemu_system base;
    pyemu_cpu_state cpu;
    uint8_t memory[PYEMU_GBC_MEMORY_SIZE];
    uint8_t frame[PYEMU_GBC_RGBA_SIZE];
    uint8_t line_scx[PYEMU_GBC_HEIGHT];
    uint8_t line_scy[PYEMU_GBC_HEIGHT];
    uint8_t line_wx[PYEMU_GBC_HEIGHT];
    uint8_t line_wy[PYEMU_GBC_HEIGHT];
    uint8_t line_lcdc[PYEMU_GBC_HEIGHT];
    uint8_t line_bgp[PYEMU_GBC_HEIGHT];
    uint8_t line_obp0[PYEMU_GBC_HEIGHT];
    uint8_t line_obp1[PYEMU_GBC_HEIGHT];
    uint8_t line_win_y[PYEMU_GBC_HEIGHT];
    uint8_t window_line_counter;
    uint8_t rom_bank0[PYEMU_GBC_ROM_BANK0_SIZE];
    uint8_t rom_bankx[PYEMU_GBC_ROM_BANKX_SIZE];
    uint8_t* rom_data;
    uint8_t vram[PYEMU_GBC_VRAM_SIZE];
    uint8_t wram[PYEMU_GBC_WRAM_SIZE];
    uint8_t hram[PYEMU_GBC_HRAM_SIZE];
    uint8_t eram[PYEMU_GBC_MAX_ERAM_SIZE];
    pyemu_gbc_palette bg_palettes[8];
    pyemu_gbc_palette sp_palettes[8];
    uint8_t current_vbk;
    uint8_t current_wram_bank;
    uint8_t bg_palette_index;
    uint8_t bg_palette_data;
    uint8_t sp_palette_index;
    uint8_t sp_palette_data;
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
    pyemu_gbc_block_cache_entry block_cache[PYEMU_GBC_BLOCK_CACHE_SIZE];
    int double_speed;
    pyemu_gbc_pulse_channel pulse1;
    pyemu_gbc_pulse_channel pulse2;
    pyemu_gbc_wave_channel wave;
    pyemu_gbc_noise_channel noise;
    int16_t audio_frame[PYEMU_GBC_AUDIO_SAMPLE_COUNT];
    int16_t audio_channels[4][PYEMU_GBC_AUDIO_SAMPLE_COUNT];
    int faulted;
    uint32_t serial_counter; /* cycles remaining until serial transfer completes */
    /* MBC3 RTC live registers */
    uint8_t rtc_s;           /* seconds 0-59 */
    uint8_t rtc_m;           /* minutes 0-59 */
    uint8_t rtc_h;           /* hours 0-23 */
    uint8_t rtc_dl;          /* day counter low byte */
    uint8_t rtc_dh;          /* day counter high + halt (bit 6) + carry (bit 7) */
    /* MBC3 RTC latched registers (written by 0x00->0x01 sequence at 0x6000-0x7FFF) */
    uint8_t rtc_latch_s;
    uint8_t rtc_latch_m;
    uint8_t rtc_latch_h;
    uint8_t rtc_latch_dl;
    uint8_t rtc_latch_dh;
    uint8_t rtc_latch_ready; /* 1 = last latch write was 0x00, waiting for 0x01 */
    uint32_t rtc_cycle_accum; /* accumulated cycles toward next RTC second tick */
} pyemu_gbc_system;

typedef struct pyemu_gbc_state_file {
    uint32_t magic;
    uint32_t version;
    pyemu_cpu_state cpu;
    uint8_t memory[PYEMU_GBC_MEMORY_SIZE];
    uint8_t vram[PYEMU_GBC_VRAM_SIZE];
    uint8_t wram[PYEMU_GBC_WRAM_SIZE];
    uint8_t hram[PYEMU_GBC_HRAM_SIZE];
    uint8_t eram[PYEMU_GBC_MAX_ERAM_SIZE];
    uint8_t line_scx[PYEMU_GBC_HEIGHT];
    uint8_t line_scy[PYEMU_GBC_HEIGHT];
    uint8_t line_wx[PYEMU_GBC_HEIGHT];
    uint8_t line_wy[PYEMU_GBC_HEIGHT];
    uint8_t line_lcdc[PYEMU_GBC_HEIGHT];
    uint8_t line_bgp[PYEMU_GBC_HEIGHT];
    uint8_t line_obp0[PYEMU_GBC_HEIGHT];
    uint8_t line_obp1[PYEMU_GBC_HEIGHT];
    uint8_t line_win_y[PYEMU_GBC_HEIGHT];
    uint8_t window_line_counter;
    pyemu_gbc_palette bg_palettes[8];
    pyemu_gbc_palette sp_palettes[8];
    uint8_t current_vbk;
    uint8_t current_wram_bank;
    uint8_t bg_palette_index;
    uint8_t sp_palette_index;
    int ime_delay;
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
    int double_speed;
    int faulted;
    uint32_t serial_counter;
    /* MBC3 RTC fields */
    uint8_t rtc_s;
    uint8_t rtc_m;
    uint8_t rtc_h;
    uint8_t rtc_dl;
    uint8_t rtc_dh;
    uint8_t rtc_latch_s;
    uint8_t rtc_latch_m;
    uint8_t rtc_latch_h;
    uint8_t rtc_latch_dl;
    uint8_t rtc_latch_dh;
    uint8_t rtc_latch_ready;
    uint32_t rtc_cycle_accum;
} pyemu_gbc_state_file;

void pyemu_gbc_update_audio_frame(pyemu_gbc_system* gbc);
void pyemu_gbc_apu_handle_write(pyemu_gbc_system* gbc, uint16_t address, uint8_t value);
void pyemu_gbc_get_audio_buffer(const pyemu_system* system, pyemu_audio_buffer* out_audio);

int pyemu_gbc_uses_mbc1(const pyemu_gbc_system* gbc);
int pyemu_gbc_uses_mbc3(const pyemu_gbc_system* gbc);
int pyemu_gbc_uses_mbc5(const pyemu_gbc_system* gbc);
int pyemu_gbc_has_battery(const pyemu_gbc_system* gbc);
int pyemu_gbc_save_file_present(const pyemu_gbc_system* gbc);
int pyemu_gbc_has_external_ram(const pyemu_gbc_system* gbc);
size_t pyemu_gbc_eram_size_from_code(uint8_t ram_size_code);
uint8_t pyemu_gbc_mbc1_upper_bits(const pyemu_gbc_system* gbc);
uint8_t pyemu_gbc_mbc1_mode(const pyemu_gbc_system* gbc);
uint8_t pyemu_gbc_current_rom_bank(const pyemu_gbc_system* gbc);
size_t pyemu_gbc_current_eram_offset(const pyemu_gbc_system* gbc);
uint8_t pyemu_gbc_normalize_mbc1_bank(const pyemu_gbc_system* gbc, uint8_t value);
int pyemu_gbc_save_battery_ram(const pyemu_gbc_system* gbc);
void pyemu_gbc_load_battery_ram(pyemu_gbc_system* gbc);
void pyemu_gbc_refresh_rom_mapping(pyemu_gbc_system* gbc);
void pyemu_gbc_refresh_eram_window(pyemu_gbc_system* gbc);
uint8_t pyemu_gbc_current_joypad_value(const pyemu_gbc_system* gbc);
void pyemu_gbc_refresh_joypad(pyemu_gbc_system* gbc, uint8_t previous_value);
void pyemu_gbc_set_joypad_state(pyemu_system* system, uint8_t buttons, uint8_t directions);
void pyemu_gbc_set_bus_tracking(pyemu_system* system, int enabled);
void pyemu_gbc_request_interrupt(pyemu_gbc_system* gbc, uint8_t mask);
int pyemu_gbc_lcd_enabled(const pyemu_gbc_system* gbc);
uint8_t pyemu_gbc_pending_interrupts(const pyemu_gbc_system* gbc);
void pyemu_gbc_update_stat(pyemu_gbc_system* gbc);
void pyemu_gbc_latch_scanline_registers(pyemu_gbc_system* gbc, uint8_t ly);
int pyemu_gbc_cpu_can_access_vram(const pyemu_gbc_system* gbc);
int pyemu_gbc_cpu_can_access_oam(const pyemu_gbc_system* gbc);
int pyemu_gbc_timer_signal(const pyemu_gbc_system* gbc);
void pyemu_gbc_apply_timer_edge(pyemu_gbc_system* gbc, int old_signal, int new_signal);
void pyemu_gbc_update_demo_frame(pyemu_gbc_system* gbc);
void pyemu_gbc_tick(pyemu_gbc_system* gbc, int cycles);
uint32_t pyemu_gbc_cycles_until_vblank(const pyemu_gbc_system* gbc);
void pyemu_gbc_fast_forward_to_vblank(pyemu_gbc_system* gbc);
uint16_t pyemu_gbc_get_af(const pyemu_gbc_system* gbc);
uint16_t pyemu_gbc_get_bc(const pyemu_gbc_system* gbc);
uint16_t pyemu_gbc_get_de(const pyemu_gbc_system* gbc);
uint16_t pyemu_gbc_get_hl(const pyemu_gbc_system* gbc);
void pyemu_gbc_set_af(pyemu_gbc_system* gbc, uint16_t value);
void pyemu_gbc_set_bc(pyemu_gbc_system* gbc, uint16_t value);
void pyemu_gbc_set_de(pyemu_gbc_system* gbc, uint16_t value);
void pyemu_gbc_set_hl(pyemu_gbc_system* gbc, uint16_t value);
void pyemu_gbc_set_flag(pyemu_gbc_system* gbc, uint8_t mask, int enabled);
int pyemu_gbc_get_flag(const pyemu_gbc_system* gbc, uint8_t mask);
void pyemu_gbc_set_flags_znhc(pyemu_gbc_system* gbc, int z, int n, int h, int c);
uint8_t pyemu_gbc_inc8(pyemu_gbc_system* gbc, uint8_t value);
uint8_t pyemu_gbc_dec8(pyemu_gbc_system* gbc, uint8_t value);
uint8_t pyemu_gbc_swap8(pyemu_gbc_system* gbc, uint8_t value);
void pyemu_gbc_rlca(pyemu_gbc_system* gbc);
void pyemu_gbc_rrca(pyemu_gbc_system* gbc);
void pyemu_gbc_rla(pyemu_gbc_system* gbc);
void pyemu_gbc_rra(pyemu_gbc_system* gbc);
uint8_t pyemu_gbc_rlc8(pyemu_gbc_system* gbc, uint8_t value);
uint8_t pyemu_gbc_rrc8(pyemu_gbc_system* gbc, uint8_t value);
uint8_t pyemu_gbc_rl8(pyemu_gbc_system* gbc, uint8_t value);
uint8_t pyemu_gbc_rr8(pyemu_gbc_system* gbc, uint8_t value);
uint8_t pyemu_gbc_sla8(pyemu_gbc_system* gbc, uint8_t value);
uint8_t pyemu_gbc_sra8(pyemu_gbc_system* gbc, uint8_t value);
uint8_t pyemu_gbc_srl8(pyemu_gbc_system* gbc, uint8_t value);
void pyemu_gbc_add_a(pyemu_gbc_system* gbc, uint8_t value, int with_carry);
void pyemu_gbc_and_a(pyemu_gbc_system* gbc, uint8_t value);
void pyemu_gbc_xor_a(pyemu_gbc_system* gbc, uint8_t value);
void pyemu_gbc_or_a(pyemu_gbc_system* gbc, uint8_t value);
void pyemu_gbc_cp_a(pyemu_gbc_system* gbc, uint8_t value);
void pyemu_gbc_sub_a(pyemu_gbc_system* gbc, uint8_t value);
void pyemu_gbc_sbc_a(pyemu_gbc_system* gbc, uint8_t value);
void pyemu_gbc_add_sp_signed(pyemu_gbc_system* gbc, int8_t value);
uint16_t pyemu_gbc_sp_plus_signed(pyemu_gbc_system* gbc, int8_t value);
void pyemu_gbc_bit_test(pyemu_gbc_system* gbc, uint8_t value, uint8_t bit);
void pyemu_gbc_add_hl(pyemu_gbc_system* gbc, uint16_t value);
void pyemu_gbc_daa(pyemu_gbc_system* gbc);
uint8_t pyemu_gbc_peek_memory(const pyemu_gbc_system* gbc, uint16_t address);
uint8_t pyemu_gbc_read_memory(const pyemu_gbc_system* gbc, uint16_t address);
void pyemu_gbc_write_memory(pyemu_gbc_system* gbc, uint16_t address, uint8_t value);
uint8_t pyemu_gbc_fetch_u8(pyemu_gbc_system* gbc);
uint16_t pyemu_gbc_fetch_u16(pyemu_gbc_system* gbc);
void pyemu_gbc_push_u16(pyemu_gbc_system* gbc, uint16_t value);
uint16_t pyemu_gbc_pop_u16(pyemu_gbc_system* gbc);
int pyemu_gbc_service_interrupts(pyemu_gbc_system* gbc);
uint8_t pyemu_gbc_read_r8(pyemu_gbc_system* gbc, int index);
void pyemu_gbc_write_r8(pyemu_gbc_system* gbc, int index, uint8_t value);
int pyemu_gbc_execute_cb(pyemu_gbc_system* gbc);
int pyemu_gbc_execute_load_store(pyemu_gbc_system* gbc, uint8_t opcode);
int pyemu_gbc_execute_control_flow(pyemu_gbc_system* gbc, uint8_t opcode);
int pyemu_gbc_execute_alu(pyemu_gbc_system* gbc, uint8_t opcode);
