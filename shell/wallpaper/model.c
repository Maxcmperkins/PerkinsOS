/*
 * The aircraft: 3D models, drawn entirely in software.
 *
 * A model is a "mesh": a list of points in space (vertices) and a list of
 * triangles joining them. The meshes are read from .mesh files, made from
 * downloaded models by tools/glb2mesh.py (see load_mesh()).
 *
 * Drawing a mesh each frame (the "pipeline") is four steps:
 *
 *   1. Transform   turn and move every vertex from the model's own
 *                  coordinates to where it is in front of the camera
 *   2. Light       work out each vertex's colour from the angle between
 *                  its surface and the sun
 *   3. Project     divide by distance to get its position on the screen:
 *                  far things are smaller. Same rule as the sky.
 *   4. Rasterise   fill in each triangle, pixel by pixel, keeping for each
 *                  pixel only the surface nearest the camera (a "depth
 *                  buffer"), so near parts hide far parts automatically
 *
 * This is what a graphics card does, done by hand. Step 4 uses whole-number
 * arithmetic only, because without KVM floating point is slow here.
 *
 * Coordinates: x to the right, y up, z forwards. Units are metres. A model
 * has its nose towards +z, and the camera looks along +z, so we see tails.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wallpaper.h"

/* ---- the mesh ---- */

struct vec3 { float x, y, z; };

struct vertex {
	struct vec3 position;
	struct vec3 normal;   /* the direction the surface faces here */
	uint8_t r, g, b;
	uint8_t kind;         /* one of the KIND_ values below */
};

enum {
	KIND_PAINT, /* ordinary painted surface */
	KIND_GLASS, /* canopy: darker, with a bright glint */
	KIND_GLOW,  /* exhaust: gives out its own light, ignores the sun */
};

/* The same vertex in whole numbers, which is what the drawing code uses:
 * positions in 1/1024ths of a metre, directions scaled so that a length
 * of 1 is 16384 ("Q14" fixed point). Whole-number arithmetic is far faster
 * than floating point when the CPU is being emulated. */
struct fixed_vertex {
	int32_t x, y, z;
	int16_t nx, ny, nz;
	uint8_t r, g, b, kind;
};

struct mesh {
	struct fixed_vertex *fixed;
	struct vertex *vertices;
	int vertex_count;
	int *triangles; /* three vertex numbers per triangle */
	int triangle_count;
	/* Where the engine exhausts are, for the glow drawn over the top. */
	struct vec3 nozzles[2];
	float nozzle_radius;
	struct vec3 tips[2]; /* the left and right wing tips */
};

static struct vec3 vec(float x, float y, float z) {
	struct vec3 v = { x, y, z };
	return v;
}

