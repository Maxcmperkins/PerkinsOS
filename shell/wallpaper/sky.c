/*
 * The view outside: two layers of cloud above us, two below over the sea,
 * and fog where they all meet at the horizon, all moving as you fly.
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
 *   Three layers are carpets like that, each at its own height and speed:
 *   two above us, and loose fog below. The nearer slides past the further,
 *   and that difference (parallax) is what the eye reads as depth.
 *
 *   The cloud over the sea is not flat: thick cloud stands up as hills, so
 *   a near cloud can hide a far one. See draw_deck().
 *
 *   Each texture pixel holds two things: how thick the cloud is there, and
 *   how brightly the sun lights it. The light is worked out once, when the
 *   texture is made, so shading costs nothing per frame. The two are packed
 *   into one number, so drawing still takes one table lookup per layer.
 *
 * The inner loops use no floating-point arithmetic. That matters here:
 * without KVM, QEMU emulates floating point in software, which is slow.
 */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
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
static uint16_t *mip[MIP_LEVELS];

/* A texture pixel ("texel") is 10 bits: thickness in the top 6, light in
 * the bottom 4. Drawing uses tables with a colour for each of the 1024
 * possibilities, so more bits would mean slower tables. */
#define LIGHT_BITS 4
#define LIGHT_LEVELS (1 << LIGHT_BITS)
#define THICK_LEVELS 64
#define TEXELS (THICK_LEVELS * LIGHT_LEVELS)
/* The part of the 0..255 thickness that the 6 bits cover. */
#define THICK_FROM 72
#define THICK_RANGE 160

/* The same texture again for the cloud deck, which needs smooth heights
 * more than it needs small numbers: 8 bits of thickness in bits 16-23 and 8
 * of light in bits 0-7, with gaps so that both can be blended in one
 * multiplication. Each level is half the size of the one before. */
#define RELIEF_LEVELS 5
static uint32_t *relief[RELIEF_LEVELS];
/* Height of the tallest cloud. Low enough to stay under the loose fog. */
#define DECK_TOP 32
#define RELIEF_BITS(level) (mip_bits[0] - (level))

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

	/* Stretch the result to use the whole range 0..255, in 8.8 fixed point:
	 * the 8 extra bits keep small slopes accurate for the light below. */
	int lowest = 65535, highest = 0;
	for (int i = 0; i < size * size; i++) {
		if (sum[i] < lowest) lowest = sum[i];
		if (sum[i] > highest) highest = sum[i];
	}
	for (int i = 0; i < size * size; i++) {
		sum[i] = (uint32_t)(sum[i] - lowest) * 65535u / (highest - lowest);
	}

	/* Light. Treat thickness as the height of a hill, with the sun low on
	 * the left and ahead. Where the ground towards the sun is lower, this
	 * side faces the sun and is bright; where it is higher, it is in
	 * shade. Two distances: the near one picks out small lumps, the far
	 * one whole clouds. */
	uint16_t *light = malloc((size_t)size * size * sizeof(*light));
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			int here = sum[y * size + x];
			int near = sum[((y + 5) & (size - 1)) * size + ((x - 5) & (size - 1))];
			int far = sum[((y + 16) & (size - 1)) * size + ((x - 16) & (size - 1))];
			int value = 32768 + (here - near) * 2 + (here - far) * 3 / 2;
			if (value < 0) value = 0;
			if (value > 65535) value = 65535;
			light[y * size + x] = value;
		}
	}

	relief[0] = malloc((size_t)size * size * sizeof(*relief[0]));
	for (int i = 0; i < size * size; i++) {
		relief[0][i] = ((uint32_t)(sum[i] >> 8) << 16) | (light[i] >> 8);
	}
	for (int level = 1; level < RELIEF_LEVELS; level++) {
		int big = 1 << RELIEF_BITS(level - 1), small = big / 2;
		const uint32_t *from = relief[level - 1];
		relief[level] = malloc((size_t)small * small * sizeof(*relief[level]));
		for (int y = 0; y < small; y++) {
			for (int x = 0; x < small; x++) {
				const uint32_t *block = from + 2 * y * big + 2 * x;
				uint32_t total = block[0] + block[1] +
					block[big] + block[big + 1];
				relief[level][y * small + x] = (total >> 2) & 0x00ff00ffu;
			}
		}
	}

	/* Pack each level, then average 4x4 blocks to make the next. The
	 * averaging happens on the two separate values: averaging packed
	 * numbers would let thickness spill into light. */
	for (int level = 0; level < MIP_LEVELS; level++) {
		int width = 1 << mip_bits[level];
		mip[level] = malloc((size_t)width * width * sizeof(*mip[level]));
		for (int i = 0; i < width * width; i++) {
			/* Only thicknesses 72..231 matter (clear sky is below, solid
			 * cloud above), so spend the 6 bits on those. */
			int thick = ((sum[i] >> 8) - THICK_FROM) * THICK_LEVELS / THICK_RANGE;
			if (thick < 0) thick = 0;
			if (thick >= THICK_LEVELS) thick = THICK_LEVELS - 1;
			int lit = light[i] >> (16 - LIGHT_BITS);
			mip[level][i] = (thick << LIGHT_BITS) | lit;
		}
		if (level == MIP_LEVELS - 1) {
			break;
		}
		/* Shrinking in place is safe: the block being written is never
		 * further along than the one being read. */
		int small = 1 << mip_bits[level + 1];
		for (int y = 0; y < small; y++) {
			for (int x = 0; x < small; x++) {
				uint32_t thick_total = 0, light_total = 0;
				for (int dy = 0; dy < 4; dy++) {
					for (int dx = 0; dx < 4; dx++) {
						int from = (y * 4 + dy) * width + x * 4 + dx;
						thick_total += sum[from];
						light_total += light[from];
					}
				}
				sum[y * small + x] = thick_total / 16;
				light[y * small + x] = light_total / 16;
			}
		}
	}
	free(light);
	free(sum);
}

