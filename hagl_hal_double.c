/*

MIT License

Copyright (c) 2019-2023 Mika Tuupola

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

-cut-

This file is part of the Raspberry Pi Pico MIPI DCS backend for the HAGL
graphics library: https://github.com/tuupola/hagl_pico_mipi

SPDX-License-Identifier: MIT

-cut-

This is the backend when double buffering is enabled. The GRAM of the
display driver chip is the framebuffer. The memory allocated by the
backend is the back buffer. Total two buffers.

Note that all coordinates are already clipped in the main library itself.
Backend does not need to validate the coordinates, they can always be
assumed to be valid.

*/

#include "hagl_hal.h"

#ifdef HAGL_HAL_USE_DOUBLE_BUFFER

#include <string.h>
#include <hardware/gpio.h>
#include <hardware/dma.h>
#include <hardware/spi.h>
#include <hardware/irq.h>
#include <mipi_display.h>
#include <mipi_dcs.h>

#include <hagl/backend.h>
#include <hagl/bitmap.h>
#include <hagl.h>

#include <stdio.h>
#include <stdlib.h>

static hagl_bitmap_t bb;

static void flush_dma_pixel_double();
void mipi_display_set_address_xyxy(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

static size_t
flush(void *self)
{
#if MIPI_DISPLAY_PIN_TE > 0
    while (!gpio_get(MIPI_DISPLAY_PIN_TE)) {}
#endif /* MIPI_DISPLAY_PIN_TE > 0 */

#if HAGL_HAL_PIXEL_SIZE==1
    /* Flush the whole back buffer. */
    return mipi_display_write_xywh(0, 0, bb.width, bb.height, (uint8_t *) bb.buffer);
#endif /* HAGL_HAL_PIXEL_SIZE==1 */

#if HAGL_HAL_PIXEL_SIZE==2
#ifdef HAGL_HAL_USE_DMA
    flush_dma_pixel_double();
#else
    static hagl_color_t line[MIPI_DISPLAY_WIDTH];

    hagl_color_t *ptr = (hagl_color_t *) bb.buffer;
    size_t sent = 0;

    for (uint16_t y = 0; y < HAGL_PICO_MIPI_DISPLAY_HEIGHT; y++) {
        for (uint16_t x = 0; x < HAGL_PICO_MIPI_DISPLAY_WIDTH; x++) {
            line[x * 2] = *(ptr);
            line[x * 2 + 1] = *(ptr++);
        }
        sent += mipi_display_write_xywh(0, y * 2, MIPI_DISPLAY_WIDTH, 1, (uint8_t *) line);
        sent += mipi_display_write_xywh(0, y * 2 + 1, MIPI_DISPLAY_WIDTH, 1, (uint8_t *) line);
    }
    return sent;
#endif
#endif /* HAGL_HAL_PIXEL_SIZE==2 */
}

struct dma_transfer_t
{
    bool transfer_in_progress;
    hagl_color_t line[MIPI_DISPLAY_WIDTH];
    hagl_color_t *ptr;
    size_t transfer_count;
    uint16_t y;
 //   uint16_t x;
    bool even_line;
    int dma_chan;
    uint64_t start_time_us;
} dma_transfer;

static void
flush_dma_pixel_double()
{
    if (!dma_transfer.transfer_in_progress)
    {
        dma_transfer.transfer_in_progress = true;
        dma_transfer.ptr = (hagl_color_t *) bb.buffer;
        dma_transfer.transfer_count = 0;
        dma_transfer.y = 0;
     //   dma_transfer.x = 0;
        dma_transfer.even_line = true;
        dma_transfer.dma_chan = dma_claim_unused_channel(true);
        dma_transfer.start_time_us = time_us_64();
        memset(dma_transfer.line, 0, sizeof(dma_transfer.line));

        dma_channel_config dma_config = dma_channel_get_default_config(dma_transfer.dma_chan);
        if (spi0 == MIPI_DISPLAY_SPI_PORT) {
            channel_config_set_dreq(&dma_config, DREQ_SPI0_TX);
        } else {
            channel_config_set_dreq(&dma_config, DREQ_SPI1_TX);
        }

        dma_channel_set_write_addr(dma_transfer.dma_chan, &spi_get_hw(MIPI_DISPLAY_SPI_PORT)->dr, false);

        dma_channel_set_irq0_enabled(dma_transfer.dma_chan, true);
        irq_set_exclusive_handler(DMA_IRQ_0, flush_dma_pixel_double);
        irq_set_enabled(DMA_IRQ_0, true);
        
        channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_8); // MIPI_DISPLAY_DEPTH = 2 => 16bit
        dma_channel_set_trans_count(dma_transfer.dma_chan, MIPI_DISPLAY_WIDTH*(MIPI_DISPLAY_DEPTH/8), false);
        
        dma_channel_set_config(dma_transfer.dma_chan, &dma_config, false);
        mipi_display_set_address_xyxy(0, 0, MIPI_DISPLAY_WIDTH-1, MIPI_DISPLAY_HEIGHT-1);
    }
    else
    {
        dma_hw->ints0 = 1u << dma_transfer.dma_chan;
    }

    if (dma_transfer.even_line)
    {
        dma_transfer.even_line = false;

        for (; dma_transfer.y < HAGL_PICO_MIPI_DISPLAY_HEIGHT; ) {
            for (uint16_t x = 0; x < HAGL_PICO_MIPI_DISPLAY_WIDTH; x++) {
                dma_transfer.line[x * 2] = *(dma_transfer.ptr);
                dma_transfer.line[x * 2 + 1] = *(dma_transfer.ptr++);
            }
            // start DMA 1
    //        mipi_display_set_address_xyxy(0, dma_transfer.y, MIPI_DISPLAY_WIDTH, dma_transfer.y);

            /* Incoming data */
            gpio_put(MIPI_DISPLAY_PIN_DC, 1);
            gpio_put(MIPI_DISPLAY_PIN_CS, 0);
            dma_channel_set_read_addr(dma_transfer.dma_chan, dma_transfer.line, true);
            dma_transfer.transfer_count++;
            dma_transfer.y++;
            return;
        }
    }
    else
    {
        // start DMA 2
        dma_transfer.even_line = true;
  //      mipi_display_set_address_xyxy(0, dma_transfer.y+1, MIPI_DISPLAY_WIDTH, dma_transfer.y+1);

        /* Incoming data */
        gpio_put(MIPI_DISPLAY_PIN_DC, 1);
        gpio_put(MIPI_DISPLAY_PIN_CS, 0);
        dma_channel_set_read_addr(dma_transfer.dma_chan, dma_transfer.line, true);
        dma_transfer.transfer_count++;
        return;
    }

    // done; update cs/dc etc.
    dma_transfer.transfer_in_progress = false;
    dma_channel_cleanup(dma_transfer.dma_chan);
    dma_channel_unclaim(dma_transfer.dma_chan);
    uint64_t time_taken_us = time_us_64() - dma_transfer.start_time_us;
    printf("DMA complete, time = %llu us (%llu ms). y = %d, transfer_count = %d;\n", time_taken_us, time_taken_us/1000, dma_transfer.y, dma_transfer.transfer_count);

    /* Set CS low to de-reserve the SPI bus. */
    gpio_put(MIPI_DISPLAY_PIN_CS, 1);
    //gpio_put(MIPI_DISPLAY_PIN_DC, 0);
}

