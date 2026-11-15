/*
 * The aircraft: 3D models, built and drawn entirely in software.
 *
 * A model is a "mesh": a list of points in space (vertices) and a list of
 * triangles joining them. Nothing is loaded from a file. Each aircraft is
 * assembled at start-up from a few kinds of part:
 *
 *   loft    a body made by joining a row of cross-sections, like the ribs
 *           of a boat: fuselage, canopy, engine pipes, missiles
 *   plate   a thin flat panel with a ridge down the middle: wings, fins,
 *           tailplanes, canards
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
#include <stdint.h>
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
	int vertex_count, vertex_capacity;
	int *triangles; /* three vertex numbers per triangle */
	int triangle_count, triangle_capacity;
	/* Where the engine exhausts are, for the glow drawn over the top. */
	struct vec3 nozzles[2];
	float nozzle_radius;
};

struct paint { uint8_t r, g, b, kind; };

static const struct paint grey = { 150, 158, 168, KIND_PAINT };
static const struct paint grey_dark = { 104, 110, 120, KIND_PAINT };
static const struct paint radome = { 84, 88, 96, KIND_PAINT };
static const struct paint bare_metal = { 196, 201, 208, KIND_PAINT };
static const struct paint hot_metal = { 92, 86, 82, KIND_PAINT };
static const struct paint soot = { 22, 20, 20, KIND_PAINT };
static const struct paint glass = { 44, 66, 92, KIND_GLASS };
static const struct paint flame = { 255, 176, 88, KIND_GLOW };
static const struct paint white = { 214, 216, 220, KIND_PAINT };

static struct vec3 vec(float x, float y, float z) {
	struct vec3 v = { x, y, z };
	return v;
}

static struct vec3 sub(struct vec3 a, struct vec3 b) {
	return vec(a.x - b.x, a.y - b.y, a.z - b.z);
}

static struct vec3 lerp(struct vec3 a, struct vec3 b, float t) {
	return vec(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
		a.z + (b.z - a.z) * t);
}

static float dot(struct vec3 a, struct vec3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

/* The cross product: a vector at right angles to both a and b. For two
 * edges of a flat face, that is the direction the face points. */
static struct vec3 cross(struct vec3 a, struct vec3 b) {
	return vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x);
}

static struct vec3 normalise(struct vec3 v) {
	float length = sqrtf(dot(v, v));
	if (length < 1e-6f) {
		return vec(0, 1, 0);
	}
	return vec(v.x / length, v.y / length, v.z / length);
}

static int add_vertex(struct mesh *mesh, struct vec3 position,
		struct vec3 normal, struct paint paint) {
	if (mesh->vertex_count == mesh->vertex_capacity) {
		mesh->vertex_capacity = mesh->vertex_capacity ?
			mesh->vertex_capacity * 2 : 256;
		mesh->vertices = realloc(mesh->vertices,
			mesh->vertex_capacity * sizeof(struct vertex));
	}
	struct vertex *v = &mesh->vertices[mesh->vertex_count];
	v->position = position;
	v->normal = normalise(normal);
	v->r = paint.r;
	v->g = paint.g;
	v->b = paint.b;
	v->kind = paint.kind;
	return mesh->vertex_count++;
}

static void add_triangle(struct mesh *mesh, int a, int b, int c) {
	if (mesh->triangle_count == mesh->triangle_capacity) {
		mesh->triangle_capacity = mesh->triangle_capacity ?
			mesh->triangle_capacity * 2 : 512;
		mesh->triangles = realloc(mesh->triangles,
			mesh->triangle_capacity * 3 * sizeof(int));
	}
	int *t = &mesh->triangles[mesh->triangle_count * 3];
	t[0] = a;
	t[1] = b;
	t[2] = c;
	mesh->triangle_count++;
}

/* A flat four-cornered face. `facing` says roughly which way is "out", so
 * the face's true direction (from the cross product) can be flipped to
 * agree with it. */
