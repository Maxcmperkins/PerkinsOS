/*
 * Lighting for the cockpit: turns a flat painting into something solid.
 *
 * scene.c draws the cockpit twice. Once in colour, as you would paint it.
 * Once as a "relief": a picture whose red says how high each point stands
 * towards you, whose green says how grainy its metal is, and whose blue
 * says how leathery. This file reads the relief and darkens or brightens
 * the colour picture to match. Every part is lit by the same sun, so they
 * belong together in a way that hand-painted highlights never quite do.
 *
 * For each pixel:
 *
 *   slope    The relief is a landscape. Comparing a pixel's neighbours
 *            gives the slope there, and so the direction the surface faces
 *            (its "normal"). Facing the sun is bright, facing away is dark.
 *   shine    If the surface faces halfway between the sun and your eye, it
 *            reflects the sun at you: a highlight.
 *   shadow   Walk from the pixel towards the sun. If the land rises above
 *            the sun's ray on the way, something is casting a shadow here.
 *   corners  Compare the pixel's height with the average around it. Lower
 *            than its surroundings: it is in a corner, where less light
 *            gets in, so darken it. Higher: it is an exposed edge, where
 *            paint wears off and bare metal shows. Dust collects in corners.
 *
 * It is slow, hundreds of operations a pixel, but it runs once, when the
 * cockpit picture is made. Drawing a frame only copies the result.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "wallpaper.h"

/* Towards the sun: left, up the screen, and out towards you. The same
 * side as the light on the clouds (sky.c). */
#define SUN_X (-0.46f)
#define SUN_Y (-0.58f)
#define SUN_Z 0.672f

static float clampf(float value, float low, float high) {
	return value < low ? low : value > high ? high : value;
}

/* A random-looking number from 0 to 1 that is always the same for the same
 * x and y. */
static float noise_at(int x, int y) {
	uint32_t n = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
	n = (n ^ (n >> 13)) * 1274126177u;
	return (float)((n ^ (n >> 16)) & 0xffff) / 65535.0f;
}

/* The same, but smooth: random at points `size` apart, blended between. */
static float smooth_noise(float x, float y, float size) {
	x /= size;
	y /= size;
	int xi = (int)floorf(x), yi = (int)floorf(y);
	float fx = x - xi, fy = y - yi;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	float top = noise_at(xi, yi) * (1 - fx) + noise_at(xi + 1, yi) * fx;
	float bottom = noise_at(xi, yi + 1) * (1 - fx) + noise_at(xi + 1, yi + 1) * fx;
	return top * (1 - fy) + bottom * fy;
}

/*
 * Blur a picture of numbers by averaging each with its neighbours up to
 * `radius` away, along rows and then along columns. The average is kept as
 * a running total: add the number coming in, take off the one going out.
 * So a wide blur costs no more than a narrow one.
 */
static void blur(float *data, float *scratch, int width, int height, int radius) {
	float share = 1.0f / (2 * radius + 1);
	for (int y = 0; y < height; y++) {
		const float *in = data + (size_t)y * width;
		float *out = scratch + (size_t)y * width;
		/* Start with the numbers around x = 0, less the last one, which
		 * the loop adds. Off the left edge, in[0] stands in. */
		float total = in[0] * radius;
		for (int i = 0; i < radius; i++) {
			total += in[i < width ? i : width - 1];
		}
		for (int x = 0; x < width; x++) {
			total += in[x + radius < width ? x + radius : width - 1];
			out[x] = total * share;
			total -= in[x > radius ? x - radius : 0];
		}
	}
	for (int x = 0; x < width; x++) {
		float total = scratch[x] * radius;
		for (int i = 0; i < radius; i++) {
			total += scratch[(size_t)(i < height ? i : height - 1) * width + x];
		}
		for (int y = 0; y < height; y++) {
			int enter = y + radius < height ? y + radius : height - 1;
			int leave = y > radius ? y - radius : 0;
			total += scratch[(size_t)enter * width + x];
			data[(size_t)y * width + x] = total * share;
			total -= scratch[(size_t)leave * width + x];
		}
	}
}