static float dot(struct vec3 a, struct vec3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static struct vec3 normalise(struct vec3 v) {
	float length = sqrtf(dot(v, v));
	if (length < 1e-6f) {
		return vec(0, 1, 0);
	}
	return vec(v.x / length, v.y / length, v.z / length);
}

/* ---- the aircraft ---- */

static struct mesh typhoon, f35;

/* Make the whole-number copy of a finished mesh. */
static void finish_mesh(struct mesh *mesh) {
	mesh->fixed = malloc(mesh->vertex_count * sizeof(struct fixed_vertex));
	for (int i = 0; i < mesh->vertex_count; i++) {
		const struct vertex *v = &mesh->vertices[i];
		struct fixed_vertex *f = &mesh->fixed[i];
		f->x = (int32_t)lroundf(v->position.x * 1024);
		f->y = (int32_t)lroundf(v->position.y * 1024);
		f->z = (int32_t)lroundf(v->position.z * 1024);
		f->nx = (int16_t)lroundf(v->normal.x * 16383);
		f->ny = (int16_t)lroundf(v->normal.y * 16383);
		f->nz = (int16_t)lroundf(v->normal.z * 16383);
		f->r = v->r;
		f->g = v->g;
		f->b = v->b;
		f->kind = v->kind;
		/* The wing tips are the points furthest out to each side. */
		if (i == 0 || v->position.x < mesh->tips[0].x) {
			mesh->tips[0] = v->position;
		}
		if (i == 0 || v->position.x > mesh->tips[1].x) {
			mesh->tips[1] = v->position;
		}
	}
}

/* ---- loaded aircraft ---- */

/* Where the .mesh files are in the image. They are copied there from
 * buildroot/overlay/usr/share/perkinsos/models/. The build on an ordinary
 * distro (distro/Makefile) installs them somewhere else and says where. */
#ifndef MODEL_DIR
#define MODEL_DIR "/usr/share/perkinsos/models"
#endif

/*
 * Read a mesh file made by tools/glb2mesh.py into `mesh`. The file is
 * little-endian, the same byte order as the x86 CPU, so each number can
 * be read straight into a variable of the same type:
 *
 *   "PKMESH1\0"                          8 bytes, to check it's our format
 *   vertex count, triangle count         2 x uint32
 *   the two exhaust exits, their radius  7 x float, in metres
 *   each vertex: position, normal        6 x float
 *                r, g, b, kind           4 x uint8
 *   each triangle: three vertex numbers  3 x uint32
 *
 * Returns false and leaves the mesh empty if the file is missing or
 * damaged. Every vertex number is checked, because the drawing code uses
 * them as array indexes without looking.
 */
static bool load_mesh(struct mesh *mesh, const char *path) {
	FILE *file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "wallpaper: can't open %s\n", path);
		return false;
	}
	char magic[8];
	uint32_t counts[2]; /* vertices, triangles */
	float exhaust[7];
	bool ok = fread(magic, sizeof(magic), 1, file) == 1 &&
		memcmp(magic, "PKMESH1", sizeof(magic)) == 0 &&
		fread(counts, sizeof(counts), 1, file) == 1 &&
		fread(exhaust, sizeof(exhaust), 1, file) == 1 &&
		counts[0] > 0 && counts[0] <= 1000000 && counts[1] <= 1000000;

	if (ok) {
		mesh->vertex_count = (int)counts[0];
		mesh->triangle_count = (int)counts[1];
		mesh->vertices = malloc(counts[0] * sizeof(struct vertex));
		mesh->triangles = malloc(counts[1] * 3 * sizeof(int));
		ok = mesh->vertices && mesh->triangles;
	}
	for (uint32_t i = 0; ok && i < counts[0]; i++) {
		float f[6];
		uint8_t c[4];
		ok = fread(f, sizeof(f), 1, file) == 1 && fread(c, sizeof(c), 1, file) == 1;
		struct vertex *v = &mesh->vertices[i];
		v->position = vec(f[0], f[1], f[2]);
		v->normal = normalise(vec(f[3], f[4], f[5]));
		v->r = c[0];
		v->g = c[1];
		v->b = c[2];
		v->kind = c[3] <= KIND_GLOW ? c[3] : KIND_PAINT;
	}
	for (uint32_t i = 0; ok && i < counts[1] * 3; i++) {
		uint32_t index;
		ok = fread(&index, sizeof(index), 1, file) == 1 && index < counts[0];
		mesh->triangles[i] = (int)index;
	}
	fclose(file);

	if (!ok) {
		fprintf(stderr, "wallpaper: %s is not a valid mesh file\n", path);
		free(mesh->vertices);
		free(mesh->triangles);
		memset(mesh, 0, sizeof(*mesh));
		return false;
	}
	mesh->nozzles[0] = vec(exhaust[0], exhaust[1], exhaust[2]);
	mesh->nozzles[1] = vec(exhaust[3], exhaust[4], exhaust[5]);
	mesh->nozzle_radius = exhaust[6];
	return true;
}

void models_init(void) {
	static bool done;
	if (done) {
		return;
	}
	done = true;
	if (load_mesh(&typhoon, MODEL_DIR "/typhoon.mesh")) {
		finish_mesh(&typhoon);
	}
	if (load_mesh(&f35, MODEL_DIR "/f35.mesh")) {
		finish_mesh(&f35);
	}
}