static void add_face(struct mesh *mesh, struct vec3 a, struct vec3 b,
		struct vec3 c, struct vec3 d, struct vec3 facing, struct paint paint) {
	struct vec3 normal = normalise(cross(sub(b, a), sub(d, a)));
	if (dot(normal, facing) < 0) {
		normal = vec(-normal.x, -normal.y, -normal.z);
	}
	int ia = add_vertex(mesh, a, normal, paint);
	int ib = add_vertex(mesh, b, normal, paint);
	int ic = add_vertex(mesh, c, normal, paint);
	int id = add_vertex(mesh, d, normal, paint);
	add_triangle(mesh, ia, ib, ic);
	add_triangle(mesh, ia, ic, id);
}

/* ---- part: loft ---- */

/* One cross-section of a loft. */
struct station {
	float z;                    /* position along the body */
	float half_width, half_height;
	float centre_y;             /* how far the section is raised */
	struct paint paint;
};

/* How many points go round each cross-section. More is rounder but costs
 * more triangles; small parts like missiles get by with fewer. */
#define MAX_LOFT_SIDES 18
static int loft_sides = MAX_LOFT_SIDES;

/*
 * Join a row of cross-sections into a smooth body. Each section is an
 * oval; `squareness` 2 is a true ellipse, larger numbers push it towards
 * a rounded box (a "superellipse"), which suits a fuselage with flat sides.
 * `centre_x` shifts the whole body sideways (for the second engine, etc).
 * The ends are closed with flat caps painted `front_cap` and `back_cap`.
 */
static void add_loft(struct mesh *mesh, const struct station *stations,
		int count, float squareness, float centre_x,
		struct paint front_cap, struct paint back_cap) {
	int first = mesh->vertex_count;
	float power = 2.0f / squareness;

	for (int s = 0; s < count; s++) {
		const struct station *st = &stations[s];
		/* How fast the body is widening here, to tilt the surface
		 * direction forwards on a nose cone and backwards on a boat-tail. */
		const struct station *before = &stations[s > 0 ? s - 1 : s];
		const struct station *after = &stations[s < count - 1 ? s + 1 : s];
		float dz = after->z - before->z;
		float growth = dz != 0 ? ((after->half_width + after->half_height) -
			(before->half_width + before->half_height)) / (2 * dz) : 0;

		for (int i = 0; i < loft_sides; i++) {
			float angle = (float)i / loft_sides * 2 * (float)M_PI;
			float c = cosf(angle), sn = sinf(angle);
			/* sign * |value|^power squashes the circle outwards. */
			float ux = (c < 0 ? -1 : 1) * powf(fabsf(c), power);
			float uy = (sn < 0 ? -1 : 1) * powf(fabsf(sn), power);
			struct vec3 position = vec(centre_x + ux * st->half_width,
				st->centre_y + uy * st->half_height, st->z);
			/* The surface direction of an oval is not towards its
			 * centre: dividing by the half-sizes corrects for the squash. */
			struct vec3 normal = vec(
				ux / (st->half_width > 0.01f ? st->half_width : 0.01f),
				uy / (st->half_height > 0.01f ? st->half_height : 0.01f), 0);
			normal = normalise(normal);
			normal.z = -growth;
			add_vertex(mesh, position, normal, st->paint);
		}
	}
	/* Join each section to the next with a band of triangles. */
	for (int s = 0; s < count - 1; s++) {
		for (int i = 0; i < loft_sides; i++) {
			int a = first + s * loft_sides + i;
			int b = first + s * loft_sides + (i + 1) % loft_sides;
			int c = a + loft_sides, d = b + loft_sides;
			add_triangle(mesh, a, b, d);
			add_triangle(mesh, a, d, c);
		}
	}
	/* Caps: a fan of triangles from the centre of each end. */
	for (int end = 0; end < 2; end++) {
		const struct station *st = &stations[end == 0 ? 0 : count - 1];
		/* Which way an end faces depends on which way the list runs. */
		float direction = (stations[0].z > stations[count - 1].z) ? 1 : -1;
		struct vec3 normal = vec(0, 0, end == 0 ? direction : -direction);
		struct paint paint = end == 0 ? front_cap : back_cap;
		int centre = add_vertex(mesh, vec(centre_x, st->centre_y, st->z),
			normal, paint);
		int ring = first + (end == 0 ? 0 : (count - 1) * loft_sides);
		int rim[MAX_LOFT_SIDES];
		for (int i = 0; i < loft_sides; i++) {
			/* Separate rim vertices so the cap gets its own flat shading. */
			rim[i] = add_vertex(mesh, mesh->vertices[ring + i].position,
				normal, paint);
		}
		for (int i = 0; i < loft_sides; i++) {
			add_triangle(mesh, centre, rim[i], rim[(i + 1) % loft_sides]);
		}
	}
}