/* ---- colours ---- */

struct rgb { int r, g, b; };

static const struct rgb sky_zenith = { 22, 74, 158 };     /* deep blue overhead */
static const struct rgb sky_low = { 150, 190, 228 };      /* paler lower down */
static const struct rgb fog_colour = { 206, 220, 235 };   /* at the horizon */
static const struct rgb sea_colour = { 26, 66, 112 };
static const struct rgb sea_light = { 84, 122, 160 };
static const struct rgb cloud_shade = { 128, 148, 180 };  /* facing away from the sun */
static const struct rgb cloud_white = { 255, 253, 248 };  /* facing it */

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

/* Mix two packed pixels. Red and blue are done together in one
 * multiplication; they sit far enough apart in the word not to collide. */
static inline uint32_t mix_packed(uint32_t a, uint32_t b, uint32_t amount) {
	uint32_t keep = 256 - amount;
	uint32_t rb = ((a & 0x00ff00ffu) * keep + (b & 0x00ff00ffu) * amount) >> 8;
	uint32_t g = ((a & 0x0000ff00u) * keep + (b & 0x0000ff00u) * amount) >> 8;
	return 0xff000000u | (rb & 0x00ff00ffu) | (g & 0x0000ff00u);
}

/* For each texel: the colour of cloud there, how much of the view each
 * layer's cloud hides (0..256). */
static uint32_t cloud_colour[TEXELS];
static uint16_t wisp_cover[TEXELS];
static uint16_t cirrus_cover[TEXELS], puff_cover[TEXELS];

/* The cloud deck and the sea, before fog, for a thickness and a light. */
#define DECK_LIGHTS 32
static uint32_t deck_colour[256][DECK_LIGHTS];
/* How high the top of the deck stands above the sea for a thickness, in
 * texture pixels, 8.8 fixed point. */
static int deck_height[256];

