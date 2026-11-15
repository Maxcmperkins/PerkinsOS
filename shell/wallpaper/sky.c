/*
 * The view outside: a blue sky, a layer of cloud over the sea far below,
 * and thin high cloud above, all moving as you fly.
 *
 * This is not drawn with cairo. It writes pixels straight into the frame,
 * because it has to colour every pixel of the window every frame and a
 * hand-written loop is far cheaper than vector drawing for that.
 *
 * How it works (the technique old racing games called "mode 7"):
 *
 *   One picture of cloud, a "texture", is made once at start-up. It tiles:
 *   its right edge matches its left, its bottom matches its top.
 *
 *   Imagine that texture laid flat like a carpet, far below the aircraft,
 *   stretching to the horizon. For each row of the screen below the
 *   horizon we work out how far away the carpet is along that row: rows
 *   just under the horizon see carpet that is far away, rows at the
 *   bottom see carpet that is close. Distance is one division:
 *
 *       distance = constant / (rows below the horizon)
 *
 *   Far away, a step of one screen pixel covers a lot of carpet; close
 *   up, very little. So each row is the texture sampled along a line,
 *   with a step size proportional to its distance. Sliding the texture
 *   towards us a little each frame is what makes it look like flight.
 *
 *   The high cloud is the same idea with a second carpet above us.
 *
 * The inner loop does one table lookup per pixel and no floating-point
 * arithmetic. That matters here: without KVM, QEMU emulates floating
 * point in software, which is slow.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wallpaper.h"

/* The texture, at three levels of detail ("mip levels"): full size, and
 * two blurred, smaller copies. Far rows use a smaller copy. Sampling the
 * full texture with a big step would skip most of its pixels and the
 * result would sparkle as it moved. */
#define MIP_LEVELS 3
static const int mip_bits[MIP_LEVELS] = { 10, 8, 6 }; /* 1024, 256, 64 */
static uint8_t *mip[MIP_LEVELS];

/* ---- making the cloud texture ---- */

