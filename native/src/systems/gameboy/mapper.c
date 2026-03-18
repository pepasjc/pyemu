#include "gameboy_internal.h"

#include <stdio.h>
#include <string.h>

int pyemu_gameboy_uses_mbc1(const pyemu_gameboy_system* gb) {
    return gb->cartridge_type >= 0x01 && gb->cartridge_type <= 0x03;
}

int pyemu_gameboy_uses_mbc3(const pyemu_gameboy_system* gb) {
    return gb->cartridge_type >= 0x0F && gb->cartridge_type <= 0x13;
}

int pyemu_gameboy_has_battery(const pyemu_gameboy_system* gb) {
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

int pyemu_gameboy_save_file_present(const pyemu_gameboy_system* gb) {
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

int pyemu_gameboy_has_external_ram(const pyemu_gameboy_system* gb) {
    return gb->eram_size > 0;
}

size_t pyemu_gameboy_eram_size_from_code(uint8_t ram_size_code) {
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

uint8_t pyemu_gameboy_mbc1_upper_bits(const pyemu_gameboy_system* gb) {
    return (uint8_t)(gb->mbc3_ram_bank & 0x03);
}

uint8_t pyemu_gameboy_mbc1_mode(const pyemu_gameboy_system* gb) {
    return (uint8_t)(gb->mbc3_rom_bank & 0x01);
}

uint8_t pyemu_gameboy_current_rom_bank(const pyemu_gameboy_system* gb) {
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

size_t pyemu_gameboy_current_eram_offset(const pyemu_gameboy_system* gb) {
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

uint8_t pyemu_gameboy_normalize_mbc1_bank(const pyemu_gameboy_system* gb, uint8_t value) {
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

int pyemu_gameboy_save_battery_ram(const pyemu_gameboy_system* gb) {
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

void pyemu_gameboy_load_battery_ram(pyemu_gameboy_system* gb) {
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

void pyemu_gameboy_refresh_rom_mapping(pyemu_gameboy_system* gb) {
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

void pyemu_gameboy_refresh_eram_window(pyemu_gameboy_system* gb) {
    if (gb->eram_bank_count > 0) {
        size_t window_size = gb->eram_size < PYEMU_GAMEBOY_ERAM_BANK_SIZE ? gb->eram_size : PYEMU_GAMEBOY_ERAM_BANK_SIZE;
        memset(gb->memory + 0xA000, 0xFF, PYEMU_GAMEBOY_ERAM_BANK_SIZE);
        memcpy(gb->memory + 0xA000, gb->eram + pyemu_gameboy_current_eram_offset(gb), window_size);
    }
}