/*
 * Light `image` (ARGB32) using `relief` (RGB96F: three floating-point
 * numbers a pixel), which must be the same size. `scale` is pixels per
 * design unit. Heights in the relief are in units of RELIEF_UNITS.
 *
 * The relief is floating point because 256 levels are too few: a gentle
 * curve would become a flight of steps, and every step would catch light.
 */
void surface_light(cairo_surface_t *image, cairo_surface_t *relief,
		double scale) {
	int width = cairo_image_surface_get_width(image);
	int height = cairo_image_surface_get_height(image);
	size_t count = (size_t)width * height;
	float *high = malloc(count * sizeof(*high));
	float *around = malloc(count * sizeof(*around));
	float *scratch = malloc(count * sizeof(*scratch));
	if (high == NULL || around == NULL || scratch == NULL ||
			cairo_surface_status(relief) != CAIRO_STATUS_SUCCESS) {
		/* No memory for it: the cockpit stays flat, but it works. */
		free(high);
		free(around);
		free(scratch);
		return;
	}
	cairo_surface_flush(image);
	cairo_surface_flush(relief);
	unsigned char *pixels = cairo_image_surface_get_data(image);
	int stride = cairo_image_surface_get_stride(image);
	const unsigned char *bumps = cairo_image_surface_get_data(relief);
	int bump_stride = cairo_image_surface_get_stride(relief);
	const float unit = RELIEF_UNITS * (float)scale; /* to screen pixels */

	for (int y = 0; y < height; y++) {
		const float *bump = (const float *)(bumps + (size_t)y * bump_stride);
		for (int x = 0; x < width; x++) {
			float h = bump[3 * x] * unit;
			float leather = bump[3 * x + 2];
			if (leather > 0.01f) {
				/* Leather: soft lumps of two sizes. */
				float u = x / (float)scale, v = y / (float)scale;
				h += leather * (0.55f * smooth_noise(u, v, 2.4f) +
					0.5f * smooth_noise(u + 90, v, 5.5f)) * (float)scale;
			}
			high[(size_t)y * width + x] = h;
		}
	}
	/* Soften the very edges, or each would be one harsh bright pixel. */
	blur(high, scratch, width, height, 1);

	int reach = (int)(5 * scale + 0.5);
	for (size_t i = 0; i < count; i++) {
		around[i] = high[i];
	}
	blur(around, scratch, width, height, reach);
	blur(around, scratch, width, height, reach);

	/* How far shadows can reach, and how much the sun's ray climbs for each
	 * pixel it crosses. */
	int shadow_reach = (int)(22 * scale);
	const float flat = hypotf(SUN_X, SUN_Y);
	const float climb = SUN_Z / flat * 0.75f;
	const float go_x = SUN_X / flat, go_y = SUN_Y / flat;
	/* Halfway between the sun and your eye (which is straight out). */
	const float half_length = sqrtf(SUN_X * SUN_X + SUN_Y * SUN_Y +
		(SUN_Z + 1) * (SUN_Z + 1));
	const float half_x = SUN_X / half_length, half_y = SUN_Y / half_length;
	const float half_z = (SUN_Z + 1) / half_length;

	for (int y = 0; y < height; y++) {
		uint32_t *row = (uint32_t *)(pixels + (size_t)y * stride);
		const float *bump = (const float *)(bumps + (size_t)y * bump_stride);
		int up = y > 0 ? y - 1 : y, down = y < height - 1 ? y + 1 : y;
		for (int x = 0; x < width; x++) {
			uint32_t colour = row[x];
			uint32_t alpha = colour >> 24;
			if (alpha < 128) {
				continue; /* a hole to look out of, or glass */
			}
			size_t i = (size_t)y * width + x;
			int left = x > 0 ? x - 1 : x, right = x < width - 1 ? x + 1 : x;
			float grain = bump[3 * x + 1];
			float u = x / (float)scale, v = y / (float)scale;

			/* Slope, and from it the normal. */
			float slope_x = (high[(size_t)y * width + right] -
				high[(size_t)y * width + left]) / 2;
			float slope_y = (high[(size_t)down * width + x] -
				high[(size_t)up * width + x]) / 2;
			float length = sqrtf(slope_x * slope_x + slope_y * slope_y + 1);
			float nx = -slope_x / length, ny = -slope_y / length, nz = 1 / length;

			/* Shadow: the furthest anything pokes above the sun's ray. */
			float poke = 0;
			for (int step = 2; step <= shadow_reach; step += 2) {
				int sx = x + (int)(go_x * step), sy = y + (int)(go_y * step);
				if (sx < 0 || sy < 0) {
					break;
				}
				float over = high[(size_t)sy * width + sx] - high[i] -
					step * climb;
				if (over > poke) {
					poke = over;
				}
			}
			/* A ray only just blocked is half lit: a soft shadow edge. */
			float sun = 1 - clampf(poke / (1.6f * (float)scale), 0, 1) * 0.8f;

			/* Light arriving: sky from everywhere, sun from one side. Both
			 * are scaled so that flat, open metal keeps its own colour. */
			float facing = nx * SUN_X + ny * SUN_Y + nz * SUN_Z;
			facing = facing > 0 ? facing / SUN_Z : 0;
			float sky = 1 - 0.35f * ny;
			float light = 0.42f * sky * nz + 0.58f * facing * sun;
			light = clampf(light, 0.12f, 1.9f);

			float depth = (high[i] - around[i]) / (float)scale;
			if (depth < 0) {
				light *= clampf(1 + depth * 0.11f, 0.45f, 1);
			}

			float shine = 0, wear = 0, dust = 0;
			if (grain > 0.01f) {
				float dot = nx * half_x + ny * half_y + nz * half_z;
				dot = clampf(dot, 0, 1);
				/* dot to the 32nd power, by squaring five times. */
				for (int k = 0; k < 5; k++) {
					dot *= dot;
				}
				/* Flat metal reflects a little too; take that off so that
				 * only curves and edges shine. */
				float still = half_z;
				for (int k = 0; k < 5; k++) {
					still *= still;
				}
				shine = clampf(dot - still, 0, 1) * 0.34f * grain * sun;

				/* Painted metal: fine speckle, and slow patches of grime. */
				float speckle = noise_at(x, y) - 0.5f;
				float patches = smooth_noise(u, v, 46) * 0.6f +
					smooth_noise(u, v, 13) * 0.4f - 0.5f;
				light *= 1 + grain * (0.10f * speckle + 0.20f * patches);

				/* Dust gathers in corners, where nobody wipes. */
				dust = clampf((-depth - 0.35f) * 0.35f, 0, 1) *
					clampf(0.6f + patches, 0, 1) * 0.30f * grain;

				/* Bare metal on exposed edges, in irregular chips. */
				float chips = smooth_noise(u, v, 1.3f) * 0.45f +
					smooth_noise(u, v, 17) * 0.55f;
				wear = clampf((depth - 0.8f) * 0.8f, 0, 1) *
					clampf((chips - 0.58f) * 3, 0, 1) * 0.45f * grain;
			}

			float channel[3] = {
				(float)((colour >> 16) & 255), (float)((colour >> 8) & 255),
				(float)(colour & 255),
			};
			uint32_t out = alpha << 24;
			for (int c = 0; c < 3; c++) {
				/* The sun is a little warm and the shade a little blue. */
				static const float bare[3] = { 150, 154, 158 };
				static const float dusty[3] = { 118, 111, 98 };
				static const float warm[3] = { 255, 247, 232 };
				float value = channel[c] * (1 - wear) + bare[c] * wear;
				value = value * (1 - dust) + dusty[c] * dust;
				value = value * light + warm[c] * shine;
				/* Colours are stored multiplied by alpha, so none may
				 * exceed it. */
				value = clampf(value, 0, (float)alpha);
				out |= (uint32_t)(value + 0.5f) << (16 - 8 * c);
			}
			row[x] = out;
		}
	}
	cairo_surface_mark_dirty(image);
	free(high);
	free(around);
	free(scratch);
}