/* ---- drawing ---- */

/* The picture is first drawn at twice the size in each direction, then
 * shrunk: each final pixel is the average of four samples. That smooths
 * the staircase edges triangles would otherwise have ("supersampling"). */
#define SAMPLES 2

/* The working image for one aircraft: colour and depth for every sample.
 * Reused from frame to frame and only grown when needed. */
static uint32_t *sample_colour;
static int32_t *sample_depth;
static int sample_capacity;
#define FAR_AWAY INT32_MAX

/* A vertex after steps 1-3: where it is on the working image, how far
 * away, and what colour the light makes it. */
struct shaded {
	int32_t x, y;      /* position, in 1/16ths of a sample */
	int32_t depth;     /* distance from the camera, in 1/1024 m */
	int32_t r, g, b;   /* 0..255 */
	int away;          /* the surface here faces away from the camera */
};

static int clamp_byte(int value) {
	return value < 0 ? 0 : value > 255 ? 255 : value;
}

/* The edge function: which side of the line a->b the point p is on, and
 * how far. It is zero on the line, positive on one side, negative on the
 * other. A point is inside a triangle if it is on the same side of all
 * three edges. (It is also twice the area of triangle a,b,p.) */
static int64_t edge(const struct shaded *a, const struct shaded *b,
		int32_t px, int32_t py) {
	return (int64_t)(b->x - a->x) * (py - a->y) -
		(int64_t)(b->y - a->y) * (px - a->x);
}

static void fill_triangle(const struct shaded *v0, const struct shaded *v1,
		const struct shaded *v2, int width, int height) {
	int64_t area = edge(v0, v1, v2->x, v2->y);
	if (area == 0) {
		return; /* edge-on: nothing to draw */
	}
	if (area < 0) {
		/* Wound the other way round: swap two corners so the edge
		 * functions are positive inside. We draw both sides of every
		 * triangle and let the depth buffer sort out what is visible. */
		const struct shaded *swap = v1;
		v1 = v2;
		v2 = swap;
		area = -area;
	}

	/* The box of samples that could be covered (positions are in 16ths). */
	int32_t min_x = v0->x, max_x = v0->x, min_y = v0->y, max_y = v0->y;
	if (v1->x < min_x) min_x = v1->x;
	if (v2->x < min_x) min_x = v2->x;
	if (v1->x > max_x) max_x = v1->x;
	if (v2->x > max_x) max_x = v2->x;
	if (v1->y < min_y) min_y = v1->y;
	if (v2->y < min_y) min_y = v2->y;
	if (v1->y > max_y) max_y = v1->y;
	if (v2->y > max_y) max_y = v2->y;
	int x0 = min_x >> 4, x1 = (max_x >> 4) + 1;
	int y0 = min_y >> 4, y1 = (max_y >> 4) + 1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > width) x1 = width;
	if (y1 > height) y1 = height;
	if (x0 >= x1 || y0 >= y1) {
		return;
	}

	/* Depth and colour vary smoothly across the triangle. Rather than
	 * work each out from scratch at every sample, find how much each
	 * changes per step right and per step down, once, and then only add.
	 * Values carry 8 extra bits of fraction so the adding stays accurate.
	 *
	 * Each of those is a division by the triangle's area. Division is
	 * slow, so do one: work out 1/area as a whole number scaled up by a
	 * power of two (`shift` bits, chosen so the result has about 20
	 * significant bits), and multiply by that instead. */
	int shift = 20 + (63 - __builtin_clzll((unsigned long long)area));
	int64_t inverse = ((int64_t)1 << shift) / area;
	/* For a quantity `field`, its change per sample right (dx) and per
	 * sample down (dy). The 12 in the shift is the x16 (positions are in
	 * 16ths of a sample) times the x256 (8 bits of fraction). */