/* A straight tube from z_front to z_back: missiles, fuel tanks, pods. */
static void add_tube(struct mesh *mesh, float x, float y, float z_front,
		float z_back, float radius, struct paint paint) {
	float length = z_front - z_back;
	struct station stations[] = {
		{ z_front, radius * 0.08f, radius * 0.08f, y, paint },
		{ z_front - length * 0.14f, radius * 0.80f, radius * 0.80f, y, paint },
		{ z_front - length * 0.30f, radius, radius, y, paint },
		{ z_back + length * 0.15f, radius, radius, y, paint },
		{ z_back, radius * 0.55f, radius * 0.55f, y, paint },
	};
	loft_sides = 8;
	add_loft(mesh, stations, 5, 2, x, paint, paint);
	add_loft(mesh, stations, 5, 2, -x, paint, paint);
	loft_sides = MAX_LOFT_SIDES;
}

/* A jet pipe: the outer shell, a dark inside, and the glowing exhaust
 * part-way down it. Records the exit so the scene can add a soft glow. */
static void add_engine(struct mesh *mesh, int index, float x, float y,
		float z_front, float z_exit, float radius) {
	struct station shell[] = {
		{ z_front, radius * 1.06f, radius * 1.06f, y, hot_metal },
		{ (z_front + z_exit) / 2, radius * 1.02f, radius * 1.02f, y, hot_metal },
		{ z_exit, radius * 0.90f, radius * 0.90f, y, hot_metal },
	};
	loft_sides = 14;
	add_loft(mesh, shell, 3, 2, x, hot_metal, soot);
	/* The flame sits a little way inside, so the rim of the pipe hides
	 * its edge when seen from the side. */
	struct station fire[] = {
		{ z_exit + 0.30f, radius * 0.74f, radius * 0.74f, y, flame },
		{ z_exit + 0.05f, radius * 0.74f, radius * 0.74f, y, flame },
	};
	add_loft(mesh, fire, 2, 2, x, flame, flame);
	loft_sides = MAX_LOFT_SIDES;
	mesh->nozzles[index] = vec(x, y, z_exit);
	mesh->nozzle_radius = radius;
}

/* ---- part: plate ---- */

/*
 * A wing-like panel between a root edge and a tip edge. Each edge runs
 * from a leading point to a trailing point. The panel is thickest part-way
 * back and tapers to nothing at the front and rear, so it has four sloping
 * faces: that slope is what catches the light like a real wing.
 * `up` is the direction of its thickness: (0,1,0) for a wing, (1,0,0) for
 * a fin. With `mirror`, a copy is made on the other side of the aircraft.
 */
static void add_plate(struct mesh *mesh, struct vec3 root_lead,
		struct vec3 root_trail, struct vec3 tip_lead, struct vec3 tip_trail,
		float root_thick, float tip_thick, struct vec3 up, struct paint paint,
		int mirror) {
	for (int side = 0; side < (mirror ? 2 : 1); side++) {
		struct vec3 rl = root_lead, rt = root_trail;
		struct vec3 tl = tip_lead, tt = tip_trail;
		struct vec3 u = up;
		if (side == 1) {
			rl.x = -rl.x; rt.x = -rt.x; tl.x = -tl.x; tt.x = -tt.x;
			u.x = -u.x;
		}
		struct vec3 down = vec(-u.x, -u.y, -u.z);
		struct vec3 root_mid = lerp(rl, rt, 0.38f);
		struct vec3 tip_mid = lerp(tl, tt, 0.38f);
		struct vec3 root_top = vec(root_mid.x + u.x * root_thick / 2,
			root_mid.y + u.y * root_thick / 2, root_mid.z + u.z * root_thick / 2);
		struct vec3 root_bottom = vec(root_mid.x - u.x * root_thick / 2,
			root_mid.y - u.y * root_thick / 2, root_mid.z - u.z * root_thick / 2);
		struct vec3 tip_top = vec(tip_mid.x + u.x * tip_thick / 2,
			tip_mid.y + u.y * tip_thick / 2, tip_mid.z + u.z * tip_thick / 2);
		struct vec3 tip_bottom = vec(tip_mid.x - u.x * tip_thick / 2,
			tip_mid.y - u.y * tip_thick / 2, tip_mid.z - u.z * tip_thick / 2);

		add_face(mesh, rl, tl, tip_top, root_top, u, paint);       /* top, front */
		add_face(mesh, root_top, tip_top, tt, rt, u, paint);       /* top, rear */
		add_face(mesh, rl, tl, tip_bottom, root_bottom, down, paint);
		add_face(mesh, root_bottom, tip_bottom, tt, rt, down, paint);
		/* Close the tip. */
		struct vec3 outward = sub(tip_mid, root_mid);
		add_face(mesh, tl, tip_top, tt, tip_bottom, outward, paint);
	}
}

