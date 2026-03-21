#include "gameboy_internal.h"

/* LCD, timer, and framebuffer helpers for the DMG core. This file owns mode timing, scanline latching, and the CPU-visible side effects of the PPU. */

#include <string.h>

/* Collapse the STAT enable bits, coincidence flag, and current mode into the single interrupt line used for edge detection. */
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

/* Return whether the LCD controller is currently enabled. */
int pyemu_gameboy_lcd_enabled(const pyemu_gameboy_system* gb) {
    return (gb->memory[PYEMU_IO_LCDC] & 0x80) != 0;
}

/* Return IF & IE, masked down to the architecturally visible interrupt bits. */
uint8_t pyemu_gameboy_pending_interrupts(const pyemu_gameboy_system* gb) {
    uint8_t pending = (uint8_t)(gb->memory[PYEMU_IO_IF] & gb->memory[PYEMU_IO_IE] & 0x1F);
    if (!pyemu_gameboy_lcd_enabled(gb)) {
        pending = (uint8_t)(pending & (uint8_t)~(PYEMU_INTERRUPT_VBLANK | PYEMU_INTERRUPT_LCD));
    }
    return pending;
}

/* Recompute STAT mode/coincidence bits and generate a new LCD interrupt only on a rising edge. */
void pyemu_gameboy_update_stat(pyemu_gameboy_system* gb) {
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

/* Snapshot scroll/window/palette registers at the start of a visible line so later HBlank writes do not retroactively change it. */
void pyemu_gameboy_latch_scanline_registers(pyemu_gameboy_system* gb, uint8_t ly) {
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

/* Return whether CPU reads and writes to VRAM are currently allowed for the active LCD mode. */
int pyemu_gameboy_cpu_can_access_vram(const pyemu_gameboy_system* gb) {
    uint8_t ly;

    if (!pyemu_gameboy_lcd_enabled(gb)) {
        return 1;
    }

    ly = gb->memory[PYEMU_IO_LY];
    if (ly >= PYEMU_GAMEBOY_HEIGHT) {
        return 1;
    }

    return gb->ppu_counter < 80U || gb->ppu_counter >= 252U;
}

/* Return whether CPU reads and writes to OAM are currently allowed for the active LCD mode. */
int pyemu_gameboy_cpu_can_access_oam(const pyemu_gameboy_system* gb) {
    uint8_t ly;

    if (!pyemu_gameboy_lcd_enabled(gb)) {
        return 1;
    }

    ly = gb->memory[PYEMU_IO_LY];
    if (ly >= PYEMU_GAMEBOY_HEIGHT) {
        return 1;
    }

    return gb->ppu_counter >= 252U;
}

/* Map a two-bit DMG color id through one of the hardware palette registers. */
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

    low = gb->memory[tile_address];
    high = gb->memory[(uint16_t)(tile_address + 1)];
    return (uint8_t)((((high >> bit) & 0x01) << 1) | ((low >> bit) & 0x01));
}

/* Translate TAC frequency selection into the divider bit watched by the falling-edge TIMA circuit. */
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

/* Return the internal timer signal level for a given DIV/TAC state. */
static int pyemu_gameboy_timer_signal_from_state(uint32_t div_counter, uint8_t tac) {
    uint16_t mask;
    if ((tac & 0x04) == 0) {
        return 0;
    }
    mask = pyemu_gameboy_timer_bit_mask_from_tac(tac);
    return (div_counter & mask) != 0;
}

/* Return the current timer signal level for the live machine state. */
int pyemu_gameboy_timer_signal(const pyemu_gameboy_system* gb) {
    return pyemu_gameboy_timer_signal_from_state(gb->div_counter, gb->memory[PYEMU_IO_TAC]);
}

/* Advance TIMA once, handling overflow reload and timer interrupt generation. */
static void pyemu_gameboy_increment_tima(pyemu_gameboy_system* gb) {
    uint8_t tima = gb->memory[PYEMU_IO_TIMA];
    if (tima == 0xFF) {
        gb->memory[PYEMU_IO_TIMA] = gb->memory[PYEMU_IO_TMA];
        pyemu_gameboy_request_interrupt(gb, PYEMU_INTERRUPT_TIMER);
    } else {
        gb->memory[PYEMU_IO_TIMA] = (uint8_t)(tima + 1);
    }
}

/* Apply the DMG timer's falling-edge behavior after a DIV or TAC transition. */
void pyemu_gameboy_apply_timer_edge(pyemu_gameboy_system* gb, int old_signal, int new_signal) {
    if (old_signal && !new_signal) {
        pyemu_gameboy_increment_tima(gb);
    }
}

/* Render one visible scanline into the RGBA framebuffer using the register values latched for that line. */
static void pyemu_gameboy_render_scanline(pyemu_gameboy_system* gb, uint8_t y) {
    int x;
    int sprite_index;
    uint8_t lcdc;
    uint8_t scy;
    uint8_t scx;
    uint8_t wy;
    uint8_t wx;
    uint8_t bgp;
    uint8_t obp0;
    uint8_t obp1;

    if (y >= PYEMU_GAMEBOY_HEIGHT) {
        return;
    }

    lcdc = gb->line_lcdc[y];
    scy = gb->line_scy[y];
    scx = gb->line_scx[y];
    wy = gb->line_wy[y];
    wx = gb->line_wx[y];
    bgp = gb->line_bgp[y];
    obp0 = gb->line_obp0[y];
    obp1 = gb->line_obp1[y];

    for (x = 0; x < PYEMU_GAMEBOY_WIDTH; ++x) {
        size_t pixel = (size_t)(y * PYEMU_GAMEBOY_WIDTH + x) * 4U;
        uint8_t shade = 0xCC;
        uint8_t bg_color_id = 0;

        if (gb->rom_loaded && (lcdc & 0x01)) {
            uint8_t map_x = (uint8_t)(x + scx);
            uint8_t map_y = (uint8_t)(y + scy);
            uint16_t bg_map_base = (lcdc & 0x08) ? 0x9C00 : 0x9800;
            uint16_t tile_map_address = (uint16_t)(bg_map_base + ((map_y / 8) * 32) + (map_x / 8));
            uint8_t tile_index = gb->memory[tile_map_address];

            bg_color_id = pyemu_gameboy_tile_pixel(gb, lcdc, tile_index, map_x, map_y, 0);
            shade = pyemu_gameboy_apply_palette(bgp, bg_color_id);

            if ((lcdc & 0x20) && y >= wy && (int)x >= ((int)wx - 7)) {
                uint8_t win_x = (uint8_t)(x - ((int)wx - 7));
                uint8_t win_y = (uint8_t)(y - wy);
                uint16_t win_map_base = (lcdc & 0x40) ? 0x9C00 : 0x9800;
                uint16_t win_tile_map_address = (uint16_t)(win_map_base + ((win_y / 8) * 32) + (win_x / 8));
                uint8_t win_tile_index = gb->memory[win_tile_map_address];
                bg_color_id = pyemu_gameboy_tile_pixel(gb, lcdc, win_tile_index, win_x, win_y, 0);
                shade = pyemu_gameboy_apply_palette(bgp, bg_color_id);
            }
        } else if (gb->rom_loaded) {
            shade = 255;
        } else {
            shade = (uint8_t)((x + y + gb->cpu.a + gb->memory[PYEMU_IO_LY]) % 255);
        }

        if (lcdc & 0x02) {
            int sprite_height = (lcdc & 0x04) ? 16 : 8;
            for (sprite_index = 0; sprite_index < 40; ++sprite_index) {
                uint16_t oam = (uint16_t)(0xFE00 + sprite_index * 4);
                int sprite_y = (int)gb->memory[oam] - 16;
                int sprite_x = (int)gb->memory[(uint16_t)(oam + 1)] - 8;
                uint8_t tile = gb->memory[(uint16_t)(oam + 2)];
                uint8_t attrs = gb->memory[(uint16_t)(oam + 3)];
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

/* Generate a placeholder frame when no ROM is loaded so the frontend has something stable to display. */
void pyemu_gameboy_update_demo_frame(pyemu_gameboy_system* gb) {
    int y;

    for (y = 0; y < PYEMU_GAMEBOY_HEIGHT; ++y) {
        pyemu_gameboy_render_scanline(gb, (uint8_t)y);
    }
}

/* Advance timers, LCD modes, scanline rendering, and interrupt state for a block of CPU cycles. */
void pyemu_gameboy_tick(pyemu_gameboy_system* gb, int cycles) {
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
            uint8_t current_ly = gb->memory[PYEMU_IO_LY];
            gb->ppu_counter -= PYEMU_GAMEBOY_CYCLES_PER_SCANLINE;
            if (current_ly < 144) {
                pyemu_gameboy_render_scanline(gb, current_ly);
            }
            gb->memory[PYEMU_IO_LY] = (uint8_t)(current_ly + 1);
            if (gb->memory[PYEMU_IO_LY] == 144) {
                pyemu_gameboy_request_interrupt(gb, PYEMU_INTERRUPT_VBLANK);
            } else if (gb->memory[PYEMU_IO_LY] > 153) {
                gb->memory[PYEMU_IO_LY] = 0;
            }

            {
                uint8_t ly = gb->memory[PYEMU_IO_LY];
                if (ly < PYEMU_GAMEBOY_HEIGHT) {
                    pyemu_gameboy_latch_scanline_registers(gb, ly);
                }
            }
            stat_needs_update = 1;
        }

        if (stat_needs_update) {
            pyemu_gameboy_update_stat(gb);
        }
    }
}

/* Estimate how many cycles remain before the next VBlank entry from the current PPU state. */
uint32_t pyemu_gameboy_cycles_until_vblank(const pyemu_gameboy_system* gb) {
    uint8_t ly = gb->memory[PYEMU_IO_LY];
    uint32_t lines_until_vblank;

    if (!pyemu_gameboy_lcd_enabled(gb)) {
        return 0;
    }
    if (ly >= 144) {
        return 0;
    }

    lines_until_vblank = (uint32_t)(144 - ly - 1);
    return (PYEMU_GAMEBOY_CYCLES_PER_SCANLINE - gb->ppu_counter) + (lines_until_vblank * PYEMU_GAMEBOY_CYCLES_PER_SCANLINE);
}

/* Skip idle LCD time to the next VBlank while still preserving all visible side effects that the frame loop expects. */
void pyemu_gameboy_fast_forward_to_vblank(pyemu_gameboy_system* gb) {
    uint8_t line;
    uint32_t skip_cycles = pyemu_gameboy_cycles_until_vblank(gb);
    if (skip_cycles == 0) {
        return;
    }

    for (line = gb->memory[PYEMU_IO_LY]; line < 144; ++line) {
        if (line < PYEMU_GAMEBOY_HEIGHT) {
            pyemu_gameboy_latch_scanline_registers(gb, line);
            pyemu_gameboy_render_scanline(gb, line);
        }
    }

    gb->cycle_count += skip_cycles;
    gb->div_counter += skip_cycles;
    gb->memory[PYEMU_IO_DIV] = (uint8_t)((gb->div_counter >> 8) & 0xFF);
    gb->ppu_counter = 0;
    gb->memory[PYEMU_IO_LY] = 144;
    pyemu_gameboy_update_stat(gb);
    pyemu_gameboy_request_interrupt(gb, PYEMU_INTERRUPT_VBLANK);
}
