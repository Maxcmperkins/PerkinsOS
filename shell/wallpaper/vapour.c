/*
 * Wing-tip vortices: the thin white trails that peel off an aircraft's
 * wing tips.
 *
 * The air under a wing is at a higher pressure than the air above it, and
 * at the tip it spills round from below to above. The wing moves on and
 * leaves the spinning air behind, like a rope laid along its path. In the
 * middle the pressure is so low that the water in the air turns to mist.
 *
 * So a trail runs straight back from the tip along the flight path, which
 * is towards us: the aircraft are ahead, flying the way we look. Every
 * point of it has the same X (metres to our right) as the tip and differs
 * only in Z (metres ahead). That makes it easy to draw a column at a time.
 * A point appears at
 *
 *     column = screen_x + focal * X / Z
 *
 * so for any column we can turn that round to get Z, and from Z how long
 * ago the wing passed, how wide the trail has grown and how far it has
 * faded. The trail sweeps sideways off the screen, so each column crosses
 * it once, from top to bottom.
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "wallpaper.h"

/* How far behind the tip a trail can be seen, and how near to us it is
 * drawn, in metres. */
#define TRAIL_LENGTH 27.0f
#define TRAIL_NEAREST 5.0f

void vapour_trail(const struct model_pose *pose, const double tip[3],
		double strength, double time, uint32_t *frame, int stride_pixels,
		int width, int rows) {
	const float focal = (float)pose->focal;
	const float x_metres = (float)tip[0], tip_z = (float)tip[2];
	if (fabsf(x_metres) < 1 || tip_z <= TRAIL_NEAREST) {
		return; /* straight ahead of us: it would be a dot, not a line */
	}
	const int way = x_metres < 0 ? -1 : 1; /* the side it runs off to */
	int x = (int)(pose->screen_x + focal * x_metres / tip_z);

	for (; x >= 0 && x < width; x += way) {
		float z = focal * x_metres / (x + 0.5f - (float)pose->screen_x);
		float age = tip_z - z; /* metres behind the wing tip */
		if (age < 0) {
			continue;
		}
		if (z < TRAIL_NEAREST || age > TRAIL_LENGTH) {
			break;
		}

		/* It spreads and starts to waver as it gets older. */
		float radius = 0.16f + 0.018f * age;
		float waver = 0.006f * age * (sinf(age * 0.9f + (float)time * 4.0f) +
			0.6f * sinf(age * 2.3f + (float)time * 6.5f));
		float middle = (float)pose->screen_y -
			focal * ((float)tip[1] + waver) / z;
		float reach = focal * radius / z; /* half its height, in pixels */
		if (reach < 1.2f) {
			reach = 1.2f;
		}

		/* It forms over the first metre, then thins out. The mist is
		 * uneven, in puffs that stream back. */
		float left = 1 - age / TRAIL_LENGTH;
		float cover = (float)strength * sqrtf(left) * left *
			(age < 1 ? age : 1) *
			(0.85f + 0.15f * sinf(age * 3.1f + (float)time * 9.0f));

		int from = (int)floorf(middle - reach), to = (int)ceilf(middle + reach);
		if (from < 0) from = 0;
		if (to > rows - 1) to = rows - 1;
		for (int y = from; y <= to; y++) {
			float across = (y + 0.5f - middle) / reach; /* -1 top, 1 bottom */
			float inside = 1 - across * across;
			if (inside <= 0) {
				continue;
			}
			/* A thin bright core in a wide faint glow. */
			float core = inside * inside * inside;
			core *= core;
			int amount = (int)(cover * (0.62f * inside + 0.36f * core) * 256);
			/* White on top where the sun reaches, grey-blue underneath, so
			 * that it shows up against white cloud. */
			int shade = (int)(95 * (across + 0.30f));
			if (shade < 0) shade = 0;
			uint32_t mist = (uint32_t)(255 - shade * 5 / 4) << 16 |
				(uint32_t)(255 - shade) << 8 | (uint32_t)(255 - shade * 2 / 3);

			uint32_t *pixel = frame + (size_t)y * stride_pixels + x;
			uint32_t keep = 256 - amount;
			uint32_t rb = ((*pixel & 0x00ff00ffu) * keep +
				(mist & 0x00ff00ffu) * amount) >> 8;
			uint32_t g = ((*pixel & 0x0000ff00u) * keep +
				(mist & 0x0000ff00u) * amount) >> 8;
			*pixel = 0xff000000u | (rb & 0x00ff00ffu) | (g & 0x0000ff00u);
		}
	}
}
