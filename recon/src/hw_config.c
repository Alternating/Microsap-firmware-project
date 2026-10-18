// hw_config.c — SD card wiring for no-OS-FatFS-SD. Matches pico2maple's alternate layout:
// microSD on SPI1, SCK=GPIO10, TX(MOSI)=GPIO11, RX(MISO)=GPIO12, CS=GPIO13.
// (Standard layout uses the same GPIO10-13; only Maple/CYW/buzzer differ between layouts.)
#include "hw_config.h"

static spi_t spi = {
    .hw_inst   = spi1,
    .sck_gpio  = 10,
    .mosi_gpio = 11,
    .miso_gpio = 12,
    .baud_rate = 12 * 1000 * 1000,
};

static sd_spi_if_t spi_if = {
    .spi    = &spi,
    .ss_gpio = 13,
};

static sd_card_t sd_card = {
    .type     = SD_IF_SPI,
    .spi_if_p = &spi_if,
};

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num) { return (num == 0) ? &sd_card : NULL; }