/* ---- the three aircraft ---- */

static struct mesh typhoon, tornado, lightning;

/* Eurofighter Typhoon: 16 m long, 11 m span. A delta wing with no
 * tailplane, canards by the cockpit, a chin intake, two engines. */
static void build_typhoon(struct mesh *m) {
	struct station body[] = {
		{ 8.00f, 0.04f, 0.04f, 0.00f, radome },
		{ 7.40f, 0.24f, 0.25f, 0.00f, radome },
		{ 6.50f, 0.46f, 0.48f, 0.03f, radome },
		{ 6.45f, 0.47f, 0.49f, 0.03f, grey },
		{ 5.00f, 0.62f, 0.70f, 0.10f, grey },
		{ 3.40f, 0.80f, 0.80f, 0.06f, grey },
		{ 1.40f, 1.00f, 0.78f, 0.02f, grey },
		{ -2.00f, 1.12f, 0.74f, 0.00f, grey },
		{ -5.50f, 1.06f, 0.62f, 0.00f, grey },
		{ -7.30f, 0.98f, 0.54f, 0.00f, grey_dark },
	};
	add_loft(m, body, 10, 3.0f, 0, radome, soot);

	struct station intake[] = {
		{ 4.30f, 0.78f, 0.30f, -0.82f, grey_dark },
		{ 2.50f, 0.86f, 0.36f, -0.80f, grey },
		{ 0.00f, 0.92f, 0.32f, -0.66f, grey },
		{ -2.50f, 0.80f, 0.20f, -0.52f, grey },
	};
	add_loft(m, intake, 4, 5.0f, 0, soot, grey);

	struct station canopy[] = {
		{ 6.30f, 0.05f, 0.04f, 0.50f, glass },
		{ 5.70f, 0.32f, 0.26f, 0.60f, glass },
		{ 4.90f, 0.40f, 0.40f, 0.70f, glass },
		{ 3.90f, 0.36f, 0.32f, 0.68f, glass },
		{ 2.90f, 0.14f, 0.08f, 0.66f, grey },
	};
	add_loft(m, canopy, 5, 2.0f, 0, glass, grey);

	/* Delta wing, slightly drooped (anhedral). */
	add_plate(m, vec(1.00f, -0.22f, 2.40f), vec(1.00f, -0.22f, -6.30f),
		vec(5.45f, -0.34f, -4.70f), vec(5.45f, -0.34f, -6.10f),
		0.30f, 0.07f, vec(0, 1, 0), grey, 1);
	/* Canards, angled down. */
	add_plate(m, vec(0.62f, 0.34f, 5.30f), vec(0.62f, 0.34f, 4.30f),
		vec(2.00f, 0.02f, 4.40f), vec(2.00f, 0.02f, 3.90f),
		0.09f, 0.04f, vec(0, 1, 0), grey, 1);
	/* Fin. */
	add_plate(m, vec(0, 0.62f, -2.00f), vec(0, 0.62f, -7.20f),
		vec(0, 4.10f, -6.50f), vec(0, 4.10f, -7.60f),
		0.24f, 0.08f, vec(1, 0, 0), grey, 0);

	add_engine(m, 0, -0.50f, 0.00f, -7.10f, -8.30f, 0.44f);
	add_engine(m, 1, 0.50f, 0.00f, -7.10f, -8.30f, 0.44f);

	add_tube(m, 5.52f, -0.34f, -3.90f, -6.70f, 0.13f, grey_dark); /* wingtip pods */
	add_tube(m, 2.40f, -0.62f, -1.00f, -4.20f, 0.10f, white);     /* missiles */
	add_tube(m, 3.70f, -0.66f, -2.60f, -5.60f, 0.10f, white);
}