#define GRADIENT(field, dx, dy) \
	int32_t dx = (int32_t)((((int64_t)(v1->y - v2->y) * (v0->field - v2->field) + \
		(int64_t)(v2->y - v0->y) * (v1->field - v2->field)) * inverse) >> (shift - 12)); \
	int32_t dy = (int32_t)((((int64_t)(v2->x - v1->x) * (v0->field - v2->field) + \
		(int64_t)(v0->x - v2->x) * (v1->field - v2->field)) * inverse) >> (shift - 12));
	GRADIENT(depth, depth_dx, depth_dy)
	GRADIENT(r, r_dx, r_dy)
	GRADIENT(g, g_dx, g_dy)
	GRADIENT(b, b_dx, b_dy)
#undef GRADIENT

	/* Values at the centre of the first sample: the value at corner v2,
	 * plus the change over the distance from v2 to that sample. */
	int32_t px = (x0 << 4) + 8, py = (y0 << 4) + 8;
#define START(field, dx, dy) \
	(((int64_t)v2->field << 8) + \
		((int64_t)dx * (px - v2->x) + (int64_t)dy * (py - v2->y)) / 16)
	int64_t depth_row = START(depth, depth_dx, depth_dy);
	int32_t r_row = (int32_t)START(r, r_dx, r_dy);
	int32_t g_row = (int32_t)START(g, g_dx, g_dy);
	int32_t b_row = (int32_t)START(b, b_dx, b_dy);
#undef START

	/* The three edge functions also change by a fixed amount per step. */
	int64_t e0_row = edge(v1, v2, px, py);
	int64_t e1_row = edge(v2, v0, px, py);
	int64_t e2_row = edge(v0, v1, px, py);
	int64_t e0_dx = -(int64_t)(v2->y - v1->y) * 16, e0_dy = (int64_t)(v2->x - v1->x) * 16;
	int64_t e1_dx = -(int64_t)(v0->y - v2->y) * 16, e1_dy = (int64_t)(v0->x - v2->x) * 16;
	int64_t e2_dx = -(int64_t)(v1->y - v0->y) * 16, e2_dy = (int64_t)(v1->x - v0->x) * 16;

	for (int y = y0; y < y1; y++) {
		int64_t e0 = e0_row, e1 = e1_row, e2 = e2_row;
		int64_t depth = depth_row;
		int32_t r = r_row, g = g_row, b = b_row;
		uint32_t *colour = sample_colour + y * width;
		int32_t *nearest = sample_depth + y * width;
		for (int x = x0; x < x1; x++) {
			if ((e0 | e1 | e2) >= 0) { /* inside all three edges */
				int32_t d = (int32_t)(depth >> 8);
				if (d < nearest[x]) { /* nearer than what is there */
					nearest[x] = d;
					int rr = r >> 8, gg = g >> 8, bb = b >> 8;
					if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
					if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
					if (bb < 0) bb = 0; else if (bb > 255) bb = 255;
					colour[x] = ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | bb;
				}
			}
			e0 += e0_dx; e1 += e1_dx; e2 += e2_dx;
			depth += depth_dx; r += r_dx; g += g_dx; b += b_dx;
		}
		e0_row += e0_dy; e1_row += e1_dy; e2_row += e2_dy;
		depth_row += depth_dy; r_row += r_dy; g_row += g_dy; b_row += b_dy;
	}
}

/* Add a soft orange halo of hot gas round an exhaust, straight into the
 * frame. It brightens what is already there (the colours are added), and
 * fades from the centre to nothing at `radius`. */
static void draw_halo(uint32_t *frame, int stride_pixels, int frame_width,
		int frame_height, int cx, int cy, int radius, int strength) {
	if (radius < 1) {
		return;
	}
	int radius2 = radius * radius;
	for (int y = cy - radius; y <= cy + radius; y++) {
		if (y < 0 || y >= frame_height) {
			continue;
		}
		uint32_t *row = frame + (size_t)y * stride_pixels;
		for (int x = cx - radius; x <= cx + radius; x++) {
			int distance2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
			if (x < 0 || x >= frame_width || distance2 >= radius2) {
				continue;
			}
			/* 256 at the centre down to 0 at the edge, squared so most
			 * of the light is near the middle. */
			int falloff = (radius2 - distance2) * 256 / radius2;
			int amount = (falloff * falloff >> 8) * strength >> 8;
			uint32_t c = row[x];
			int r = (int)((c >> 16) & 255) + (255 * amount >> 8);
			int g = (int)((c >> 8) & 255) + (150 * amount >> 8);
			int b = (int)(c & 255) + (60 * amount >> 8);
			row[x] = 0xff000000u | ((uint32_t)clamp_byte(r) << 16) |
				((uint32_t)clamp_byte(g) << 8) | clamp_byte(b);
		}
	}
}

