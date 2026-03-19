#include "gbc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pyemu_gbc_uses_mbc1(const pyemu_gbc_system* gbc) {
    return gbc->cartridge_type >= 0x01 && gbc->cartridge_type <= 0x03;
}

int pyemu_gbc_uses_mbc3(const pyemu_gbc_system* gbc) {
    return gbc->cartridge_type >= 0x05 && gbc->cartridge_type <= 0x06;
}

int pyemu_gbc_has_battery(const pyemu_gbc_system* gbc) {
    return gbc->cartridge_type == 0x03 || gbc->cartridge_type == 0x06 || gbc->cartridge_type == 0x09 ||
           gbc->cartridge_type == 0x0D || gbc->cartridge_type == 0x0F || gbc->cartridge_type == 0x10 ||
           gbc->cartridge_type == 0x13 || gbc->cartridge_type == 0x1B || gbc->cartridge_type == 0x1E ||
           gbc->cartridge_type == 0x22 || gbc->cartridge_type == 0xFF;
}

int pyemu_gbc_save_file_present(const pyemu_gbc_system* gbc) {
    char save_path[280];
    size_t i;
    if (!gbc->rom_loaded || !pyemu_gbc_has_battery(gbc)) {
        return 0;
    }
    strncpy(save_path, gbc->loaded_rom, sizeof(save_path) - 1);
    save_path[sizeof(save_path) - 1] = '\0';
    for (i = 0; i < sizeof(save_path); ++i) {
        if (save_path[i] == '.') {
            save_path[i] = '\0';
            break;
        }
    }
    strncat(save_path, ".sav", sizeof(save_path) - strlen(save_path) - 1);
    FILE* f = fopen(save_path, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

int pyemu_gbc_has_external_ram(const pyemu_gbc_system* gbc) {
    return gbc->eram_size > 0;
}

size_t pyemu_gbc_eram_size_from_code(uint8_t ram_size_code) {
    static const size_t sizes[] = {0, 0, 0, 0x2000, 0x8000, 0x20000, 0x10000};
    if (ram_size_code <= 6) {
        return sizes[ram_size_code];
    }
    if (ram_size_code == 0x37) {
        return 0x8000;
    }
    if (ram_size_code == 0x38) {
        return 0x20000;
    }
    if (ram_size_code == 0x39) {
        return 0x10000;
    }
    return 0;
}

uint8_t pyemu_gbc_mbc1_upper_bits(const pyemu_gbc_system* gbc) {
    return (uint8_t)((gbc->mbc3_ram_bank >> 1) & 0x03);
}

uint8_t pyemu_gbc_mbc1_mode(const pyemu_gbc_system* gbc) {
    (void)gbc;
    return 0;
}

uint8_t pyemu_gbc_current_rom_bank(const pyemu_gbc_system* gbc) {
    if (pyemu_gbc_uses_mbc1(gbc)) {
        return gbc->mbc1_rom_bank;
    }
    if (pyemu_gbc_uses_mbc3(gbc)) {
        return gbc->mbc3_rom_bank;
    }
    return 1;
}

size_t pyemu_gbc_current_eram_offset(const pyemu_gbc_system* gbc) {
    size_t bank = 0;
    if (pyemu_gbc_uses_mbc1(gbc) && pyemu_gbc_mbc1_mode(gbc)) {
        bank = pyemu_gbc_mbc1_upper_bits(gbc);
    } else if (pyemu_gbc_uses_mbc3(gbc)) {
        bank = gbc->mbc3_ram_bank;
    }
    if (bank >= gbc->eram_bank_count) {
        bank = 0;
    }
    return bank * PYEMU_GBC_ERAM_BANK_SIZE;
}

uint8_t pyemu_gbc_normalize_mbc1_bank(const pyemu_gbc_system* gbc, uint8_t value) {
    uint8_t bank = (uint8_t)(value & 0x1F);
    if (bank == 0) {
        bank = 1;
    }
    if (gbc->rom_bank_count > 0) {
        bank = (uint8_t)(bank % gbc->rom_bank_count);
        if (bank == 0) {
            bank = 1;
        }
    }
    return bank;
}

void pyemu_gbc_refresh_rom_mapping(pyemu_gbc_system* gbc) {
    size_t bank = pyemu_gbc_current_rom_bank(gbc);
    if (bank == 0) {
        bank = 1;
    }
    if (gbc->rom_data != NULL) {
        memcpy(gbc->rom_bank0, gbc->rom_data, PYEMU_GBC_ROM_BANK0_SIZE);
        if (bank < gbc->rom_bank_count) {
            memcpy(gbc->rom_bankx, gbc->rom_data + (bank * PYEMU_GBC_ROM_BANKX_SIZE), PYEMU_GBC_ROM_BANKX_SIZE);
        }
    }
}

void pyemu_gbc_refresh_eram_window(pyemu_gbc_system* gbc) {
    size_t offset = pyemu_gbc_current_eram_offset(gbc);
    if (gbc->eram_size > 0) {
        memcpy(gbc->memory + 0xA000, gbc->eram + offset, PYEMU_GBC_ERAM_BANK_SIZE);
    }
}

uint8_t pyemu_gbc_current_joypad_value(const pyemu_gbc_system* gbc) {
    uint8_t result = 0xCF;
    if ((gbc->memory[0xFF00] & 0x20) == 0) {
        result = (uint8_t)(result & 0xF0);
        result |= gbc->joypad_directions;
    }
    if ((gbc->memory[0xFF00] & 0x10) == 0) {
        result = (uint8_t)(result & 0xF0);
        result |= gbc->joypad_buttons;
    }
    return result;
}

void pyemu_gbc_refresh_joypad(pyemu_gbc_system* gbc, uint8_t previous_value) {
    uint8_t current = pyemu_gbc_current_joypad_value(gbc);
    uint8_t changed = (uint8_t)(previous_value ^ current);
    uint8_t selected = (uint8_t)((gbc->memory[0xFF00] >> 4) & 0x03);
    if (selected == 0 && (changed & 0x0F) != 0) {
        pyemu_gbc_request_interrupt(gbc, PYEMU_INTERRUPT_JOYPAD);
    } else if (selected == 1 && (changed & 0x0F) != 0) {
        pyemu_gbc_request_interrupt(gbc, PYEMU_INTERRUPT_JOYPAD);
    }
}

void pyemu_gbc_set_joypad_state(pyemu_system* system, uint8_t buttons, uint8_t directions) {
    pyemu_gbc_system* gbc = (pyemu_gbc_system*)system;
    uint8_t prev_buttons = gbc->joypad_buttons;
    uint8_t prev_directions = gbc->joypad_directions;
    gbc->joypad_buttons = (uint8_t)((buttons & 0x0F) | 0xF0);
    gbc->joypad_directions = (uint8_t)((directions & 0x0F) | 0xF0);
    if (gbc->memory[0xFF00] != 0) {
        uint8_t prev_joypad = pyemu_gbc_current_joypad_value(gbc);
        gbc->joypad_buttons = prev_buttons;
        gbc->joypad_directions = prev_directions;
        uint8_t curr_joypad = pyemu_gbc_current_joypad_value(gbc);
        if (prev_joypad != curr_joypad) {
            pyemu_gbc_refresh_joypad(gbc, prev_joypad);
        }
    }
    gbc->joypad_buttons = (uint8_t)((buttons & 0x0F) | 0xF0);
    gbc->joypad_directions = (uint8_t)((directions & 0x0F) | 0xF0);
}

void pyemu_gbc_set_bus_tracking(pyemu_system* system, int enabled) {
    pyemu_gbc_system* gbc = (pyemu_gbc_system*)system;
    gbc->bus_tracking_enabled = (uint8_t)(enabled ? 1 : 0);
}

void pyemu_gbc_request_interrupt(pyemu_gbc_system* gbc, uint8_t mask) {
    gbc->memory[PYEMU_IO_IF] = (uint8_t)(gbc->memory[PYEMU_IO_IF] | mask);
}

int pyemu_gbc_lcd_enabled(const pyemu_gbc_system* gbc) {
    return (gbc->memory[PYEMU_IO_LCDC] & 0x80) != 0;
}

uint8_t pyemu_gbc_pending_interrupts(const pyemu_gbc_system* gbc) {
    return (uint8_t)(gbc->memory[PYEMU_IO_IF] & gbc->memory[PYEMU_IO_IE] & 0x1F);
}

void pyemu_gbc_update_stat(pyemu_gbc_system* gbc) {
    uint8_t ly = gbc->memory[PYEMU_IO_LY];
    uint8_t lyc = gbc->memory[PYEMU_IO_LYC];
    int coincidence = (ly == lyc) ? 1 : 0;
    gbc->stat_coincidence = (uint8_t)coincidence;
    uint8_t stat = gbc->memory[PYEMU_IO_STAT];
    uint8_t mode = (uint8_t)(stat & 0x03);
    uint8_t enabled = (uint8_t)((stat >> 3) & 0x0F);
    int trigger = 0;
    if ((enabled & 0x04) != 0 && coincidence) {
        trigger = 1;
    }
    if ((enabled & 0x08) != 0 && mode == 2) {
        trigger = 1;
    }
    if ((enabled & 0x10) != 0 && mode == 1) {
        trigger = 1;
    }
    if ((enabled & 0x20) != 0 && mode == 0) {
        trigger = 1;
    }
    if (trigger && !gbc->stat_irq_line) {
        pyemu_gbc_request_interrupt(gbc, PYEMU_INTERRUPT_LCD);
    }
    gbc->stat_irq_line = (uint8_t)(trigger ? 1 : 0);
}

int pyemu_gbc_cpu_can_access_vram(const pyemu_gbc_system* gbc) {
    uint8_t mode = (uint8_t)(gbc->memory[PYEMU_IO_STAT] & 0x03);
    return mode != 3;
}

int pyemu_gbc_cpu_can_access_oam(const pyemu_gbc_system* gbc) {
    uint8_t mode = (uint8_t)(gbc->memory[PYEMU_IO_STAT] & 0x03);
    return mode != 3;
}

int pyemu_gbc_timer_signal(const pyemu_gbc_system* gbc) {
    static const int divisors[] = {1024, 16, 64, 256};
    uint8_t tac = gbc->memory[PYEMU_IO_TAC];
    int index = (int)(tac & 0x03);
    int enabled = (tac & 0x04) != 0;
    return enabled ? 1 : 0;
}

void pyemu_gbc_apply_timer_edge(pyemu_gbc_system* gbc, int old_signal, int new_signal) {
    (void)gbc;
    (void)old_signal;
    (void)new_signal;
}

static uint16_t pyemu_gbc_rgb15_to_rgb888(uint16_t color15) {
    uint8_t r = (uint8_t)(((color15 >> 0) & 0x1F) * 255 / 31);
    uint8_t g = (uint8_t)(((color15 >> 5) & 0x1F) * 255 / 31);
    uint8_t b = (uint8_t)(((color15 >> 10) & 0x1F) * 255 / 31);
    return (uint16_t)((r << 16) | (g << 8) | b);
}

static uint32_t pyemu_gbc_get_bg_color(const pyemu_gbc_system* gbc, uint8_t palette_idx, uint8_t color_id) {
    if (palette_idx >= 8 || color_id >= 4) {
        return 0xFF000000;
    }
    uint8_t color_lsb = gbc->bg_palettes[palette_idx].data[color_id * 2];
    uint8_t color_msb = gbc->bg_palettes[palette_idx].data[color_id * 2 + 1];
    uint16_t rgb15 = (uint16_t)(color_msb << 8) | color_lsb;
    if (rgb15 == 0) {
        return 0x000000FF;
    }
    uint8_t r = (uint8_t)(((rgb15 >> 0) & 0x1F) * 255 / 31);
    uint8_t g = (uint8_t)(((rgb15 >> 5) & 0x1F) * 255 / 31);
    uint8_t b = (uint8_t)(((rgb15 >> 10) & 0x1F) * 255 / 31);
    return (uint32_t)((r << 24) | (g << 16) | (b << 8) | 0xFF);
}

static uint32_t pyemu_gbc_get_sp_color(const pyemu_gbc_system* gbc, uint8_t palette_idx, uint8_t color_id) {
    if (palette_idx >= 8 || color_id >= 4) {
        return 0xFF000000;
    }
    uint8_t color_lsb = gbc->sp_palettes[palette_idx].data[color_id * 2];
    uint8_t color_msb = gbc->sp_palettes[palette_idx].data[color_id * 2 + 1];
    uint16_t rgb15 = (uint16_t)(color_msb << 8) | color_lsb;
    if (rgb15 == 0) {
        return 0x000000FF;
    }
    uint8_t r = (uint8_t)(((rgb15 >> 0) & 0x1F) * 255 / 31);
    uint8_t g = (uint8_t)(((rgb15 >> 5) & 0x1F) * 255 / 31);
    uint8_t b = (uint8_t)(((rgb15 >> 10) & 0x1F) * 255 / 31);
    return (uint32_t)((r << 24) | (g << 16) | (b << 8) | 0xFF);
}

static uint8_t pyemu_gbc_tile_pixel(const pyemu_gbc_system* gbc, uint16_t cpu_address, uint8_t bank, uint8_t pixel_x, uint8_t pixel_y, int x_flip, int y_flip) {
    size_t vram_offset;
    uint8_t px = (uint8_t)(pixel_x & 0x07);
    uint8_t py = (uint8_t)(pixel_y & 0x07);
    uint8_t bit;

    if (x_flip) {
        px = (uint8_t)(7 - px);
    }
    if (y_flip) {
        py = (uint8_t)(7 - py);
    }

    bit = (uint8_t)(7 - px);
    vram_offset = (bank ? 0x2000U : 0U) + (cpu_address - 0x8000) + (py * 2U);

    if (vram_offset + 1 >= PYEMU_GBC_VRAM_SIZE) {
        return 0;
    }

    {
        uint8_t low = gbc->vram[vram_offset];
        uint8_t high = gbc->vram[vram_offset + 1];
        return (uint8_t)((((high >> bit) & 0x01) << 1) | ((low >> bit) & 0x01));
    }
}

static void pyemu_gbc_render_scanline(pyemu_gbc_system* gbc, uint8_t y) {
    int x;
    uint8_t lcdc = gbc->memory[PYEMU_IO_LCDC];
    uint8_t scy = gbc->memory[0xFF42];
    uint8_t scx = gbc->memory[0xFF43];
    uint8_t wy = gbc->memory[PYEMU_IO_WY];
    uint8_t wx = gbc->memory[PYEMU_IO_WX];

    for (x = 0; x < 160; ++x) {
        size_t pixel = (size_t)(y * 160 + x) * 4U;
        uint32_t color = 0xFF00FF00;
        uint8_t bg_color_id = 0;
        uint8_t bg_palette = 0;

        if (gbc->rom_loaded) {
            uint8_t map_x = (uint8_t)(x + scx);
            uint8_t map_y = (uint8_t)(y + scy);
            uint16_t bg_map_base = (lcdc & 0x08) ? 0x9C00 : 0x9800;
            uint16_t tile_map_address = (uint16_t)(bg_map_base + ((map_y / 8) * 32) + (map_x / 8));
            uint8_t tile_index = gbc->memory[tile_map_address];
            
            uint8_t tile_attr = gbc->vram[tile_map_address - 0x8000 + 0x2000];
            uint8_t tile_bank = (uint8_t)((tile_attr & 0x08) ? 1 : 0);
            bg_palette = (uint8_t)(tile_attr & 0x07);

            uint16_t tile_address;
            if (lcdc & 0x10) {
                tile_address = (uint16_t)(0x8000 + (tile_index * 16));
            } else {
                int8_t signed_index = (int8_t)tile_index;
                tile_address = (uint16_t)(0x9000 + (signed_index * 16));
            }

            bg_color_id = pyemu_gbc_tile_pixel(
                gbc,
                tile_address,
                tile_bank,
                map_x,
                map_y,
                (tile_attr & 0x20) != 0,
                (tile_attr & 0x40) != 0);
            color = pyemu_gbc_get_bg_color(gbc, bg_palette, bg_color_id);

            if ((lcdc & 0x20) && y >= wy && (int)x >= ((int)wx - 7)) {
                uint8_t win_x = (uint8_t)(x - ((int)wx - 7));
                uint8_t win_y = (uint8_t)(y - wy);
                uint16_t win_map_base = (lcdc & 0x40) ? 0x9C00 : 0x9800;
                uint16_t win_tile_map_address = (uint16_t)(win_map_base + ((win_y / 8) * 32) + (win_x / 8));
                uint8_t win_tile_index = gbc->memory[win_tile_map_address];
                
                uint8_t win_tile_attr = gbc->vram[win_tile_map_address - 0x8000 + 0x2000];
                uint8_t win_tile_bank = (uint8_t)((win_tile_attr & 0x08) ? 1 : 0);
                bg_palette = (uint8_t)(win_tile_attr & 0x07);

                uint16_t win_tile_address;
                if (lcdc & 0x10) {
                    win_tile_address = (uint16_t)(0x8000 + (win_tile_index * 16));
                } else {
                    int8_t signed_index = (int8_t)win_tile_index;
                    win_tile_address = (uint16_t)(0x9000 + (signed_index * 16));
                }

                bg_color_id = pyemu_gbc_tile_pixel(
                    gbc,
                    win_tile_address,
                    win_tile_bank,
                    win_x,
                    win_y,
                    (win_tile_attr & 0x20) != 0,
                    (win_tile_attr & 0x40) != 0);
                color = pyemu_gbc_get_bg_color(gbc, bg_palette, bg_color_id);
            }
        } else {
            color = (uint32_t)(((x + y + gbc->cpu.a + gbc->memory[PYEMU_IO_LY]) % 255) * 0x01010101) | 0xFF000000;
        }

        if (lcdc & 0x02) {
            int sprite_height = (lcdc & 0x04) ? 16 : 8;
            int sprite_index;
            for (sprite_index = 0; sprite_index < 40; ++sprite_index) {
                uint16_t oam = (uint16_t)(0xFE00 + sprite_index * 4);
                int sprite_y = (int)gbc->memory[oam] - 16;
                int sprite_x = (int)gbc->memory[(uint16_t)(oam + 1)] - 8;
                uint8_t tile = gbc->memory[(uint16_t)(oam + 2)];
                uint8_t sp_attr = gbc->memory[(uint16_t)(oam + 3)];
                uint8_t sp_palette = (uint8_t)(sp_attr & 0x07);
                uint8_t sx, sy;
                uint8_t sp_color_id;

                if (x < sprite_x || x >= sprite_x + 8 || y < sprite_y || y >= sprite_y + sprite_height) {
                    continue;
                }

                sx = (uint8_t)(x - sprite_x);
                sy = (uint8_t)(y - sprite_y);
                if (sp_attr & 0x20) {
                    sx = (uint8_t)(7 - sx);
                }
                if (sp_attr & 0x40) {
                    sy = (uint8_t)(sprite_height - 1 - sy);
                }

                if (sprite_height == 16) {
                    tile &= 0xFE;
                    if (sy >= 8) {
                        tile = (uint8_t)(tile + 1);
                        sy = (uint8_t)(sy - 8);
                    }
                }

                uint16_t sp_tile_address = (uint16_t)(0x8000 + (tile * 16) + (sy * 2));
                sp_color_id = pyemu_gbc_tile_pixel(gbc, sp_tile_address, (uint8_t)((sp_attr & 0x08) ? 1 : 0), sx, sy, 0, 0);
                if (sp_color_id == 0) {
                    continue;
                }
                if ((sp_attr & 0x80) && bg_color_id != 0) {
                    continue;
                }

                color = pyemu_gbc_get_sp_color(gbc, sp_palette, sp_color_id);
                break;
            }
        }

        gbc->frame[pixel + 0] = (uint8_t)((color >> 24) & 0xFF);
        gbc->frame[pixel + 1] = (uint8_t)((color >> 16) & 0xFF);
        gbc->frame[pixel + 2] = (uint8_t)((color >> 8) & 0xFF);
        gbc->frame[pixel + 3] = (uint8_t)(color & 0xFF);
    }
}

void pyemu_gbc_update_demo_frame(pyemu_gbc_system* gbc) {
    int y;
    for (y = 0; y < 144; ++y) {
        pyemu_gbc_render_scanline(gbc, (uint8_t)y);
    }
}

void pyemu_gbc_tick(pyemu_gbc_system* gbc, int cycles) {
    int ticks = gbc->double_speed ? (cycles * 2) : cycles;
    gbc->cycle_count += (uint64_t)ticks;
    gbc->div_counter += (uint32_t)ticks;
    gbc->ppu_counter += (uint32_t)ticks;
    gbc->timer_counter += (uint32_t)ticks;
    if (gbc->timer_counter >= 256) {
        gbc->timer_counter -= 256;
        uint8_t tima = gbc->memory[PYEMU_IO_TIMA];
        if (tima == 0xFF) {
            gbc->memory[PYEMU_IO_TIMA] = gbc->memory[PYEMU_IO_TMA];
            pyemu_gbc_request_interrupt(gbc, PYEMU_INTERRUPT_TIMER);
        } else {
            gbc->memory[PYEMU_IO_TIMA] = (uint8_t)(tima + 1);
        }
    }
    if (gbc->ppu_counter >= PYEMU_GBC_CYCLES_PER_SCANLINE) {
        gbc->ppu_counter -= PYEMU_GBC_CYCLES_PER_SCANLINE;
        uint8_t ly = gbc->memory[PYEMU_IO_LY];
        if (ly < 153) {
            gbc->memory[PYEMU_IO_LY] = (uint8_t)(ly + 1);
        } else {
            gbc->memory[PYEMU_IO_LY] = 0;
        }
        pyemu_gbc_update_stat(gbc);
        if (ly == 143) {
            pyemu_gbc_request_interrupt(gbc, PYEMU_INTERRUPT_VBLANK);
        }
    }
}

uint32_t pyemu_gbc_cycles_until_vblank(const pyemu_gbc_system* gbc) {
    uint8_t ly = gbc->memory[PYEMU_IO_LY];
    if (ly >= 144) {
        return 0;
    }
    uint32_t cycles_in_line = gbc->ppu_counter;
    uint32_t cycles_until_vblank = (144 - ly) * PYEMU_GBC_CYCLES_PER_SCANLINE - cycles_in_line;
    return cycles_until_vblank;
}

void pyemu_gbc_fast_forward_to_vblank(pyemu_gbc_system* gbc) {
    uint32_t target = pyemu_gbc_cycles_until_vblank(gbc);
    if (target > 0) {
        pyemu_gbc_tick(gbc, (int)target);
    }
}

uint16_t pyemu_gbc_get_af(const pyemu_gbc_system* gbc) {
    return (uint16_t)((gbc->cpu.a << 8) | gbc->cpu.f);
}

uint16_t pyemu_gbc_get_bc(const pyemu_gbc_system* gbc) {
    return (uint16_t)((gbc->cpu.b << 8) | gbc->cpu.c);
}

uint16_t pyemu_gbc_get_de(const pyemu_gbc_system* gbc) {
    return (uint16_t)((gbc->cpu.d << 8) | gbc->cpu.e);
}

uint16_t pyemu_gbc_get_hl(const pyemu_gbc_system* gbc) {
    return (uint16_t)((gbc->cpu.h << 8) | gbc->cpu.l);
}

void pyemu_gbc_set_af(pyemu_gbc_system* gbc, uint16_t value) {
    gbc->cpu.a = (uint8_t)(value >> 8);
    gbc->cpu.f = (uint8_t)(value & 0xF0);
}

void pyemu_gbc_set_bc(pyemu_gbc_system* gbc, uint16_t value) {
    gbc->cpu.b = (uint8_t)(value >> 8);
    gbc->cpu.c = (uint8_t)(value & 0xFF);
}

void pyemu_gbc_set_de(pyemu_gbc_system* gbc, uint16_t value) {
    gbc->cpu.d = (uint8_t)(value >> 8);
    gbc->cpu.e = (uint8_t)(value & 0xFF);
}

void pyemu_gbc_set_hl(pyemu_gbc_system* gbc, uint16_t value) {
    gbc->cpu.h = (uint8_t)(value >> 8);
    gbc->cpu.l = (uint8_t)(value & 0xFF);
}

void pyemu_gbc_set_flag(pyemu_gbc_system* gbc, uint8_t mask, int enabled) {
    if (enabled) {
        gbc->cpu.f = (uint8_t)(gbc->cpu.f | mask);
    } else {
        gbc->cpu.f = (uint8_t)(gbc->cpu.f & ~mask);
    }
}

int pyemu_gbc_get_flag(const pyemu_gbc_system* gbc, uint8_t mask) {
    return (gbc->cpu.f & mask) != 0 ? 1 : 0;
}

void pyemu_gbc_set_flags_znhc(pyemu_gbc_system* gbc, int z, int n, int h, int c) {
    gbc->cpu.f = 0;
    if (z) gbc->cpu.f |= PYEMU_FLAG_Z;
    if (n) gbc->cpu.f |= PYEMU_FLAG_N;
    if (h) gbc->cpu.f |= PYEMU_FLAG_H;
    if (c) gbc->cpu.f |= PYEMU_FLAG_C;
}

uint8_t pyemu_gbc_inc8(pyemu_gbc_system* gbc, uint8_t value) {
    uint8_t result = (uint8_t)(value + 1);
    pyemu_gbc_set_flags_znhc(gbc, result == 0, 1, (result & 0x0F) == 0, -1);
    return result;
}

uint8_t pyemu_gbc_dec8(pyemu_gbc_system* gbc, uint8_t value) {
    uint8_t result = (uint8_t)(value - 1);
    pyemu_gbc_set_flags_znhc(gbc, result == 0, 1, (result & 0x0F) == 0x0F, -1);
    return result;
}

uint8_t pyemu_gbc_swap8(pyemu_gbc_system* gbc, uint8_t value) {
    uint8_t result = (uint8_t)(((value & 0xF0) >> 4) | ((value & 0x0F) << 4));
    pyemu_gbc_set_flags_znhc(gbc, result == 0, 0, 0, 0);
    return result;
}

void pyemu_gbc_rlca(pyemu_gbc_system* gbc) {
    uint8_t a = gbc->cpu.a;
    int c = (a >> 7) & 1;
    gbc->cpu.a = (uint8_t)((a << 1) | c);
    pyemu_gbc_set_flags_znhc(gbc, gbc->cpu.a == 0, 0, 0, c);
}

void pyemu_gbc_rrca(pyemu_gbc_system* gbc) {
    uint8_t a = gbc->cpu.a;
    int c = a & 1;
    gbc->cpu.a = (uint8_t)((a >> 1) | (c << 7));
    pyemu_gbc_set_flags_znhc(gbc, gbc->cpu.a == 0, 0, 0, c);
}

void pyemu_gbc_rla(pyemu_gbc_system* gbc) {
    uint8_t a = gbc->cpu.a;
    int c = (a >> 7) & 1;
    int prev_c = pyemu_gbc_get_flag(gbc, PYEMU_FLAG_C) ? 1 : 0;
    gbc->cpu.a = (uint8_t)((a << 1) | prev_c);
    pyemu_gbc_set_flags_znhc(gbc, gbc->cpu.a == 0, 0, 0, c);
}

void pyemu_gbc_rra(pyemu_gbc_system* gbc) {
    uint8_t a = gbc->cpu.a;
    int c = a & 1;
    int prev_c = pyemu_gbc_get_flag(gbc, PYEMU_FLAG_C) ? 1 : 0;
    gbc->cpu.a = (uint8_t)((a >> 1) | (prev_c << 7));
    pyemu_gbc_set_flags_znhc(gbc, gbc->cpu.a == 0, 0, 0, c);
}

uint8_t pyemu_gbc_rlc8(pyemu_gbc_system* gbc, uint8_t value) {
    int c = (value >> 7) & 1;
    uint8_t result = (uint8_t)((value << 1) | c);
    pyemu_gbc_set_flags_znhc(gbc, result == 0, 0, 0, c);
    return result;
}

uint8_t pyemu_gbc_rrc8(pyemu_gbc_system* gbc, uint8_t value) {
    int c = value & 1;
    uint8_t result = (uint8_t)((value >> 1) | (c << 7));
    pyemu_gbc_set_flags_znhc(gbc, result == 0, 0, 0, c);
    return result;
}

uint8_t pyemu_gbc_rl8(pyemu_gbc_system* gbc, uint8_t value) {
    int c = (value >> 7) & 1;
    int prev_c = pyemu_gbc_get_flag(gbc, PYEMU_FLAG_C) ? 1 : 0;
    uint8_t result = (uint8_t)((value << 1) | prev_c);
    pyemu_gbc_set_flags_znhc(gbc, result == 0, 0, 0, c);
    return result;
}

uint8_t pyemu_gbc_rr8(pyemu_gbc_system* gbc, uint8_t value) {
    int c = value & 1;
    int prev_c = pyemu_gbc_get_flag(gbc, PYEMU_FLAG_C) ? 1 : 0;
    uint8_t result = (uint8_t)((value >> 1) | (prev_c << 7));
    pyemu_gbc_set_flags_znhc(gbc, result == 0, 0, 0, c);
    return result;
}

uint8_t pyemu_gbc_sla8(pyemu_gbc_system* gbc, uint8_t value) {
    int c = (value >> 7) & 1;
    uint8_t result = (uint8_t)(value << 1);
    pyemu_gbc_set_flags_znhc(gbc, result == 0, 0, 0, c);
    return result;
}

uint8_t pyemu_gbc_sra8(pyemu_gbc_system* gbc, uint8_t value) {
    int c = value & 1;
    uint8_t result = (uint8_t)((value >> 1) | (value & 0x80));
    pyemu_gbc_set_flags_znhc(gbc, result == 0, 0, 0, c);
    return result;
}

uint8_t pyemu_gbc_srl8(pyemu_gbc_system* gbc, uint8_t value) {
    int c = value & 1;
    uint8_t result = (uint8_t)(value >> 1);
    pyemu_gbc_set_flags_znhc(gbc, result == 0, 0, 0, c);
    return result;
}

void pyemu_gbc_add_a(pyemu_gbc_system* gbc, uint8_t value, int with_carry) {
    uint16_t a = gbc->cpu.a;
    uint16_t v = value;
    if (with_carry && pyemu_gbc_get_flag(gbc, PYEMU_FLAG_C)) {
        v++;
    }
    uint16_t result = (uint16_t)(a + v);
    int c = (result >> 8) != 0;
    int h = ((a & 0x0F) + (v & 0x0F)) > 0x0F;
    gbc->cpu.a = (uint8_t)result;
    pyemu_gbc_set_flags_znhc(gbc, gbc->cpu.a == 0, 0, h, c);
}

void pyemu_gbc_and_a(pyemu_gbc_system* gbc, uint8_t value) {
    gbc->cpu.a = (uint8_t)(gbc->cpu.a & value);
    pyemu_gbc_set_flags_znhc(gbc, gbc->cpu.a == 0, 0, 1, 0);
}

void pyemu_gbc_xor_a(pyemu_gbc_system* gbc, uint8_t value) {
    gbc->cpu.a = (uint8_t)(gbc->cpu.a ^ value);
    pyemu_gbc_set_flags_znhc(gbc, gbc->cpu.a == 0, 0, 0, 0);
}

void pyemu_gbc_or_a(pyemu_gbc_system* gbc, uint8_t value) {
    gbc->cpu.a = (uint8_t)(gbc->cpu.a | value);
    pyemu_gbc_set_flags_znhc(gbc, gbc->cpu.a == 0, 0, 0, 0);
}

void pyemu_gbc_cp_a(pyemu_gbc_system* gbc, uint8_t value) {
    uint8_t a = gbc->cpu.a;
    int result = (int)a - (int)value;
    int c = result < 0;
    int h = ((a & 0x0F) - (value & 0x0F)) < 0;
    pyemu_gbc_set_flags_znhc(gbc, (uint8_t)result == 0, 1, h, c);
}

void pyemu_gbc_sub_a(pyemu_gbc_system* gbc, uint8_t value) {
    uint8_t a = gbc->cpu.a;
    int with_carry = 0;
    uint16_t v = value;
    if (with_carry && pyemu_gbc_get_flag(gbc, PYEMU_FLAG_C)) {
        v++;
    }
    int result = (int)a - (int)v;
    int c = result < 0;
    int h = ((a & 0x0F) - (v & 0x0F)) < 0;
    gbc->cpu.a = (uint8_t)result;
    pyemu_gbc_set_flags_znhc(gbc, gbc->cpu.a == 0, 1, h, c);
}

void pyemu_gbc_sbc_a(pyemu_gbc_system* gbc, uint8_t value) {
    uint8_t a = gbc->cpu.a;
    uint16_t v = value;
    if (pyemu_gbc_get_flag(gbc, PYEMU_FLAG_C)) {
        v++;
    }
    int result = (int)a - (int)v;
    int c = result < 0;
    int h = ((a & 0x0F) - (v & 0x0F)) < 0;
    gbc->cpu.a = (uint8_t)result;
    pyemu_gbc_set_flags_znhc(gbc, gbc->cpu.a == 0, 1, h, c);
}

void pyemu_gbc_add_sp_signed(pyemu_gbc_system* gbc, int8_t value) {
    uint16_t sp = gbc->cpu.sp;
    uint32_t result = (uint32_t)((int)sp + (int)value);
    int c = ((sp ^ value ^ result) & 0x100) != 0;
    int h = ((sp ^ value ^ result) & 0x10) != 0;
    gbc->cpu.sp = (uint16_t)result;
    pyemu_gbc_set_flags_znhc(gbc, 0, 0, h, c);
}

uint16_t pyemu_gbc_sp_plus_signed(pyemu_gbc_system* gbc, int8_t value) {
    uint16_t sp = gbc->cpu.sp;
    uint32_t result = (uint32_t)((int)sp + (int)value);
    return (uint16_t)result;
}

void pyemu_gbc_bit_test(pyemu_gbc_system* gbc, uint8_t value, uint8_t bit) {
    int result = (value >> bit) & 1;
    pyemu_gbc_set_flags_znhc(gbc, result == 0, 0, 1, -1);
}

void pyemu_gbc_add_hl(pyemu_gbc_system* gbc, uint16_t value) {
    uint16_t hl = pyemu_gbc_get_hl(gbc);
    uint32_t result = (uint32_t)(hl + value);
    int c = (result >> 16) != 0;
    int h = ((hl & 0x0FFF) + (value & 0x0FFF)) > 0x0FFF;
    pyemu_gbc_set_hl(gbc, (uint16_t)result);
    pyemu_gbc_set_flags_znhc(gbc, -1, 0, h, c);
}

void pyemu_gbc_daa(pyemu_gbc_system* gbc) {
    uint8_t a = gbc->cpu.a;
    int n = pyemu_gbc_get_flag(gbc, PYEMU_FLAG_N) ? 1 : 0;
    int h = pyemu_gbc_get_flag(gbc, PYEMU_FLAG_H) ? 1 : 0;
    int c = pyemu_gbc_get_flag(gbc, PYEMU_FLAG_C) ? 1 : 0;
    if (!n) {
        if (c || a > 0x99) {
            a = (uint8_t)(a + 0x60);
            c = 1;
        }
        if (h || (a & 0x0F) > 0x09) {
            a = (uint8_t)(a + 0x06);
        }
    } else {
        if (c) {
            a = (uint8_t)(a - 0x60);
        }
        if (h) {
            a = (uint8_t)(a - 0x06);
        }
    }
    gbc->cpu.a = a;
    pyemu_gbc_set_flags_znhc(gbc, a == 0, n, 0, c);
}

uint8_t pyemu_gbc_fetch_u8(pyemu_gbc_system* gbc) {
    uint8_t value = pyemu_gbc_read_memory(gbc, gbc->cpu.pc);
    gbc->cpu.pc = (uint16_t)(gbc->cpu.pc + 1);
    return value;
}

uint16_t pyemu_gbc_fetch_u16(pyemu_gbc_system* gbc) {
    uint16_t lo = pyemu_gbc_fetch_u8(gbc);
    uint16_t hi = pyemu_gbc_fetch_u8(gbc);
    return (uint16_t)((hi << 8) | lo);
}

void pyemu_gbc_push_u16(pyemu_gbc_system* gbc, uint16_t value) {
    gbc->cpu.sp = (uint16_t)(gbc->cpu.sp - 2);
    pyemu_gbc_write_memory(gbc, (uint16_t)(gbc->cpu.sp + 1), (uint8_t)(value >> 8));
    pyemu_gbc_write_memory(gbc, gbc->cpu.sp, (uint8_t)(value & 0xFF));
}

uint16_t pyemu_gbc_pop_u16(pyemu_gbc_system* gbc) {
    uint16_t lo = pyemu_gbc_read_memory(gbc, gbc->cpu.sp);
    gbc->cpu.sp = (uint16_t)(gbc->cpu.sp + 1);
    uint16_t hi = pyemu_gbc_read_memory(gbc, gbc->cpu.sp);
    gbc->cpu.sp = (uint16_t)(gbc->cpu.sp + 1);
    return (uint16_t)((hi << 8) | lo);
}

int pyemu_gbc_service_interrupts(pyemu_gbc_system* gbc) {
    uint8_t pending = pyemu_gbc_pending_interrupts(gbc);
    uint8_t mask = 0;
    uint16_t vector = 0;

    if (pending == 0) {
        return 0;
    }

    if (gbc->cpu.halted) {
        gbc->cpu.halted = 0;
    }

    if (!gbc->ime) {
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

    gbc->ime = 0;
    gbc->ime_pending = 0;
    gbc->ime_delay = 0;
    gbc->memory[PYEMU_IO_IF] = (uint8_t)(0xE0 | ((gbc->memory[PYEMU_IO_IF] & (uint8_t)~mask) & 0x1F));
    pyemu_gbc_push_u16(gbc, gbc->cpu.pc);
    gbc->cpu.pc = vector;
    pyemu_gbc_tick(gbc, 20);
    return 20;
}

uint8_t pyemu_gbc_read_r8(pyemu_gbc_system* gbc, int index) {
    switch (index) {
        case 0: return gbc->cpu.b;
        case 1: return gbc->cpu.c;
        case 2: return gbc->cpu.d;
        case 3: return gbc->cpu.e;
        case 4: return gbc->cpu.h;
        case 5: return gbc->cpu.l;
        case 6: return pyemu_gbc_read_memory(gbc, pyemu_gbc_get_hl(gbc));
        case 7: return gbc->cpu.a;
        default: return 0;
    }
}

void pyemu_gbc_write_r8(pyemu_gbc_system* gbc, int index, uint8_t value) {
    switch (index) {
        case 0: gbc->cpu.b = value; break;
        case 1: gbc->cpu.c = value; break;
        case 2: gbc->cpu.d = value; break;
        case 3: gbc->cpu.e = value; break;
        case 4: gbc->cpu.h = value; break;
        case 5: gbc->cpu.l = value; break;
        case 6: pyemu_gbc_write_memory(gbc, pyemu_gbc_get_hl(gbc), value); break;
        case 7: gbc->cpu.a = value; break;
    }
}

int pyemu_gbc_execute_cb(pyemu_gbc_system* gbc) {
    uint8_t cb_opcode = pyemu_gbc_fetch_u8(gbc);
    int reg_index = cb_opcode & 0x07;
    uint8_t value;
    int cycles = 8;

    if (cb_opcode <= 0x07) {
        value = pyemu_gbc_read_r8(gbc, reg_index);
        pyemu_gbc_write_r8(gbc, reg_index, pyemu_gbc_rlc8(gbc, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x0F) {
        value = pyemu_gbc_read_r8(gbc, reg_index);
        pyemu_gbc_write_r8(gbc, reg_index, pyemu_gbc_rrc8(gbc, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x17) {
        value = pyemu_gbc_read_r8(gbc, reg_index);
        pyemu_gbc_write_r8(gbc, reg_index, pyemu_gbc_rl8(gbc, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x1F) {
        value = pyemu_gbc_read_r8(gbc, reg_index);
        pyemu_gbc_write_r8(gbc, reg_index, pyemu_gbc_rr8(gbc, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x27) {
        value = pyemu_gbc_read_r8(gbc, reg_index);
        pyemu_gbc_write_r8(gbc, reg_index, pyemu_gbc_sla8(gbc, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x2F) {
        value = pyemu_gbc_read_r8(gbc, reg_index);
        pyemu_gbc_write_r8(gbc, reg_index, pyemu_gbc_sra8(gbc, value));
        return reg_index == 6 ? 16 : 8;
    }
    if ((cb_opcode & 0xF8) == 0x30) {
        value = pyemu_gbc_read_r8(gbc, reg_index);
        pyemu_gbc_write_r8(gbc, reg_index, pyemu_gbc_swap8(gbc, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x3F) {
        value = pyemu_gbc_read_r8(gbc, reg_index);
        pyemu_gbc_write_r8(gbc, reg_index, pyemu_gbc_srl8(gbc, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode >= 0x40 && cb_opcode <= 0x7F) {
        value = pyemu_gbc_read_r8(gbc, reg_index);
        pyemu_gbc_bit_test(gbc, value, (uint8_t)((cb_opcode - 0x40) / 8));
        return reg_index == 6 ? 12 : 8;
    }
    if (cb_opcode >= 0x80 && cb_opcode <= 0xBF) {
        value = pyemu_gbc_read_r8(gbc, reg_index);
        value = (uint8_t)(value & (uint8_t)~(1U << ((cb_opcode - 0x80) / 8)));
        pyemu_gbc_write_r8(gbc, reg_index, value);
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode >= 0xC0) {
        value = pyemu_gbc_read_r8(gbc, reg_index);
        value = (uint8_t)(value | (uint8_t)(1U << ((cb_opcode - 0xC0) / 8)));
        pyemu_gbc_write_r8(gbc, reg_index, value);
        return reg_index == 6 ? 16 : 8;
    }

    gbc->cpu.halted = 1;
    gbc->faulted = 0;
    gbc->cpu.pc = (uint16_t)(gbc->cpu.pc - 2);
    return cycles;
}

int pyemu_gbc_execute_load_store(pyemu_gbc_system* gb, uint8_t opcode) {
    switch (opcode) {
        case 0x01:
            pyemu_gbc_set_bc(gb, pyemu_gbc_fetch_u16(gb));
            return 12;
        case 0x02:
            pyemu_gbc_write_memory(gb, pyemu_gbc_get_bc(gb), gb->cpu.a);
            return 8;
        case 0x03:
            pyemu_gbc_set_bc(gb, (uint16_t)(pyemu_gbc_get_bc(gb) + 1));
            return 8;
        case 0x04:
            gb->cpu.b = pyemu_gbc_inc8(gb, gb->cpu.b);
            return 4;
        case 0x05:
            gb->cpu.b = pyemu_gbc_dec8(gb, gb->cpu.b);
            return 4;
        case 0x06:
            gb->cpu.b = pyemu_gbc_fetch_u8(gb);
            return 8;
        case 0x07:
            pyemu_gbc_rlca(gb);
            return 4;
        case 0x08: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            pyemu_gbc_write_memory(gb, address, (uint8_t)(gb->cpu.sp & 0xFF));
            pyemu_gbc_write_memory(gb, (uint16_t)(address + 1), (uint8_t)(gb->cpu.sp >> 8));
            return 20;
        }
        case 0x09:
            pyemu_gbc_add_hl(gb, pyemu_gbc_get_bc(gb));
            return 8;
        case 0x0A:
            gb->cpu.a = pyemu_gbc_read_memory(gb, pyemu_gbc_get_bc(gb));
            return 8;
        case 0x0B:
            pyemu_gbc_set_bc(gb, (uint16_t)(pyemu_gbc_get_bc(gb) - 1));
            return 8;
        case 0x0C:
            gb->cpu.c = pyemu_gbc_inc8(gb, gb->cpu.c);
            return 4;
        case 0x0D:
            gb->cpu.c = pyemu_gbc_dec8(gb, gb->cpu.c);
            return 4;
        case 0x0E:
            gb->cpu.c = pyemu_gbc_fetch_u8(gb);
            return 8;
        case 0x0F:
            pyemu_gbc_rrca(gb);
            return 4;
        case 0x11:
            pyemu_gbc_set_de(gb, pyemu_gbc_fetch_u16(gb));
            return 12;
        case 0x12:
            pyemu_gbc_write_memory(gb, pyemu_gbc_get_de(gb), gb->cpu.a);
            return 8;
        case 0x13:
            pyemu_gbc_set_de(gb, (uint16_t)(pyemu_gbc_get_de(gb) + 1));
            return 8;
        case 0x14:
            gb->cpu.d = pyemu_gbc_inc8(gb, gb->cpu.d);
            return 4;
        case 0x15:
            gb->cpu.d = pyemu_gbc_dec8(gb, gb->cpu.d);
            return 4;
        case 0x16:
            gb->cpu.d = pyemu_gbc_fetch_u8(gb);
            return 8;
        case 0x17:
            pyemu_gbc_rla(gb);
            return 4;
        case 0x18: {
            int8_t offset = (int8_t)pyemu_gbc_fetch_u8(gb);
            gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
            return 12;
        }
        case 0x19:
            pyemu_gbc_add_hl(gb, pyemu_gbc_get_de(gb));
            return 8;
        case 0x1A:
            gb->cpu.a = pyemu_gbc_read_memory(gb, pyemu_gbc_get_de(gb));
            return 8;
        case 0x1B:
            pyemu_gbc_set_de(gb, (uint16_t)(pyemu_gbc_get_de(gb) - 1));
            return 8;
        case 0x1C:
            gb->cpu.e = pyemu_gbc_inc8(gb, gb->cpu.e);
            return 4;
        case 0x1D:
            gb->cpu.e = pyemu_gbc_dec8(gb, gb->cpu.e);
            return 4;
        case 0x1E:
            gb->cpu.e = pyemu_gbc_fetch_u8(gb);
            return 8;
        case 0x1F:
            pyemu_gbc_rra(gb);
            return 4;
        case 0x20: {
            int8_t offset = (int8_t)pyemu_gbc_fetch_u8(gb);
            if (!pyemu_gbc_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                return 12;
            }
            return 8;
        }
        case 0x21:
            pyemu_gbc_set_hl(gb, pyemu_gbc_fetch_u16(gb));
            return 12;
        case 0x22: {
            uint16_t hl = pyemu_gbc_get_hl(gb);
            pyemu_gbc_write_memory(gb, hl, gb->cpu.a);
            pyemu_gbc_set_hl(gb, (uint16_t)(hl + 1));
            return 8;
        }
        case 0x23:
            pyemu_gbc_set_hl(gb, (uint16_t)(pyemu_gbc_get_hl(gb) + 1));
            return 8;
        case 0x24:
            gb->cpu.h = pyemu_gbc_inc8(gb, gb->cpu.h);
            return 4;
        case 0x25:
            gb->cpu.h = pyemu_gbc_dec8(gb, gb->cpu.h);
            return 4;
        case 0x26:
            gb->cpu.h = pyemu_gbc_fetch_u8(gb);
            return 8;
        case 0x27:
            pyemu_gbc_daa(gb);
            return 4;
        case 0x28: {
            int8_t offset = (int8_t)pyemu_gbc_fetch_u8(gb);
            if (pyemu_gbc_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                return 12;
            }
            return 8;
        }
        case 0x29:
            pyemu_gbc_add_hl(gb, pyemu_gbc_get_hl(gb));
            return 8;
        case 0x2A: {
            uint16_t hl = pyemu_gbc_get_hl(gb);
            gb->cpu.a = pyemu_gbc_read_memory(gb, hl);
            pyemu_gbc_set_hl(gb, (uint16_t)(hl + 1));
            return 8;
        }
        case 0x2B:
            pyemu_gbc_set_hl(gb, (uint16_t)(pyemu_gbc_get_hl(gb) - 1));
            return 8;
        case 0x2C:
            gb->cpu.l = pyemu_gbc_inc8(gb, gb->cpu.l);
            return 4;
        case 0x2D:
            gb->cpu.l = pyemu_gbc_dec8(gb, gb->cpu.l);
            return 4;
        case 0x2E:
            gb->cpu.l = pyemu_gbc_fetch_u8(gb);
            return 8;
        case 0x2F:
            gb->cpu.a = (uint8_t)~gb->cpu.a;
            pyemu_gbc_set_flag(gb, PYEMU_FLAG_N, 1);
            pyemu_gbc_set_flag(gb, PYEMU_FLAG_H, 1);
            return 4;
        case 0x30: {
            int8_t offset = (int8_t)pyemu_gbc_fetch_u8(gb);
            if (!pyemu_gbc_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                return 12;
            }
            return 8;
        }
        case 0x31:
            gb->cpu.sp = pyemu_gbc_fetch_u16(gb);
            return 12;
        case 0x32: {
            uint16_t hl = pyemu_gbc_get_hl(gb);
            pyemu_gbc_write_memory(gb, hl, gb->cpu.a);
            pyemu_gbc_set_hl(gb, (uint16_t)(hl - 1));
            return 8;
        }
        case 0x33:
            gb->cpu.sp = (uint16_t)(gb->cpu.sp + 1);
            return 8;
        case 0x34: {
            uint16_t hl = pyemu_gbc_get_hl(gb);
            uint8_t value = pyemu_gbc_read_memory(gb, hl);
            pyemu_gbc_write_memory(gb, hl, pyemu_gbc_inc8(gb, value));
            return 12;
        }
        case 0x35: {
            uint16_t hl = pyemu_gbc_get_hl(gb);
            uint8_t value = pyemu_gbc_read_memory(gb, hl);
            pyemu_gbc_write_memory(gb, hl, pyemu_gbc_dec8(gb, value));
            return 12;
        }
        case 0x36: {
            uint16_t hl = pyemu_gbc_get_hl(gb);
            pyemu_gbc_write_memory(gb, hl, pyemu_gbc_fetch_u8(gb));
            return 12;
        }
        case 0x37:
            pyemu_gbc_set_flag(gb, PYEMU_FLAG_N, 0);
            pyemu_gbc_set_flag(gb, PYEMU_FLAG_H, 0);
            pyemu_gbc_set_flag(gb, PYEMU_FLAG_C, 1);
            return 4;
        case 0x38: {
            int8_t offset = (int8_t)pyemu_gbc_fetch_u8(gb);
            if (pyemu_gbc_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                return 12;
            }
            return 8;
        }
        case 0x39:
            pyemu_gbc_add_hl(gb, gb->cpu.sp);
            return 8;
        case 0x3A: {
            uint16_t hl = pyemu_gbc_get_hl(gb);
            gb->cpu.a = pyemu_gbc_read_memory(gb, hl);
            pyemu_gbc_set_hl(gb, (uint16_t)(hl - 1));
            return 8;
        }
        case 0x3B:
            gb->cpu.sp = (uint16_t)(gb->cpu.sp - 1);
            return 8;
        case 0x3C:
            gb->cpu.a = pyemu_gbc_inc8(gb, gb->cpu.a);
            return 4;
        case 0x3D:
            gb->cpu.a = pyemu_gbc_dec8(gb, gb->cpu.a);
            return 4;
        case 0x3F:
            pyemu_gbc_set_flag(gb, PYEMU_FLAG_N, 0);
            pyemu_gbc_set_flag(gb, PYEMU_FLAG_H, 0);
            pyemu_gbc_set_flag(gb, PYEMU_FLAG_C, !pyemu_gbc_get_flag(gb, PYEMU_FLAG_C));
            return 4;
        case 0x3E:
            gb->cpu.a = pyemu_gbc_fetch_u8(gb);
            return 8;
        case 0x46:
            gb->cpu.b = pyemu_gbc_read_memory(gb, pyemu_gbc_get_hl(gb));
            return 8;
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
            return 4;
        case 0x77:
            pyemu_gbc_write_memory(gb, pyemu_gbc_get_hl(gb), gb->cpu.a);
            return 8;
        case 0x7E:
            gb->cpu.a = pyemu_gbc_read_memory(gb, pyemu_gbc_get_hl(gb));
            return 8;
        case 0xE0: {
            uint16_t address = (uint16_t)(0xFF00 | pyemu_gbc_fetch_u8(gb));
            pyemu_gbc_write_memory(gb, address, gb->cpu.a);
            return 12;
        }
        case 0xE2:
            pyemu_gbc_write_memory(gb, (uint16_t)(0xFF00 | gb->cpu.c), gb->cpu.a);
            return 8;
        case 0xEA: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            pyemu_gbc_write_memory(gb, address, gb->cpu.a);
            return 16;
        }
        case 0xF0: {
            uint16_t address = (uint16_t)(0xFF00 | pyemu_gbc_fetch_u8(gb));
            gb->cpu.a = pyemu_gbc_read_memory(gb, address);
            return 12;
        }
        case 0xF2:
            gb->cpu.a = pyemu_gbc_read_memory(gb, (uint16_t)(0xFF00 | gb->cpu.c));
            return 8;
        case 0xFA: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            gb->cpu.a = pyemu_gbc_read_memory(gb, address);
            return 16;
        }
        default:
            return -1;
    }
}

int pyemu_gbc_execute_control_flow(pyemu_gbc_system* gb, uint8_t opcode) {
    switch (opcode) {
        case 0xC0:
            if (!pyemu_gbc_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = pyemu_gbc_pop_u16(gb);
                return 20;
            }
            return 8;
        case 0xC1:
            pyemu_gbc_set_bc(gb, pyemu_gbc_pop_u16(gb));
            return 12;
        case 0xC2: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            if (!pyemu_gbc_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = address;
                return 16;
            }
            return 12;
        }
        case 0xC3:
            gb->cpu.pc = pyemu_gbc_fetch_u16(gb);
            return 16;
        case 0xC4: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            if (!pyemu_gbc_get_flag(gb, PYEMU_FLAG_Z)) {
                pyemu_gbc_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                return 24;
            }
            return 12;
        }
        case 0xC5:
            pyemu_gbc_push_u16(gb, pyemu_gbc_get_bc(gb));
            return 16;
        case 0xC7:
        case 0xCF:
        case 0xD7:
        case 0xDF:
        case 0xE7:
        case 0xEF:
        case 0xF7:
        case 0xFF:
            pyemu_gbc_push_u16(gb, gb->cpu.pc);
            gb->cpu.pc = (uint16_t)(opcode & 0x38);
            return 16;
        case 0xC8:
            if (pyemu_gbc_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = pyemu_gbc_pop_u16(gb);
                return 20;
            }
            return 8;
        case 0xC9:
            gb->cpu.pc = pyemu_gbc_pop_u16(gb);
            return 16;
        case 0xCA: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            if (pyemu_gbc_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = address;
                return 16;
            }
            return 12;
        }
        case 0xCC: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            if (pyemu_gbc_get_flag(gb, PYEMU_FLAG_Z)) {
                pyemu_gbc_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                return 24;
            }
            return 12;
        }
        case 0xCD: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            pyemu_gbc_push_u16(gb, gb->cpu.pc);
            gb->cpu.pc = address;
            return 24;
        }
        case 0xD0:
            if (!pyemu_gbc_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = pyemu_gbc_pop_u16(gb);
                return 20;
            }
            return 8;
        case 0xD1:
            pyemu_gbc_set_de(gb, pyemu_gbc_pop_u16(gb));
            return 12;
        case 0xD2: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            if (!pyemu_gbc_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = address;
                return 16;
            }
            return 12;
        }
        case 0xD4: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            if (!pyemu_gbc_get_flag(gb, PYEMU_FLAG_C)) {
                pyemu_gbc_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                return 24;
            }
            return 12;
        }
        case 0xD5:
            pyemu_gbc_push_u16(gb, pyemu_gbc_get_de(gb));
            return 16;
        case 0xD8:
            if (pyemu_gbc_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = pyemu_gbc_pop_u16(gb);
                return 20;
            }
            return 8;
        case 0xD9:
            gb->cpu.pc = pyemu_gbc_pop_u16(gb);
            gb->ime = 1;
            gb->ime_pending = 0;
            gb->ime_delay = 0;
            return 16;
        case 0xDA: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            if (pyemu_gbc_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = address;
                return 16;
            }
            return 12;
        }
        case 0xDC: {
            uint16_t address = pyemu_gbc_fetch_u16(gb);
            if (pyemu_gbc_get_flag(gb, PYEMU_FLAG_C)) {
                pyemu_gbc_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                return 24;
            }
            return 12;
        }
        case 0xE1:
            pyemu_gbc_set_hl(gb, pyemu_gbc_pop_u16(gb));
            return 12;
        case 0xE5:
            pyemu_gbc_push_u16(gb, pyemu_gbc_get_hl(gb));
            return 16;
        case 0xE9:
            gb->cpu.pc = pyemu_gbc_get_hl(gb);
            return 4;
        case 0xF1:
            pyemu_gbc_set_af(gb, pyemu_gbc_pop_u16(gb));
            return 12;
        case 0xF5:
            pyemu_gbc_push_u16(gb, pyemu_gbc_get_af(gb));
            return 16;
        case 0xF9:
            gb->cpu.sp = pyemu_gbc_get_hl(gb);
            return 8;
        default:
            return -1;
    }
}

int pyemu_gbc_execute_alu(pyemu_gbc_system* gb, uint8_t opcode) {
    switch (opcode) {
        case 0x80: pyemu_gbc_add_a(gb, gb->cpu.b, 0); return 4;
        case 0x81: pyemu_gbc_add_a(gb, gb->cpu.c, 0); return 4;
        case 0x82: pyemu_gbc_add_a(gb, gb->cpu.d, 0); return 4;
        case 0x83: pyemu_gbc_add_a(gb, gb->cpu.e, 0); return 4;
        case 0x84: pyemu_gbc_add_a(gb, gb->cpu.h, 0); return 4;
        case 0x85: pyemu_gbc_add_a(gb, gb->cpu.l, 0); return 4;
        case 0x86: pyemu_gbc_add_a(gb, pyemu_gbc_read_memory(gb, pyemu_gbc_get_hl(gb)), 0); return 8;
        case 0x87: pyemu_gbc_add_a(gb, gb->cpu.a, 0); return 4;
        case 0x88: pyemu_gbc_add_a(gb, gb->cpu.b, 1); return 4;
        case 0x89: pyemu_gbc_add_a(gb, gb->cpu.c, 1); return 4;
        case 0x8A: pyemu_gbc_add_a(gb, gb->cpu.d, 1); return 4;
        case 0x8B: pyemu_gbc_add_a(gb, gb->cpu.e, 1); return 4;
        case 0x8C: pyemu_gbc_add_a(gb, gb->cpu.h, 1); return 4;
        case 0x8D: pyemu_gbc_add_a(gb, gb->cpu.l, 1); return 4;
        case 0x8E: pyemu_gbc_add_a(gb, pyemu_gbc_read_memory(gb, pyemu_gbc_get_hl(gb)), 1); return 8;
        case 0x8F: pyemu_gbc_add_a(gb, gb->cpu.a, 1); return 4;
        case 0x90: pyemu_gbc_sub_a(gb, gb->cpu.b); return 4;
        case 0x91: pyemu_gbc_sub_a(gb, gb->cpu.c); return 4;
        case 0x92: pyemu_gbc_sub_a(gb, gb->cpu.d); return 4;
        case 0x93: pyemu_gbc_sub_a(gb, gb->cpu.e); return 4;
        case 0x94: pyemu_gbc_sub_a(gb, gb->cpu.h); return 4;
        case 0x95: pyemu_gbc_sub_a(gb, gb->cpu.l); return 4;
        case 0x96: pyemu_gbc_sub_a(gb, pyemu_gbc_read_memory(gb, pyemu_gbc_get_hl(gb))); return 8;
        case 0x97: pyemu_gbc_sub_a(gb, gb->cpu.a); return 4;
        case 0x98: pyemu_gbc_sbc_a(gb, gb->cpu.b); return 4;
        case 0x99: pyemu_gbc_sbc_a(gb, gb->cpu.c); return 4;
        case 0x9A: pyemu_gbc_sbc_a(gb, gb->cpu.d); return 4;
        case 0x9B: pyemu_gbc_sbc_a(gb, gb->cpu.e); return 4;
        case 0x9C: pyemu_gbc_sbc_a(gb, gb->cpu.h); return 4;
        case 0x9D: pyemu_gbc_sbc_a(gb, gb->cpu.l); return 4;
        case 0x9E: pyemu_gbc_sbc_a(gb, pyemu_gbc_read_memory(gb, pyemu_gbc_get_hl(gb))); return 8;
        case 0x9F: pyemu_gbc_sbc_a(gb, gb->cpu.a); return 4;
        case 0xA0: pyemu_gbc_and_a(gb, gb->cpu.b); return 4;
        case 0xA1: pyemu_gbc_and_a(gb, gb->cpu.c); return 4;
        case 0xA2: pyemu_gbc_and_a(gb, gb->cpu.d); return 4;
        case 0xA3: pyemu_gbc_and_a(gb, gb->cpu.e); return 4;
        case 0xA4: pyemu_gbc_and_a(gb, gb->cpu.h); return 4;
        case 0xA5: pyemu_gbc_and_a(gb, gb->cpu.l); return 4;
        case 0xA6: pyemu_gbc_and_a(gb, pyemu_gbc_read_memory(gb, pyemu_gbc_get_hl(gb))); return 8;
        case 0xA7: pyemu_gbc_and_a(gb, gb->cpu.a); return 4;
        case 0xA8: pyemu_gbc_xor_a(gb, gb->cpu.b); return 4;
        case 0xA9: pyemu_gbc_xor_a(gb, gb->cpu.c); return 4;
        case 0xAA: pyemu_gbc_xor_a(gb, gb->cpu.d); return 4;
        case 0xAB: pyemu_gbc_xor_a(gb, gb->cpu.e); return 4;
        case 0xAC: pyemu_gbc_xor_a(gb, gb->cpu.h); return 4;
        case 0xAD: pyemu_gbc_xor_a(gb, gb->cpu.l); return 4;
        case 0xAE: pyemu_gbc_xor_a(gb, pyemu_gbc_read_memory(gb, pyemu_gbc_get_hl(gb))); return 8;
        case 0xAF: pyemu_gbc_xor_a(gb, gb->cpu.a); return 4;
        case 0xB0: pyemu_gbc_or_a(gb, gb->cpu.b); return 4;
        case 0xB1: pyemu_gbc_or_a(gb, gb->cpu.c); return 4;
        case 0xB2: pyemu_gbc_or_a(gb, gb->cpu.d); return 4;
        case 0xB3: pyemu_gbc_or_a(gb, gb->cpu.e); return 4;
        case 0xB4: pyemu_gbc_or_a(gb, gb->cpu.h); return 4;
        case 0xB5: pyemu_gbc_or_a(gb, gb->cpu.l); return 4;
        case 0xB6: pyemu_gbc_or_a(gb, pyemu_gbc_read_memory(gb, pyemu_gbc_get_hl(gb))); return 8;
        case 0xB7: pyemu_gbc_or_a(gb, gb->cpu.a); return 4;
        case 0xB8: pyemu_gbc_cp_a(gb, gb->cpu.b); return 4;
        case 0xB9: pyemu_gbc_cp_a(gb, gb->cpu.c); return 4;
        case 0xBA: pyemu_gbc_cp_a(gb, gb->cpu.d); return 4;
        case 0xBB: pyemu_gbc_cp_a(gb, gb->cpu.e); return 4;
        case 0xBC: pyemu_gbc_cp_a(gb, gb->cpu.h); return 4;
        case 0xBD: pyemu_gbc_cp_a(gb, gb->cpu.l); return 4;
        case 0xBE: pyemu_gbc_cp_a(gb, pyemu_gbc_read_memory(gb, pyemu_gbc_get_hl(gb))); return 8;
        case 0xBF: pyemu_gbc_cp_a(gb, gb->cpu.a); return 4;
        case 0xC6: pyemu_gbc_add_a(gb, pyemu_gbc_fetch_u8(gb), 0); return 8;
        case 0xCE: pyemu_gbc_add_a(gb, pyemu_gbc_fetch_u8(gb), 1); return 8;
        case 0xD6: pyemu_gbc_sub_a(gb, pyemu_gbc_fetch_u8(gb)); return 8;
        case 0xDE: pyemu_gbc_sbc_a(gb, pyemu_gbc_fetch_u8(gb)); return 8;
        case 0xE6: pyemu_gbc_and_a(gb, pyemu_gbc_fetch_u8(gb)); return 8;
        case 0xEE: pyemu_gbc_xor_a(gb, pyemu_gbc_fetch_u8(gb)); return 8;
        case 0xF6: pyemu_gbc_or_a(gb, pyemu_gbc_fetch_u8(gb)); return 8;
        case 0xFE: pyemu_gbc_cp_a(gb, pyemu_gbc_fetch_u8(gb)); return 8;
        default:
            return -1;
    }
}

int pyemu_gbc_save_battery_ram(const pyemu_gbc_system* gbc) {
    char save_path[280];
    size_t i;
    FILE* save_file;
    if (!gbc->rom_loaded || !pyemu_gbc_has_battery(gbc) || gbc->eram_size == 0) {
        return 0;
    }
    strncpy(save_path, gbc->loaded_rom, sizeof(save_path) - 1);
    save_path[sizeof(save_path) - 1] = '\0';
    for (i = 0; i < sizeof(save_path); ++i) {
        if (save_path[i] == '.') {
            save_path[i] = '\0';
            break;
        }
    }
    strncat(save_path, ".sav", sizeof(save_path) - strlen(save_path) - 1);
    save_file = fopen(save_path, "wb");
    if (save_file == NULL) {
        return 0;
    }
    if (fwrite(gbc->eram, 1, gbc->eram_size, save_file) != gbc->eram_size) {
        fclose(save_file);
        return 0;
    }
    fclose(save_file);
    return 1;
}

void pyemu_gbc_load_battery_ram(pyemu_gbc_system* gbc) {
    char save_path[280];
    size_t i;
    FILE* save_file;
    if (!gbc->rom_loaded || !pyemu_gbc_has_battery(gbc) || gbc->eram_size == 0) {
        return;
    }
    strncpy(save_path, gbc->loaded_rom, sizeof(save_path) - 1);
    save_path[sizeof(save_path) - 1] = '\0';
    for (i = 0; i < sizeof(save_path); ++i) {
        if (save_path[i] == '.') {
            save_path[i] = '\0';
            break;
        }
    }
    strncat(save_path, ".sav", sizeof(save_path) - strlen(save_path) - 1);
    save_file = fopen(save_path, "rb");
    if (save_file == NULL) {
        return;
    }
    if (fread(gbc->eram, 1, gbc->eram_size, save_file) != gbc->eram_size) {
        memset(gbc->eram, 0, gbc->eram_size);
    }
    fclose(save_file);
}