static void build_colour_tables(void) {
	for (int i = 0; i < TEXELS; i++) {
		int n = THICK_FROM + (i >> LIGHT_BITS) * THICK_RANGE / THICK_LEVELS;
		int lit = (i & (LIGHT_LEVELS - 1)) * 256 / (LIGHT_LEVELS - 1);
		/* Thick cloud scatters light about, so it is paler in shade too. */
		struct rgb shade = mix(cloud_shade, cloud_white, ramp(n, 120, 220) / 4);
		struct rgb cloud = mix(shade, cloud_white, lit);
		cloud_colour[i] = pack(cloud);
		/* Wide ramps: cloud that thins out gradually looks like vapour.
		 * A narrow ramp looks cut out with scissors. */
		wisp_cover[i] = ramp(n, 122, 226) * 200 / 256;
		cirrus_cover[i] = ramp(n, 108, 214);
		puff_cover[i] = ramp(n, 128, 206);
	}

	for (int n = 0; n < 256; n++) {
		/* Cloud tops start rising where the cloud starts. This curve
		 * makes them round-topped, like cumulus; a straight line would
		 * make them pointed, and a square root, walls at the edges. */
		double tall = ramp(n, 100, 225) / 256.0;
		deck_height[n] = (int)(DECK_TOP * 256 * tall * (2 - tall));
		/* A little variation in the sea so it isn't one flat colour. */
		struct rgb sea = mix(sea_colour, sea_light, ramp(n, 60, 130) / 3);
		for (int i = 0; i < DECK_LIGHTS; i++) {
			/* Low parts of a cloud sit in the shadow of the rest. */
			int lit = i * 256 / (DECK_LIGHTS - 1);
			lit = lit * (112 + ramp(n, 104, 170) * 144 / 256) / 256;
			lit += (256 - lit) * ramp(n, 130, 230) / 640;
			struct rgb cloud = mix(cloud_shade, cloud_white, lit);
			deck_colour[n][i] = pack(mix(sea, cloud, ramp(n, 96, 124)));
		}
	}
}

/* How fast we fly, in metres a second: about Mach 1 at 24,500 feet, where
 * sound is slower than it is at the ground. */
#define AIR_SPEED 311.0
/* How many metres of sky one texture pixel covers in each layer. This is
 * what sets how fast a layer goes by: the same speed over bigger clouds
 * takes longer. The loose fog is close, the high cloud far. */
#define DECK_METRES 26.0
#define FOG_METRES 4.5
#define PUFF_METRES 16.0
#define CIRRUS_METRES 60.0

/* The cloud deck's camera. Distances are in texture pixels. */
#define EYE_HEIGHT 60.0   /* above the sea */
#define EYE_FOCAL 900.0   /* lens length, design pixels: sets perspective */
#define DECK_NEAR 128     /* where the first slice is */
#define DECK_FAR 8192     /* where the fog has hidden everything */

/* How much fog hides something this many design pixels from the horizon,
 * above or below (0..256). The same curve both ways, so there is no line
 * where sky meets sea; squaring makes it a thick band that then clears. */
static int fog_at(double distance, double reach) {
	double d = distance / reach;
	return (int)(256 / (1 + d * d));
}

/* ---- walking along one row of one layer ---- */

struct walk {
	const uint16_t *row; /* the line of texture this screen row sees */
	uint32_t mask;       /* keeps a column inside the texture */
	uint32_t u, du;      /* column, and how far to move each time */
};

/*
 * Set up a walk. `step` is texture pixels per screen pixel, `depth` how far
 * along the carpet the row is, `shift` slides the carpet sideways so that
 * two layers don't show the same clouds.
 */
static struct walk walk_start(double step, double depth, double shift,
		double centre) {
	/* Pick the level of detail so a step is never much more than a couple
	 * of texture pixels. Each level is 4x smaller, so texture coordinates
	 * shrink by 4 too. */
	int level = 0;
	while (level < MIP_LEVELS - 1 && step > 2.5) {
		level++;
		step /= 4;
		depth /= 4;
		shift /= 4;
	}
	const int bits = mip_bits[level];
	const uint32_t mask = (1u << bits) - 1;
	/* 16.16 fixed point: the top 16 bits are the texture column, the
	 * bottom 16 a fraction. Adding `du` each time replaces a
	 * multiplication, and letting the number wrap round is what makes the
	 * texture repeat. Two screen pixels per sample (see sky_render). */
	return (struct walk){
		.row = mip[level] + (((uint32_t)(int64_t)depth & mask) << bits),
		.mask = mask,
		.u = (uint32_t)(int64_t)((shift - centre * step) * 65536.0),
		.du = (uint32_t)(int64_t)(step * 2 * 65536.0),
	};
}

