/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dynamic LVGL demo for the ST7735S TFT on SPI2 - the display bring-up and
 * stress test the static "Hello world" cannot be: it keeps the whole screen
 * changing so that every failure mode of the link shows up as something
 * specific on the panel.
 *
 * What runs and what each piece is for:
 *
 *   - the full-screen background steps through black -> red -> green ->
 *     blue -> white every 700 ms. Black and white check that every pixel
 *     writes (white is all bits one, so a stuck data line or a broken byte
 *     swap shows as the wrong shade), and the three primaries tell RGB from
 *     BGR and inverted from not.
 *   - an amber rectangle bounces around the screen at ~30 Hz - continuous
 *     partial refreshes, the pattern a stalled or misplaced window update
 *     breaks first.
 *   - a row of solid color bars (red, green, blue) along the bottom edge
 *     stays put - a constant reference while everything else moves.
 *   - the header line shows the background color name, the measured frame
 *     rate and the uptime, so a console-less board still identifies itself.
 *
 * Console output: the panel geometry and pixel format as the display stack
 * sees them, then nothing - everything else is on the screen.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>
#include <stdio.h>
#include <zephyr/kernel.h>

struct demo {
	/* Widgets */
	lv_obj_t *bg;
	lv_obj_t *ball;
	lv_obj_t *header;
	/* Bounce state */
	int32_t ball_x, ball_y;
	int32_t ball_dx, ball_dy;
	/* Color cycle state */
	uint8_t color_idx;
	/* Frame counting: one animation tick == at most one redraw */
	uint32_t frames;
};

static struct demo demo;

static const struct {
	uint32_t value;
	const char *name;
} bg_colors[] = {
	{ 0x000000, "black" },
	{ 0xff0000, "red" },
	{ 0x00ff00, "green" },
	{ 0x0000ff, "blue" },
	{ 0xffffff, "white" },
};

#define BALL_SIZE 24
#define ANIM_PERIOD_MS 33 /* ~30 Hz */
#define COLOR_PERIOD_MS 700

static void anim_timer_cb(lv_timer_t *timer)
{
	struct demo *d = &demo;
	int32_t w = lv_display_get_horizontal_resolution(NULL);
	int32_t h = lv_display_get_vertical_resolution(NULL);

	d->ball_x += d->ball_dx;
	d->ball_y += d->ball_dy;

	if (d->ball_x <= 0 || d->ball_x >= w - BALL_SIZE) {
		d->ball_dx = -d->ball_dx;
		d->ball_x = CLAMP(d->ball_x, 0, w - BALL_SIZE);
	}
	if (d->ball_y <= 0 || d->ball_y >= h - BALL_SIZE) {
		d->ball_dy = -d->ball_dy;
		d->ball_y = CLAMP(d->ball_y, 0, h - BALL_SIZE);
	}

	lv_obj_set_pos(d->ball, d->ball_x, d->ball_y);
	d->frames++;
}

static void color_timer_cb(lv_timer_t *timer)
{
	struct demo *d = &demo;

	d->color_idx = (d->color_idx + 1U) % ARRAY_SIZE(bg_colors);
	lv_obj_set_style_bg_color(d->bg, lv_color_hex(bg_colors[d->color_idx].value),
				  LV_PART_MAIN);
}

static void fps_timer_cb(lv_timer_t *timer)
{
	struct demo *d = &demo;
	char text[64];

	snprintf(text, sizeof(text), "%s | %u fps | up %u s",
		 bg_colors[d->color_idx].name, d->frames,
		 (uint32_t)(k_uptime_get_32() / 1000U));
	lv_label_set_text(d->header, text);
	d->frames = 0;
}

int main(void)
{
	const struct device *display_dev;
	struct display_capabilities caps;
	lv_obj_t *screen;
	int32_t w, h;
	int ret;

	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		printk("display device not ready\n");
		return 0;
	}

	display_get_capabilities(display_dev, &caps);
	printk("display: %ux%u, pixel format %d\n", caps.x_resolution, caps.y_resolution,
	       caps.current_pixel_format);

	w = caps.x_resolution;
	h = caps.y_resolution;

	/* The active screen doubles as the full-screen cycling background */
	screen = lv_screen_active();
	demo.bg = screen;
	lv_obj_set_style_bg_color(screen, lv_color_hex(bg_colors[0].value), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);

	/* Header line: color name, frame rate, uptime */
	demo.header = lv_label_create(screen);
	lv_label_set_text(demo.header, "black | 0 fps | up 0 s");
	lv_obj_set_style_text_color(demo.header, lv_color_hex(0x40e040), LV_PART_MAIN);
	lv_obj_align(demo.header, LV_ALIGN_TOP_LEFT, 4, 4);

	/* Bouncing ball */
	demo.ball = lv_obj_create(screen);
	lv_obj_set_size(demo.ball, BALL_SIZE, BALL_SIZE);
	lv_obj_set_style_bg_color(demo.ball, lv_color_hex(0xffa000), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(demo.ball, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_width(demo.ball, 0, LV_PART_MAIN);
	lv_obj_set_style_radius(demo.ball, 4, LV_PART_MAIN);
	demo.ball_x = 8;
	demo.ball_y = 32;
	demo.ball_dx = 3;
	demo.ball_dy = 2;
	lv_obj_set_pos(demo.ball, demo.ball_x, demo.ball_y);

	/*
	 * Three solid color bars along the bottom edge (red, green, blue left
	 * to right) - a fixed reference while everything else moves.
	 */
	for (int i = 0; i < 3; i++) {
		lv_obj_t *bar = lv_obj_create(screen);

		lv_obj_remove_style_all(bar);
		lv_obj_set_size(bar, w / 3, 12);
		lv_obj_set_pos(bar, i * (w / 3), h - 12);
		lv_obj_set_style_bg_color(bar, lv_color_hex(0xff0000U << (8U * (2U - i))),
					  LV_PART_MAIN);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
	}

	lv_timer_create(anim_timer_cb, ANIM_PERIOD_MS, &demo);
	lv_timer_create(color_timer_cb, COLOR_PERIOD_MS, &demo);
	lv_timer_create(fps_timer_cb, 1000, &demo);

	/*
	 * The blanking state is what actually gates the panel: turn it on
	 * explicitly in case the driver defaults to suspended.
	 */
	ret = display_blanking_off(display_dev);
	if (ret != 0) {
		printk("display_blanking_off: %d\n", ret);
	}

	for (;;) {
		lv_task_handler();
		k_sleep(K_MSEC(10));
	}

	return 0;
}