/* Directions as Q14 whole numbers (16384 = length 1).
 * The sun: high, to the left, and behind us, so the surfaces facing the
 * camera are lit. `glint_axis` is halfway between the sun and the camera:
 * a surface facing that way reflects the sun straight at us. */
static const int32_t sun[3] = { -6901, 12158, -8544 };
static const int32_t glint_axis[3] = { -3957, 6971, -14293 };

/* The 3x3 matrix, row by row, that turns a point by the pose's three
 * angles: roll about the nose-to-tail axis, then pitch, then yaw. */
static void rotation_of(const struct model_pose *pose, float rotation[9]) {
	float cr = cosf(pose->roll), sr = sinf(pose->roll);
	float cp = cosf(pose->pitch), sp = sinf(pose->pitch);
	float cy = cosf(pose->yaw), sy = sinf(pose->yaw);
	const float result[9] = {
		cy * cr + sy * sp * sr, -cy * sr + sy * sp * cr, sy * cp,
		cp * sr,                cp * cr,                 -sp,
		-sy * cr + cy * sp * sr, sy * sr + cy * sp * cr, cy * cp,
	};
	memcpy(rotation, result, sizeof(result));
}

bool model_wing_tip(int which, int side, const struct model_pose *pose,
		double tip[3]) {
	const struct mesh *mesh = which == MODEL_TYPHOON ? &typhoon : &f35;
	if (mesh->vertex_count == 0) {
		return false;
	}
	float m[9];
	rotation_of(pose, m);
	struct vec3 p = mesh->tips[side];
	tip[0] = pose->x + m[0] * p.x + m[1] * p.y + m[2] * p.z;
	tip[1] = pose->y + m[3] * p.x + m[4] * p.y + m[5] * p.z;
	tip[2] = pose->z + m[6] * p.x + m[7] * p.y + m[8] * p.z;
	return true;
}