/* Panavia Tornado: 16.7 m long. Boxy fuselage with big side intakes,
 * wings mounted on top (swung forward), a very tall fin, large tailplanes. */
static void build_tornado(struct mesh *m) {
	struct station body[] = {
		{ 8.30f, 0.04f, 0.04f, -0.05f, radome },
		{ 7.40f, 0.30f, 0.32f, -0.02f, radome },
		{ 6.30f, 0.52f, 0.56f, 0.02f, radome },
		{ 6.25f, 0.53f, 0.57f, 0.02f, grey },
		{ 4.80f, 0.70f, 0.78f, 0.08f, grey },
		{ 2.60f, 0.86f, 0.90f, 0.06f, grey },
		{ 1.60f, 1.36f, 0.88f, 0.00f, grey },   /* the intakes begin */
		{ -3.00f, 1.36f, 0.90f, 0.00f, grey },
		{ -6.40f, 1.20f, 0.82f, 0.00f, grey },
		{ -7.60f, 1.08f, 0.72f, 0.00f, grey_dark },
	};
	add_loft(m, body, 10, 4.5f, 0, radome, soot);

	/* Two seats, so a long canopy. */
	struct station canopy[] = {
		{ 6.20f, 0.05f, 0.04f, 0.56f, glass },
		{ 5.50f, 0.34f, 0.28f, 0.70f, glass },
		{ 4.20f, 0.42f, 0.42f, 0.84f, glass },
		{ 2.90f, 0.40f, 0.36f, 0.84f, glass },
		{ 1.90f, 0.20f, 0.12f, 0.82f, grey },
	};
	add_loft(m, canopy, 5, 2.0f, 0, glass, grey);

	/* Wings, on the shoulders of the fuselage. */
	add_plate(m, vec(1.25f, 0.62f, 1.30f), vec(1.25f, 0.62f, -1.50f),
		vec(6.90f, 0.50f, -2.10f), vec(6.90f, 0.50f, -3.30f),
		0.30f, 0.09f, vec(0, 1, 0), grey, 1);
	/* All-moving tailplanes, drooped. */
	add_plate(m, vec(1.15f, 0.05f, -4.30f), vec(1.15f, 0.05f, -7.60f),
		vec(3.45f, -0.22f, -7.50f), vec(3.45f, -0.22f, -8.60f),
		0.16f, 0.06f, vec(0, 1, 0), grey, 1);
	/* The tall fin. */
	add_plate(m, vec(0, 0.80f, -2.40f), vec(0, 0.80f, -7.80f),
		vec(0, 5.10f, -7.10f), vec(0, 5.10f, -8.60f),
		0.30f, 0.10f, vec(1, 0, 0), grey, 0);

	add_engine(m, 0, -0.52f, 0.00f, -7.40f, -8.60f, 0.50f);
	add_engine(m, 1, 0.52f, 0.00f, -7.40f, -8.60f, 0.50f);

	add_tube(m, 3.10f, 0.02f, 1.60f, -3.80f, 0.38f, grey);   /* drop tanks */
	add_tube(m, 5.20f, 0.20f, -0.80f, -3.60f, 0.12f, white); /* outboard stores */
}

/* English Electric Lightning: 16.8 m long. A tall, slab-sided fuselage
 * with the nose as the air intake, two engines one above the other,
 * sharply swept wings and a low tailplane. Bare polished metal. */