static uint32_t random_state = 0x2026b10fu;
static uint32_t random_next(void) {
	/* xorshift: three shifts and exclusive-ors give a good enough stream
	 * of numbers, the same on every run. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/*
 * Cloud-like noise: add together several layers of random blobs, each
 * layer twice as fine and half as strong as the one before. The coarse
 * layers give the big shapes, the fine layers the ragged edges.
 * (This is called fractal noise.)
 *
 * Each layer is a small grid of random numbers stretched up to the full
 * size, blending smoothly between neighbouring grid points. Wrapping the
 * grid at its edges is what makes the texture tile.
 */
static void build_texture(void) {
	const int size = 1 << mip_bits[0];
	uint16_t *sum = calloc((size_t)size * size, sizeof(*sum));

	/* smooth[f]: an S-shaped curve from 0 to 256, used to blend between
	 * grid points. A straight-line blend would leave visible creases
	 * along the grid lines. */
	int smooth[257];
	for (int f = 0; f <= 256; f++) {
		smooth[f] = (f * f * (768 - 2 * f)) >> 16;
	}

	for (int octave = 0; octave < 6; octave++) {
		int grid_size = 8 << octave;            /* 8, 16, ... 256 */
		int cell_shift = mip_bits[0] - 3 - octave; /* log2(size / grid_size) */
		int cell = 1 << cell_shift;
		int weight = 256 >> octave;             /* 256, 128, ... 8 */

		uint8_t *grid = malloc((size_t)grid_size * grid_size);
		for (int i = 0; i < grid_size * grid_size; i++) {
			grid[i] = random_next() >> 24;
		}

		for (int y = 0; y < size; y++) {
			int gy = y >> cell_shift;
			int fy = smooth[((y & (cell - 1)) << 8) >> cell_shift];
			const uint8_t *row0 = grid + gy * grid_size;
			const uint8_t *row1 = grid + ((gy + 1) % grid_size) * grid_size;
			for (int x = 0; x < size; x++) {
				int gx = x >> cell_shift;
				int gx1 = (gx + 1) % grid_size;
				int fx = smooth[((x & (cell - 1)) << 8) >> cell_shift];
				/* Blend the four surrounding grid points: along the top
				 * edge, along the bottom edge, then between the two. */
				int top = row0[gx] * (256 - fx) + row0[gx1] * fx;
				int bottom = row1[gx] * (256 - fx) + row1[gx1] * fx;
				int value = (top * (256 - fy) + bottom * fy) >> 16;
				sum[y * size + x] += (value * weight) >> 8;
			}
		}
		free(grid);
	}

	/* Stretch the result to use the whole range 0..255. */
	int lowest = 65535, highest = 0;
	for (int i = 0; i < size * size; i++) {
		if (sum[i] < lowest) lowest = sum[i];
		if (sum[i] > highest) highest = sum[i];
	}
	mip[0] = malloc((size_t)size * size);
	for (int i = 0; i < size * size; i++) {
		mip[0][i] = (sum[i] - lowest) * 255 / (highest - lowest);
	}
	free(sum);

	/* Each smaller copy: every pixel is the average of a 4x4 block of the
	 * level above. */
	for (int level = 1; level < MIP_LEVELS; level++) {
		int small = 1 << mip_bits[level];
		int big = 1 << mip_bits[level - 1];
		mip[level] = malloc((size_t)small * small);
		for (int y = 0; y < small; y++) {
			for (int x = 0; x < small; x++) {
				int total = 0;
				for (int dy = 0; dy < 4; dy++) {
					for (int dx = 0; dx < 4; dx++) {
						total += mip[level - 1][(y * 4 + dy) * big + x * 4 + dx];
					}
				}
				mip[level][y * small + x] = total / 16;
			}
		}
	}
}

/* ---- colours ---- */

struct rgb { int r, g, b; };

static const struct rgb sky_zenith = { 30, 92, 178 };     /* deep blue overhead */
static const struct rgb sky_horizon = { 186, 214, 240 };  /* pale haze */
static const struct rgb sea_colour = { 26, 66, 112 };
static const struct rgb cloud_shade = { 168, 184, 206 };  /* thin cloud edge */
static const struct rgb cloud_white = { 255, 255, 255 };

/* Mix two colours; amount runs 0 (all a) to 256 (all b). */
static struct rgb mix(struct rgb a, struct rgb b, int amount) {
	struct rgb out = {
		a.r + (((b.r - a.r) * amount) >> 8),
		a.g + (((b.g - a.g) * amount) >> 8),
		a.b + (((b.b - a.b) * amount) >> 8),
	};
	return out;
}

/* 0 below `from`, 256 above `to`, a straight ramp in between. */
static int ramp(int value, int from, int to) {
	if (value <= from) return 0;
	if (value >= to) return 256;
	return (value - from) * 256 / (to - from);
}

/* A pixel in the frame's format: 0xAARRGGBB with alpha 255 (opaque). */
static uint32_t pack(struct rgb c) {
	return 0xff000000u | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

/* What the ground looks like for each texture value, before haze: low
 * values are open sea, high values are cloud, brightest at its thickest. */
static struct rgb ground_colour[256];
/* How much high cloud there is for each texture value (0..256). */
static int cirrus_cover[256];

static void build_colour_tables(void) {
	for (int n = 0; n < 256; n++) {
		/* A little variation in the sea so it isn't one flat colour. */
		struct rgb sea = mix(sea_colour, sky_horizon, n / 6);
		struct rgb cloud = mix(cloud_shade, cloud_white, ramp(n, 128, 196));
		ground_colour[n] = mix(sea, cloud, ramp(n, 112, 144));
		cirrus_cover[n] = ramp(n, 138, 210);
	}
}

/* ---- public functions (see wallpaper.h) ---- */

void sky_init(void) {
	if (mip[0] != NULL) {
		return; /* already built */
	}
	build_texture();
	build_colour_tables();
}

void sky_render(uint32_t *pixels, int stride_pixels, const struct sky_view *view) {
	const double scale = view->scale;
	/* A sideways shift of the whole view, in pixels, as the heading changes. */
	const double centre = view->width / 2.0 - view->pan;

	for (int y = 0; y < view->rows; y++) {
		uint32_t *out = pixels + (size_t)y * stride_pixels;
		double below = y - view->horizon; /* rows below the horizon */
		uint32_t table[256]; /* this row's colour for each texture value */
		double step;  /* texture pixels covered by one screen pixel */
		double depth; /* how far along the carpet this row is */

		if (below < 0) {
			/* Sky. Its colour deepens from haze at the horizon to blue
			 * overhead; sqrt makes most of the change happen low down. */
			double height = -below / (560 * scale);
			if (height > 1) height = 1;
			struct rgb base = mix(sky_horizon, sky_zenith,
				(int)(256 * sqrt(height)));
			/* High cloud: a carpet above us. It thins out towards the
			 * horizon, where we would be looking along it edge-on. */
			double distance = 1.0 / (-below + 6 * scale);
			step = 46 * scale * distance;
			depth = 46 * scale * 520 * scale * distance + view->time * 5;
			int strength = ramp((int)(-below / scale), 6, 150) * 150 / 256;
			for (int n = 0; n < 256; n++) {
				table[n] = pack(mix(base, cloud_white,
					(cirrus_cover[n] * strength) >> 8));
			}
		} else {
			/* Sea and cloud below. Haze: the further away, the more the
			 * colour is pulled towards the pale horizon colour. */
			double distance = 1.0 / (below + 1.5 * scale);
			step = 60 * scale * distance;
			depth = 60 * scale * 900 * scale * distance + view->time * 210;
			int haze = (int)(256 / (1 + below / (34 * scale)));
			for (int n = 0; n < 256; n++) {
				table[n] = pack(mix(ground_colour[n], sky_horizon, haze));
			}
		}

		/* Pick the level of detail so a step is never much more than a
		 * couple of texture pixels. Each level is 4x smaller, so texture
		 * coordinates shrink by 4 too. */
		int level = 0;
		while (level < MIP_LEVELS - 1 && step > 2.5) {
			level++;
			step /= 4;
			depth /= 4;
		}
		const int bits = mip_bits[level];
		const uint32_t mask = (1u << bits) - 1;
		const uint8_t *texture_row =
			mip[level] + (((uint32_t)(int64_t)depth & mask) << bits);

		/* Walk along the row in 16.16 fixed point: the top 16 bits are
		 * the texture column, the bottom 16 a fraction. Adding `du` each
		 * pixel replaces a multiplication, and letting the number wrap
		 * round is what makes the texture repeat. */
		uint32_t u = (uint32_t)(int64_t)(-centre * step * 65536.0);
		uint32_t du = (uint32_t)(int64_t)(step * 65536.0);
		for (int x = 0; x < view->width; x++) {
			out[x] = table[texture_row[(u >> 16) & mask]];
			u += du;
		}
	}
}

/* ---- laying a cached image over the frame ---- */

struct span { int y, x0, x1; };

struct overlay *overlay_create(const uint32_t *image, int stride_pixels,
		int width, int height) {
	struct overlay *overlay = calloc(1, sizeof(*overlay));
	overlay->image = image;
	overlay->stride_pixels = stride_pixels;

	/* Find the runs of non-transparent pixels in each row, once. Short
	 * gaps are bridged so that a row is a few long runs, not hundreds of
	 * tiny ones. */
	int capacity = 0;
	for (int y = 0; y < height; y++) {
		const uint32_t *row = image + (size_t)y * stride_pixels;
		int x = 0;
		while (x < width) {
			while (x < width && (row[x] >> 24) == 0) x++;
			if (x == width) break;
			int start = x, end = x, gap = 0;
			while (x < width && gap < 24) {
				if ((row[x] >> 24) != 0) {
					end = x + 1;
					gap = 0;
				} else {
					gap++;
				}
				x++;
			}
			if (overlay->count == capacity) {
				capacity = capacity ? capacity * 2 : 1024;
				overlay->spans = realloc(overlay->spans,
					capacity * sizeof(struct span));
			}
			struct span *span = &((struct span *)overlay->spans)[overlay->count++];
			span->y = y;
			span->x0 = start;
			span->x1 = end;
		}
	}
	return overlay;
}

void overlay_destroy(struct overlay *overlay) {
	if (overlay != NULL) {
		free(overlay->spans);
		free(overlay);
	}
}

void overlay_blit(const struct overlay *overlay, uint32_t *pixels,
		int stride_pixels) {
	const struct span *spans = overlay->spans;
	for (int i = 0; i < overlay->count; i++) {
		const uint32_t *src = overlay->image +
			(size_t)spans[i].y * overlay->stride_pixels;
		uint32_t *dst = pixels + (size_t)spans[i].y * stride_pixels;
		for (int x = spans[i].x0; x < spans[i].x1; x++) {
			uint32_t s = src[x];
			uint32_t alpha = s >> 24;
			if (alpha == 255) {
				dst[x] = s; /* opaque: just copy */
			} else if (alpha != 0) {
				/* Partly transparent (the soft edge of a shape): keep
				 * (255 - alpha)/255 of what is underneath and add the
				 * overlay, whose colours are already multiplied by alpha.
				 * Red and blue are done together in one multiplication;
				 * they sit far enough apart in the word not to collide. */
				uint32_t keep = 256 - alpha;
				uint32_t d = dst[x];
				uint32_t rb = (((d & 0x00ff00ffu) * keep) >> 8) & 0x00ff00ffu;
				uint32_t g = (((d & 0x0000ff00u) * keep) >> 8) & 0x0000ff00u;
				dst[x] = 0xff000000u | ((rb | g) + (s & 0x00ffffffu));
			}
		}
	}
}