void model_draw(int which, const struct model_pose *pose, uint32_t *frame,
		int stride_pixels, int frame_width, int frame_height) {
	const struct mesh *mesh = which == MODEL_TYPHOON ? &typhoon : &f35;
	if (mesh->vertex_count == 0) {
		return; /* its file was missing: models_init() said so */
	}

	/* Step 1 set-up: one 3x3 rotation matrix from the three angles,
	 * converted to Q14. This handful of floating-point sums happens once per aircraft
	 * per frame; everything after it is whole numbers. */
	float rotation[9];
	rotation_of(pose, rotation);
	int32_t m[9];
	for (int i = 0; i < 9; i++) {
		m[i] = (int32_t)lroundf(rotation[i] * 16384);
	}
	const int32_t pose_x = (int32_t)(pose->x * 1024);
	const int32_t pose_y = (int32_t)(pose->y * 1024);
	const int32_t pose_z = (int32_t)(pose->z * 1024);

	const int unit = SAMPLES * 16; /* position units per screen pixel */
	const int64_t focal = (int64_t)(pose->focal * unit);
	const int32_t aim_x = (int32_t)(pose->screen_x * unit);
	const int32_t aim_y = (int32_t)(pose->screen_y * unit);
	const int glow = (int)(pose->glow * 256);

	/* Steps 1-3 for every vertex. */
	struct shaded *shaded = malloc(mesh->vertex_count * sizeof(*shaded));
	for (int i = 0; i < mesh->vertex_count; i++) {
		const struct fixed_vertex *v = &mesh->fixed[i];
		struct shaded *out = &shaded[i];

		/* 1. Transform: rotate (matrix times vector, then drop the 14
		 * bits of scaling), then move in front of the camera. */
		int32_t x = ((m[0] * v->x + m[1] * v->y + m[2] * v->z) >> 14) + pose_x;
		int32_t y = ((m[3] * v->x + m[4] * v->y + m[5] * v->z) >> 14) + pose_y;
		int32_t z = ((m[6] * v->x + m[7] * v->y + m[8] * v->z) >> 14) + pose_z;
		int32_t nx = (m[0] * v->nx + m[1] * v->ny + m[2] * v->nz) >> 14;
		int32_t ny = (m[3] * v->nx + m[4] * v->ny + m[5] * v->nz) >> 14;
		int32_t nz = (m[6] * v->nx + m[7] * v->ny + m[8] * v->nz) >> 14;

		/* The camera is at 0,0,0, so the vertex's own position is the
		 * direction from the camera to it. If the surface direction
		 * points the same way, we are looking at its back. */
		out->away = ((int64_t)nx * x + (int64_t)ny * y + (int64_t)nz * z) > 0;

		/* 2. Light. */
		int red, green, blue;
		if (v->kind == KIND_GLOW) {
			/* Half as bright again as its paint: dark orange on its own
			 * looks like rust, not heat. */
			red = clamp_byte(v->r * glow * 3 >> 9);
			green = clamp_byte(v->g * glow * 3 >> 9);
			blue = clamp_byte(v->b * glow * 3 >> 9);
		} else {
			/* Sunlight: brightest where the surface faces the sun,
			 * nothing where it faces away. (A dot product of two
			 * directions is the cosine of the angle between them.) */
			int32_t sunlight = (nx * sun[0] + ny * sun[1] + nz * sun[2]) >> 14;
			if (sunlight < 0) sunlight = 0;
			/* Skylight: bluish from above, greyer from below, where the
			 * cloud bounces light back up. `up` runs 0 to 16384. */
			int32_t up = (16384 + ny) >> 1;
			int32_t light_r = 4915 + (3277 * up >> 14) + (13107 * sunlight >> 14);
			int32_t light_g = 5407 + (3932 * up >> 14) + (12780 * sunlight >> 14);
			int32_t light_b = 6226 + (5571 * up >> 14) + (11796 * sunlight >> 14);
			red = v->r * light_r >> 14;
			green = v->g * light_g >> 14;
			blue = v->b * light_b >> 14;
			if (v->kind == KIND_GLASS) {
				/* A glint: a small bright spot where the glass reflects
				 * the sun at us. Raising to the 24th power (by squaring
				 * repeatedly) is what makes the spot small and sharp. */
				int32_t g1 = (nx * glint_axis[0] + ny * glint_axis[1] +
					nz * glint_axis[2]) >> 14;
				if (g1 > 0) {
					int32_t g2 = g1 * g1 >> 14, g4 = g2 * g2 >> 14;
					int32_t g8 = g4 * g4 >> 14, g16 = g8 * g8 >> 14;
					int32_t glint = (g16 * g8 >> 14) * 230 >> 14;
					red += glint; green += glint; blue += glint;
				}
			}
		}
		out->r = clamp_byte(red);
		out->g = clamp_byte(green);
		out->b = clamp_byte(blue);

		/* 3. Project: divide by distance. */
		if (z < 1024) z = 1024;
		out->x = aim_x + (int32_t)(focal * x / z);
		out->y = aim_y - (int32_t)(focal * y / z);
		out->depth = z;
	}

	/* The working image is the smallest box that holds every vertex,
	 * plus a pixel of margin. Sizing it to the aircraft rather than to a
	 * generous guess matters: clearing and shrinking it is a large part
	 * of the cost of a frame. */
	int32_t min_x = INT32_MAX, min_y = INT32_MAX;
	int32_t max_x = INT32_MIN, max_y = INT32_MIN;
	for (int i = 0; i < mesh->vertex_count; i++) {
		if (shaded[i].x < min_x) min_x = shaded[i].x;
		if (shaded[i].x > max_x) max_x = shaded[i].x;
		if (shaded[i].y < min_y) min_y = shaded[i].y;
		if (shaded[i].y > max_y) max_y = shaded[i].y;
	}
	int left = min_x / unit - 1, top = min_y / unit - 1;
	int box_width = max_x / unit + 2 - left, box_height = max_y / unit + 2 - top;
	for (int i = 0; i < mesh->vertex_count; i++) {
		shaded[i].x -= left * unit;
		shaded[i].y -= top * unit;
	}
	int width = box_width * SAMPLES, height = box_height * SAMPLES;
	if (width <= 0 || height <= 0 || box_width > frame_width * 2 ||
			box_height > frame_height * 2) {
		free(shaded);
		return; /* nothing sensible to draw */
	}
	if (width * height > sample_capacity) {
		sample_capacity = width * height;
		sample_colour = realloc(sample_colour, sample_capacity * sizeof(uint32_t));
		sample_depth = realloc(sample_depth, sample_capacity * sizeof(int32_t));
	}
	for (int i = 0; i < width * height; i++) {
		sample_depth[i] = FAR_AWAY;
	}

	/* Step 4 for every triangle that might be visible. Every part is a
	 * closed shape, so a triangle whose three corners all face away is on
	 * the far side of something and can be skipped: about half of them. */
	for (int i = 0; i < mesh->triangle_count; i++) {
		const int *t = &mesh->triangles[i * 3];
		if (shaded[t[0]].away && shaded[t[1]].away && shaded[t[2]].away) {
			continue;
		}
		fill_triangle(&shaded[t[0]], &shaded[t[1]], &shaded[t[2]], width, height);
	}
	free(shaded);

	/* Shrink onto the frame. A final pixel only partly covered by the
	 * aircraft is blended with what is behind it in proportion. */
	for (int y = 0; y < box_height; y++) {
		int fy = top + y;
		if (fy < 0 || fy >= frame_height) {
			continue;
		}
		uint32_t *out = frame + (size_t)fy * stride_pixels;
		for (int x = 0; x < box_width; x++) {
			int fx = left + x;
			if (fx < 0 || fx >= frame_width) {
				continue;
			}
			int covered = 0, r = 0, g = 0, b = 0;
			for (int sy2 = 0; sy2 < SAMPLES; sy2++) {
				int row = (y * SAMPLES + sy2) * width + x * SAMPLES;
				for (int sx2 = 0; sx2 < SAMPLES; sx2++) {
					if (sample_depth[row + sx2] != FAR_AWAY) {
						uint32_t c = sample_colour[row + sx2];
						r += (c >> 16) & 255;
						g += (c >> 8) & 255;
						b += c & 255;
						covered++;
					}
				}
			}
			if (covered == 0) {
				continue;
			}
			const int total = SAMPLES * SAMPLES;
			uint32_t behind = out[fx];
			int keep = total - covered;
			r = (r + keep * (int)((behind >> 16) & 255)) / total;
			g = (g + keep * (int)((behind >> 8) & 255)) / total;
			b = (b + keep * (int)(behind & 255)) / total;
			out[fx] = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
		}
	}

	/* The haze of hot gas over each exhaust. */
	for (int i = 0; i < 2; i++) {
		const struct vec3 *p = &mesh->nozzles[i];
		float x = rotation[0] * p->x + rotation[1] * p->y + rotation[2] * p->z + (float)pose->x;
		float y = rotation[3] * p->x + rotation[4] * p->y + rotation[5] * p->z + (float)pose->y;
		float z = rotation[6] * p->x + rotation[7] * p->y + rotation[8] * p->z + (float)pose->z;
		float scale = (float)pose->focal / z;
		draw_halo(frame, stride_pixels, frame_width, frame_height,
			(int)(pose->screen_x + scale * x), (int)(pose->screen_y - scale * y),
			(int)(scale * mesh->nozzle_radius * 1.4f), (int)(pose->glow * 80));
	}
}