static void build_lightning(struct mesh *m) {
	struct station body[] = {
		{ 8.20f, 0.66f, 0.72f, 0.05f, bare_metal },
		{ 6.40f, 0.76f, 0.98f, 0.02f, bare_metal },
		{ 2.50f, 0.84f, 1.30f, 0.00f, bare_metal },
		{ -4.00f, 0.86f, 1.36f, 0.00f, bare_metal },
		{ -7.20f, 0.80f, 1.28f, 0.00f, bare_metal },
		{ -7.90f, 0.76f, 1.22f, 0.00f, hot_metal },
	};
	add_loft(m, body, 6, 3.0f, 0, soot, soot);

	/* The pointed centre-body sticking out of the nose intake. */
	struct station cone[] = {
		{ 9.00f, 0.03f, 0.03f, 0.05f, radome },
		{ 8.15f, 0.34f, 0.34f, 0.05f, radome },
	};
	add_loft(m, cone, 2, 2.0f, 0, radome, radome);

	struct station canopy[] = {
		{ 6.50f, 0.05f, 0.04f, 0.92f, glass },
		{ 5.90f, 0.30f, 0.26f, 1.06f, glass },
		{ 5.10f, 0.34f, 0.34f, 1.18f, glass },
		{ 4.20f, 0.28f, 0.22f, 1.20f, bare_metal },
		{ 1.00f, 0.22f, 0.12f, 1.28f, bare_metal }, /* the spine behind it */
	};
	add_loft(m, canopy, 5, 2.0f, 0, glass, bare_metal);

	/* The fuel tank bulge under the belly. */
	struct station belly[] = {
		{ 3.00f, 0.10f, 0.06f, -1.25f, bare_metal },
		{ 1.50f, 0.46f, 0.34f, -1.40f, bare_metal },
		{ -2.00f, 0.50f, 0.38f, -1.42f, bare_metal },
		{ -4.00f, 0.12f, 0.08f, -1.30f, bare_metal },
	};
	add_loft(m, belly, 4, 2.0f, 0, bare_metal, bare_metal);

	/* Wings swept back 60 degrees. */
	add_plate(m, vec(0.84f, 0.00f, 2.80f), vec(0.84f, 0.00f, -2.30f),
		vec(5.30f, -0.10f, -4.60f), vec(5.30f, -0.10f, -5.60f),
		0.26f, 0.08f, vec(0, 1, 0), bare_metal, 1);
	/* Tailplanes, set low. */
	add_plate(m, vec(0.80f, -0.90f, -4.90f), vec(0.80f, -0.90f, -7.40f),
		vec(2.95f, -0.96f, -8.00f), vec(2.95f, -0.96f, -8.80f),
		0.14f, 0.05f, vec(0, 1, 0), bare_metal, 1);
	/* Fin. */
	add_plate(m, vec(0, 1.30f, -3.00f), vec(0, 1.30f, -7.80f),
		vec(0, 4.50f, -7.40f), vec(0, 4.50f, -8.60f),
		0.26f, 0.09f, vec(1, 0, 0), bare_metal, 0);

	add_engine(m, 0, 0, 0.60f, -7.60f, -8.70f, 0.52f);
	add_engine(m, 1, 0, -0.62f, -7.60f, -8.70f, 0.52f);
}

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
	}
}

void models_init(void) {
	if (typhoon.vertex_count != 0) {
		return;
	}
	build_typhoon(&typhoon);
	build_tornado(&tornado);
	build_lightning(&lightning);
	finish_mesh(&typhoon);
	finish_mesh(&tornado);
	finish_mesh(&lightning);
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

void model_draw(int which, const struct model_pose *pose, uint32_t *frame,
		int stride_pixels, int frame_width, int frame_height) {
	const struct mesh *mesh = which == MODEL_TYPHOON ? &typhoon :
		which == MODEL_TORNADO ? &tornado : &lightning;

	/* Step 1 set-up: one 3x3 rotation matrix from the three angles (roll
	 * about the nose-to-tail axis, then pitch, then yaw), converted to
	 * Q14. This handful of floating-point sums happens once per aircraft
	 * per frame; everything after it is whole numbers. */
	float cr = cosf(pose->roll), sr = sinf(pose->roll);
	float cp = cosf(pose->pitch), sp = sinf(pose->pitch);
	float cy = cosf(pose->yaw), sy = sinf(pose->yaw);
	float rotation[9] = {
		cy * cr + sy * sp * sr, -cy * sr + sy * sp * cr, sy * cp,
		cp * sr,                cp * cr,                 -sp,
		-sy * cr + cy * sp * sr, sy * sr + cy * sp * cr, cy * cp,
	};
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
			red = v->r * glow >> 8;
			green = v->g * glow >> 8;
			blue = v->b * glow >> 8;
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
			(int)(scale * mesh->nozzle_radius * 2.2f), (int)(pose->glow * 80));
	}
}