static void
put_pixel(void *self, int16_t x0, int16_t y0, hagl_color_t color)
{
    bb.put_pixel(&bb, x0, y0, color);
}

static hagl_color_t
get_pixel(void *self, int16_t x0, int16_t y0)
{
    return bb.get_pixel(&bb, x0, y0);

}

static void
blit(void *self, int16_t x0, int16_t y0, hagl_bitmap_t *src)
{
    bb.blit(&bb, x0, y0, src);
}

static void
scale_blit(void *self, uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, hagl_bitmap_t *src)
{
    bb.scale_blit(&bb, x0, y0, w, h, src);
}

static void
hline(void *self, int16_t x0, int16_t y0, uint16_t width, hagl_color_t color)
{
    bb.hline(&bb, x0, y0, width, color);
}

static void
vline(void *self, int16_t x0, int16_t y0, uint16_t height, hagl_color_t color)
{
    bb.vline(&bb, x0, y0, height, color);
}

void
hagl_hal_init(hagl_backend_t *backend)
{
    mipi_display_init();

    if (!backend->buffer) {
        backend->buffer = calloc(HAGL_PICO_MIPI_DISPLAY_WIDTH * HAGL_PICO_MIPI_DISPLAY_HEIGHT * (HAGL_PICO_MIPI_DISPLAY_DEPTH / 8), sizeof(uint8_t));
        hagl_hal_debug("Allocated back buffer to address %p.\n", (void *) backend->buffer);
    } else {
        hagl_hal_debug("Using provided back buffer at address %p.\n", (void *) backend->buffer);
    }

    backend->width = HAGL_PICO_MIPI_DISPLAY_WIDTH;
    backend->height = HAGL_PICO_MIPI_DISPLAY_HEIGHT;
    backend->depth = HAGL_PICO_MIPI_DISPLAY_DEPTH;
    backend->put_pixel = put_pixel;
    backend->get_pixel = get_pixel;
    backend->hline = hline;
    backend->vline = vline;
    backend->blit = blit;
    backend->scale_blit = scale_blit;
    backend->flush = flush;

    hagl_bitmap_init(&bb, backend->width, backend->height, backend->depth, backend->buffer);
}

#endif /* HAGL_HAL_USE_DOUBLE_BUFFER */