/* ---- public functions (see wallpaper.h) ---- */

void sky_init(void) {
	if (mip[0] != NULL) {
		return; /* already built */
	}
	build_texture();
	build_colour_tables();
}

/* ---- the cloud deck, in 3D ---- */

/*
 * Draw the cloud deck and the sea below the horizon, as a "height field":
 * every point of the texture has a height, and we look across the hills.
 *
 * We cut the scene into slices across our path, nearest first. For each
 * slice and each screen column: find the cloud's height there, and from it
 * the screen row its top appears on. If that is above everything drawn in
 * this column so far, fill the rows in between; if not, something nearer
 * hides it. So `top[]`, one row number per column, is all that is needed
 * to get hiding right, and no pixel is ever drawn twice.
 *
 * Slices sit at fixed places in the cloud, not at fixed distances from us.
 * If they moved with us they would cut each cloud in a different place
 * every frame, and the clouds would seem to boil. The gap between slices
 * doubles each time the distance does, like the size of things on screen
 * halving.
 */
static void draw_deck(uint32_t *pixels, int stride_pixels,
		const struct sky_view *view, double centre, int first_row) {
	/* One column for every two screen pixels, as in sky_render(). The
	 * arrays are only made again when the width changes. */
	static int allocated;
	static int *top, *last_row;     /* per column: see above; and the row */
	static uint32_t *last_colour;   /* and colour of the slice before */
	int columns = (view->width + 1) / 2;
	if (allocated != columns) {
		top = realloc(top, columns * sizeof(*top));
		last_row = realloc(last_row, columns * sizeof(*last_row));
		last_colour = realloc(last_colour, columns * sizeof(*last_colour));
		allocated = columns;
	}
	for (int x = 0; x < columns; x++) {
		top[x] = view->rows;
		last_row[x] = -1; /* no slice before the first */
	}

	const double focal = EYE_FOCAL * view->scale;
	const uint32_t fog_pixel = pack(fog_colour);
	/* How far we have flown. The texture repeats, so this can too, which
	 * keeps the numbers small. */
	const double flown = fmod(view->time * AIR_SPEED / DECK_METRES,
		1 << mip_bits[0]);

	int gap = DECK_NEAR / 64, level = 0;
	for (int near = DECK_NEAR; near < DECK_FAR; near *= 2, gap *= 2) {
		/* Use the copy of the texture whose pixels are a quarter of a gap. */
		if (gap > 4) {
			level++;
		}
		const int bits = RELIEF_BITS(level);
		const uint32_t mask = (1u << bits) - 1;

		/* Slices are at multiples of `gap`, measured along the cloud. */
		int along = (int)ceil((near + flown) / gap) * gap;
		for (; along < 2 * near + flown; along += gap) {
			double distance = along - flown;
			double pixels_per_unit = focal / distance;
			/* Rows below the horizon of the sea here, and how many rows
			 * 1/256 of a unit of height is worth: 16.16 fixed point. */
			int sea_row = (int)((view->horizon +
				EYE_HEIGHT * pixels_per_unit) * 65536);
			int rows_per_height = (int)(pixels_per_unit * 256);
			int fog = fog_at(EYE_HEIGHT * pixels_per_unit / view->scale, 64);

			const uint32_t *line = relief[level] +
				((((uint32_t)along >> level) & mask) << bits);
			double step = 1 / (pixels_per_unit * (1 << level));
			uint32_t u = (uint32_t)(int64_t)((view->drift / DECK_METRES / (1 << level) -
				centre * step) * 65536);
			uint32_t du = (uint32_t)(int64_t)(step * 2 * 65536);

			for (int x = 0; x < columns; x++, u += du) {
				/* Blend the two nearest texture pixels, or the hills
				 * would be made of square blocks. */
				uint32_t f = (u >> 8) & 255;
				uint32_t texel = ((line[(u >> 16) & mask] * (256 - f) +
					line[((u >> 16) + 1) & mask] * f) >> 8) & 0x00ff00ffu;
				int thick = texel >> 16;
				uint32_t colour = deck_colour[thick][(texel & 255) >> 3];
				int row = (sea_row - deck_height[thick] * rows_per_height) >> 16;

				/* Fill up from the top of what is drawn, going smoothly
				 * from the last slice's colour to this one's. */
				if (row < top[x]) {
					int stop = row < first_row ? first_row : row;
					uint32_t here = mix_packed(colour, fog_pixel, fog);
					uint32_t before = here;
					int span = last_row[x] - row;
					if (span > 1) {
						before = mix_packed(last_colour[x], fog_pixel, fog);
					}
					uint32_t *out = pixels +
						(size_t)(top[x] - 1) * stride_pixels + 2 * x;
					/* If the slice before was hidden, this is the outline of a
					 * nearer cloud. Cloud has no hard edge, and the steps of
					 * one would show: let the near colour bleed up a little. */
					uint32_t bleed = last_row[x] > top[x] + 1 ? 160 : 0;
					if (top[x] == view->rows) bleed = 0; /* nothing drawn yet */
					for (int y = top[x] - 1; y >= stop; y--) {
						uint32_t c = here;
						if (span > 1 && y > row && y < last_row[x]) {
							c = mix_packed(here, before, (y - row) * 256 / span);
						}
						if (bleed != 0) {
							c = mix_packed(c, out[stride_pixels], bleed);
							bleed = bleed > 24 ? bleed - 24 : 0;
						}
						out[0] = c;
						if (2 * x + 1 < view->width) {
							out[1] = c;
						}
						out -= stride_pixels;
					}
					if (stop < top[x]) {
						top[x] = stop;
					}
				}
				last_row[x] = row;
				last_colour[x] = colour;
			}
		}
	}

	/* Whatever is left, up to the horizon, is all fog. */
	for (int x = 0; x < view->width; x++) {
		for (int y = first_row; y < top[x / 2]; y++) {
			pixels[(size_t)y * stride_pixels + x] = fog_pixel;
		}
	}
}

/* ---- the whole view ---- */

void sky_render(uint32_t *pixels, int stride_pixels, const struct sky_view *view) {
	const double scale = view->scale;
	/* The column everything streams out of. The aircraft fly towards the
	 * same point (see draw_formation() in scene.c); if the two differed,
	 * they would seem to slip sideways through the cloud. */
	const double centre = view->width / 2.0;
	/* Turning slides the layers sideways instead. Both are in metres. */
	const double drift = view->drift, flown = view->time * AIR_SPEED;
	const uint32_t fog_pixel = pack(fog_colour);

	/* The deck first; the loop below then lays loose fog over it. */
	int first_row = (int)ceil(view->horizon);
	if (first_row < 0) first_row = 0;
	if (first_row < view->rows) {
		draw_deck(pixels, stride_pixels, view, centre, first_row);
	}

	/* The further layer is opaque: the sky is already mixed into its
	 * colours. The nearer one is laid over it, so it needs a colour and a
	 * cover for each texel. */
	static uint32_t far_table[TEXELS], near_colour[TEXELS];
	static uint16_t near_cover[TEXELS];
	double table_below = 0; /* the row the tables were made for */

	for (int y = 0; y < view->rows; y++) {
		uint32_t *out = pixels + (size_t)y * stride_pixels;
		double below = y - view->horizon; /* rows below the horizon */
		struct walk far = { 0 }, near;

		/* Filling the tables costs more than drawing a row, and away from
		 * the horizon neighbouring rows have all but the same colours. So
		 * reuse them: for about 1 row per 25 of distance, at most 4. */
		double reuse = fabs(below) / 25;
		if (reuse > 4) reuse = 4;
		bool new_tables = y == 0 || (below < 0) != (table_below < 0) ||
			below - table_below >= reuse;
		if (new_tables) {
			table_below = below;
		}

		if (below < 0) {
			double up = -below / scale; /* design pixels above the horizon */
			/* Sky deepens from pale to blue overhead; sqrt makes most of
			 * the change happen low down. */
			double height = up / 560;
			if (height > 1) height = 1;
			struct rgb blue = mix(sky_low, sky_zenith, (int)(256 * sqrt(height)));
			int fog = fog_at(up, 52);
			uint32_t base = mix_packed(pack(blue), fog_pixel, fog);

			/* High, slow cirrus (thin, so plain white), then lower, faster puffs under it. Both
			 * thin out towards the horizon, where we look at them
			 * edge-on and they would turn into stripes. */
			double distance = 1.0 / (-below + 6 * scale);
			far = walk_start(100 * scale * distance,
				100 * scale * 520 * scale * distance + flown / CIRRUS_METRES,
				drift / CIRRUS_METRES, centre);
			near = walk_start(150 * scale * distance,
				150 * scale * 620 * scale * distance + flown / PUFF_METRES,
				400 + drift / PUFF_METRES, centre);
			int far_strength = ramp((int)up, 6, 150) * 130 / 256;
			int near_strength = ramp((int)up, 24, 190) * 225 / 256;
			for (int i = 0; new_tables && i < TEXELS; i++) {
				far_table[i] = mix_packed(base, 0xffffffffu,
					(cirrus_cover[i] * far_strength) >> 8);
				near_colour[i] = mix_packed(cloud_colour[i], fog_pixel, fog);
				near_cover[i] = (puff_cover[i] * near_strength) >> 8;
			}
		} else {
			/* Loose fog drifting between the cloud deck and us. */
			double down = below / scale;
			double distance = 1.0 / (below + 1.5 * scale);
			near = walk_start(26 * scale * distance,
				26 * scale * 900 * scale * distance + flown / FOG_METRES,
				300 + drift / FOG_METRES, centre);
			int fog = fog_at(down, 64);
			int near_strength = ramp((int)down, 4, 70);
			for (int i = 0; new_tables && i < TEXELS; i++) {
				near_colour[i] = mix_packed(cloud_colour[i], fog_pixel, fog);
				near_cover[i] = (wisp_cover[i] * near_strength) >> 8;
			}
		}

		/* Sample every second pixel and write each sample twice. Cloud
		 * and sea have no sharp vertical edges, so the eye can't tell,
		 * and it halves the work of the busiest loop in the program. */
		int pairs = (view->width + 1) / 2;
		for (int x = 0; x < pairs; x++) {
			uint32_t colour = below < 0 ?
				far_table[far.row[(far.u >> 16) & far.mask]] : out[2 * x];
			uint32_t texel = near.row[(near.u >> 16) & near.mask];
			uint32_t cover = near_cover[texel];
			if (cover != 0) {
				colour = mix_packed(colour, near_colour[texel], cover);
			}
			out[2 * x] = colour;
			if (2 * x + 1 < view->width) {
				out[2 * x + 1] = colour;
			}
			far.u += far.du;
			near.u += near.du;
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
		int stride_pixels, int left, int top, int width, int height) {
	const struct span *spans = overlay->spans;
	for (int i = 0; i < overlay->count; i++) {
		int y = spans[i].y + top;
		if (y < 0 || y >= height) {
			continue;
		}
		/* Cut the run down to the part that is inside the frame. */
		int from = spans[i].x0 + left < 0 ? -left : spans[i].x0;
		int to = spans[i].x1 + left > width ? width - left : spans[i].x1;
		const uint32_t *src = overlay->image +
			(size_t)spans[i].y * overlay->stride_pixels;
		/* `dst` is moved along by `left` so that the same x works for
		 * both. */
		uint32_t *dst = pixels + (ptrdiff_t)y * stride_pixels + left;
		for (int x = from; x < to; x++) {
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
