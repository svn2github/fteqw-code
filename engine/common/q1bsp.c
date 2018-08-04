#include "quakedef.h"

#include "pr_common.h"

#include "shader.h"

extern cvar_t r_decal_noperpendicular;

/*
Decal functions
*/

#define MAXFRAGMENTVERTS 360
int Fragment_ClipPolyToPlane(float *inverts, float *outverts, int incount, float *plane, float planedist)
{
#define C 4
	float dotv[MAXFRAGMENTVERTS+1];
	char keep[MAXFRAGMENTVERTS+1];
#define KEEP_KILL 0
#define KEEP_KEEP 1
#define KEEP_BORDER 2
	int i;
	int outcount = 0;
	int clippedcount = 0;
	float d, *p1, *p2, *out;
#define FRAG_EPSILON (1.0/32) //0.5

	for (i = 0; i < incount; i++)
	{
		dotv[i] = DotProduct((inverts+i*C), plane) - planedist;
		if (dotv[i]<-FRAG_EPSILON)
		{
			keep[i] = KEEP_KILL;
			clippedcount++;
		}
		else if (dotv[i] > FRAG_EPSILON)
			keep[i] = KEEP_KEEP;
		else
			keep[i] = KEEP_BORDER;
	}
	dotv[i] = dotv[0];
	keep[i] = keep[0];

	if (clippedcount == incount)
		return 0;	//all were clipped
	if (clippedcount == 0)
	{	//none were clipped
		for (i = 0; i < incount; i++)
			VectorCopy((inverts+i*C), (outverts+i*C));
		return incount;
	}

	for (i = 0; i < incount; i++)
	{
		p1 = inverts+i*C;
		if (keep[i] == KEEP_BORDER)
		{
			out = outverts+outcount++*C;
			VectorCopy(p1, out);
			continue;
		}
		if (keep[i] == KEEP_KEEP)
		{
			out = outverts+outcount++*C;
			VectorCopy(p1, out);
		}
		if (keep[i+1] == KEEP_BORDER || keep[i] == keep[i+1])
			continue;
		p2 = inverts+((i+1)%incount)*C;
		d = dotv[i] - dotv[i+1];
		if (d)
			d = dotv[i] / d;

		out = outverts+outcount++*C;
		VectorInterpolate(p1, d, p2, out);
	}
	return outcount;
}

//the plane itself must be a vec4_t, but can have other data packed between
size_t Fragment_ClipPlaneToBrush(vecV_t *points, size_t maxpoints, void *planes, size_t planestride, size_t numplanes, vec4_t face)
{
	int p, a;
	vec4_t verts[MAXFRAGMENTVERTS];
	vec4_t verts2[MAXFRAGMENTVERTS];
	vec4_t *cverts;
	int flip;
//	vec3_t d1, d2, n;
	size_t numverts;

	//generate some huge quad/poly aligned with the plane
	vec3_t tmp;
	vec3_t right, forward;
	double t;
	float *plane;
	
//	if (face[2] != 1)
//		return 0;

	t = fabs(face[2]);
	if (t > fabs(face[0]) && t > fabs(face[1]))
		VectorSet(tmp, 1, 0, 0);
	else
		VectorSet(tmp, 0, 0, 1);

	CrossProduct(face, tmp, right);
	VectorNormalize(right);
	CrossProduct(face, right, forward);
	VectorNormalize(forward);

	VectorScale(face, face[3],			verts[0]);
	VectorMA(verts[0], 32767, right,		verts[0]);
	VectorMA(verts[0], 32767, forward,	verts[0]);

	VectorScale(face, face[3],			verts[1]);
	VectorMA(verts[1], 32767, right,		verts[1]);
	VectorMA(verts[1], -32767, forward,	verts[1]);

	VectorScale(face, face[3],			verts[2]);
	VectorMA(verts[2], -32767, right,	verts[2]);
	VectorMA(verts[2], -32767, forward,	verts[2]);

	VectorScale(face, face[3],			verts[3]);
	VectorMA(verts[3], -32767, right,	verts[3]);
	VectorMA(verts[3], 32767, forward,	verts[3]);

	numverts = 4;


	//clip the quad to the various other planes
	flip = 0;
	for (p = 0; p < numplanes; p++)
	{
		plane = (float*)((qbyte*)planes + p*planestride);
		if (plane != face)
		{
			vec3_t norm;
			flip^=1;
			VectorNegate(plane, norm);
			if (flip)
				numverts = Fragment_ClipPolyToPlane((float*)verts, (float*)verts2, numverts, norm, -plane[3]);
			else
				numverts = Fragment_ClipPolyToPlane((float*)verts2, (float*)verts, numverts, norm, -plane[3]);
	
			if (numverts < 3)	//totally clipped.
				return 0;
		}
	}

	if (numverts > maxpoints)
		return 0;

	if (flip)
		cverts = verts2;
	else
		cverts = verts;
	for (p = 0; p < numverts; p++)
	{
		for (a = 0; a < 3; a++)
		{
			float f = cverts[p][a];
			int rounded = floor(f + 0.5);
			//if its within 1/1000th of a qu, just round it.
			if (fabs(f - rounded) < 0.001)
				points[p][a] = rounded;
			else
				points[p][a] = f;
		}
	}

	return numverts;
}

#ifndef SERVERONLY

#define MAXFRAGMENTTRIS 256
vec3_t decalfragmentverts[MAXFRAGMENTTRIS*3];

struct fragmentdecal_s
{
	vec3_t center;

	vec3_t normal;
//	vec3_t tangent1;
//	vec3_t tangent2;

	vec3_t planenorm[6];
	float planedist[6];
	int numplanes;

	vec_t radius;

	//will only appear on surfaces with the matching surfaceflag
	unsigned int surfflagmask;
	unsigned int surfflagmatch;

	void (*callback)(void *ctx, vec3_t *fte_restrict points, size_t numpoints, shader_t *shader);
	void *ctx;
};

//#define SHOWCLIPS
//#define FRAGMENTASTRIANGLES	//works, but produces more fragments.

#ifdef FRAGMENTASTRIANGLES

//if the triangle is clipped away, go recursive if there are tris left.
static void Fragment_ClipTriToPlane(int trinum, float *plane, float planedist, fragmentdecal_t *dec)
{
	float *point[3];
	float dotv[3];

	vec3_t impact1, impact2;
	float t;

	int i, i2, i3;
	int clippedverts = 0;

	for (i = 0; i < 3; i++)
	{
		point[i] = decalfragmentverts[trinum*3+i];
		dotv[i] = DotProduct(point[i], plane)-planedist;
		clippedverts += dotv[i] < 0;
	}

	//if they're all clipped away, scrap the tri
	switch (clippedverts)
	{
	case 0:
		return;	//plane does not clip the triangle.

	case 1:	//split into 3, disregard the clipped vert
		for (i = 0; i < 3; i++)
		{
			if (dotv[i] < 0)
			{	//This is the vertex that's getting clipped.

				if (dotv[i] > -DIST_EPSILON)
					return;	//it's only over the line by a tiny ammount.

				i2 = (i+1)%3;
				i3 = (i+2)%3;

				if (dotv[i2] < DIST_EPSILON)
					return;
				if (dotv[i3] < DIST_EPSILON)
					return;

				//work out where the two lines impact the plane
				t = (dotv[i]) / (dotv[i]-dotv[i2]);
				VectorInterpolate(point[i], t, point[i2], impact1);

				t = (dotv[i]) / (dotv[i]-dotv[i3]);
				VectorInterpolate(point[i], t, point[i3], impact2);

#ifdef SHOWCLIPS
				if (dec->numtris != MAXFRAGMENTTRIS)
				{
					VectorCopy(impact2,					decalfragmentverts[dec->numtris*3+0]);
					VectorCopy(decalfragmentverts[trinum*3+i],	decalfragmentverts[dec->numtris*3+1]);
					VectorCopy(impact1,					decalfragmentverts[dec->numtris*3+2]);
					dec->numtris++;
				}
#endif


				//shrink the tri, putting the impact into the killed vertex.
				VectorCopy(impact2, point[i]);


				if (dec->numtris == MAXFRAGMENTTRIS)
					return;	//:(

				//build the second tri
				VectorCopy(impact1,					decalfragmentverts[dec->numtris*3+0]);
				VectorCopy(decalfragmentverts[trinum*3+i2],	decalfragmentverts[dec->numtris*3+1]);
				VectorCopy(impact2,					decalfragmentverts[dec->numtris*3+2]);
				dec->numtris++;

				return;
			}
		}
		Sys_Error("Fragment_ClipTriToPlane: Clipped vertex not founc\n");
		return;	//can't handle it
	case 2:	//split into 3, disregarding both the clipped.
		for (i = 0; i < 3; i++)
		{
			if (!(dotv[i] < 0))
			{	//This is the vertex that's staying.

				if (dotv[i] < DIST_EPSILON)
					break;	//only just inside

				i2 = (i+1)%3;
				i3 = (i+2)%3;

				//work out where the two lines impact the plane
				t = (dotv[i]) / (dotv[i]-dotv[i2]);
				VectorInterpolate(point[i], t, point[i2], impact1);

				t = (dotv[i]) / (dotv[i]-dotv[i3]);
				VectorInterpolate(point[i], t, point[i3], impact2);

				//shrink the tri, putting the impact into the killed vertex.

#ifdef SHOWCLIPS
				if (dec->numtris != MAXFRAGMENTTRIS)
				{
					VectorCopy(impact1,					decalfragmentverts[dec->numtris*3+0]);
					VectorCopy(point[i2],	decalfragmentverts[dec->numtris*3+1]);
					VectorCopy(point[i3],					decalfragmentverts[dec->numtris*3+2]);
					dec->numtris++;
				}
				if (dec->numtris != MAXFRAGMENTTRIS)
				{
					VectorCopy(impact1,					decalfragmentverts[dec->numtris*3+0]);
					VectorCopy(point[i3],	decalfragmentverts[dec->numtris*3+1]);
					VectorCopy(impact2,					decalfragmentverts[dec->numtris*3+2]);
					dec->numtris++;
				}
#endif

				VectorCopy(impact1, point[i2]);
				VectorCopy(impact2, point[i3]);
				return;
			}
		}
	case 3://scrap it
		//fill the verts with the verts of the last and go recursive (due to the nature of Fragment_ClipTriangle, which doesn't actually know if we clip them away)
#ifndef SHOWCLIPS
		dec->numtris--;
		VectorCopy(decalfragmentverts[dec->numtris*3+0], decalfragmentverts[trinum*3+0]);
		VectorCopy(decalfragmentverts[dec->numtris*3+1], decalfragmentverts[trinum*3+1]);
		VectorCopy(decalfragmentverts[dec->numtris*3+2], decalfragmentverts[trinum*3+2]);
		if (trinum < dec->numtris)
			Fragment_ClipTriToPlane(trinum, plane, planedist, dec);
#endif
		return;
	}
}

static void Fragment_ClipTriangle(fragmentdecal_t *dec, float *a, float *b, float *c)
{
	//emit the triangle, and clip it's fragments.
	int start, i;

	int p;

	if (dec->numtris == MAXFRAGMENTTRIS)
		return;	//:(

	start = dec->numtris;

	VectorCopy(a, decalfragmentverts[dec->numtris*3+0]);
	VectorCopy(b, decalfragmentverts[dec->numtris*3+1]);
	VectorCopy(c, decalfragmentverts[dec->numtris*3+2]);
	dec->numtris++;

	//clip all the fragments to all of the planes.
	//This will produce a quad if the source triangle was big enough.

	for (p = 0; p < 6; p++)
	{
		for (i = start; i < dec->numtris; i++)
			Fragment_ClipTriToPlane(i, dec->planenorm[p], dec->plantdist[p], dec);
	}
}

#else

void Fragment_ClipPoly(fragmentdecal_t *dec, int numverts, float *inverts, shader_t *surfshader)
{
	//emit the triangle, and clip it's fragments.
	int p;
	float verts[MAXFRAGMENTVERTS*C];
	float verts2[MAXFRAGMENTVERTS*C];
	float *cverts;
	int flip;
	vec3_t d1, d2, n;
	size_t numtris;

	if (numverts > MAXFRAGMENTTRIS)
		return;

	if (r_decal_noperpendicular.ival)
	{
		VectorSubtract(inverts+C*1, inverts+C*0, d1);
		for (p = 2; ; p++)
		{
			if (p >= numverts)
				return;
			VectorSubtract(inverts+C*p, inverts+C*0, d2);
			CrossProduct(d1, d2, n);
			if (DotProduct(n,n)>.1)
				break;
		}
		VectorNormalizeFast(n);
		if (DotProduct(n, dec->normal) < 0.1)
			return;	//faces too far way from the normal
	}

	//clip to the first plane specially, so we don't have extra copys
	numverts = Fragment_ClipPolyToPlane(inverts, verts, numverts, dec->planenorm[0], dec->planedist[0]);

	//clip the triangle to the 6 planes.
	flip = 0;
	for (p = 1; p < dec->numplanes; p++)
	{
		flip^=1;
		if (flip)
			numverts = Fragment_ClipPolyToPlane(verts, verts2, numverts, dec->planenorm[p], dec->planedist[p]);
		else
			numverts = Fragment_ClipPolyToPlane(verts2, verts, numverts, dec->planenorm[p], dec->planedist[p]);

		if (numverts < 3)	//totally clipped.
			return;
	}

	if (flip)
		cverts = verts2;
	else
		cverts = verts;

	//decompose the resultant polygon into triangles.

	numtris = 0;
	while(numverts-->2)
	{
		if (numtris == MAXFRAGMENTTRIS)
		{
			dec->callback(dec->ctx, decalfragmentverts, numtris, NULL);
			numtris = 0;
			break;
		}

		VectorCopy((cverts+C*0),			decalfragmentverts[numtris*3+0]);
		VectorCopy((cverts+C*(numverts-1)),	decalfragmentverts[numtris*3+1]);
		VectorCopy((cverts+C*numverts),		decalfragmentverts[numtris*3+2]);
		numtris++;
	}
	if (numtris)
		dec->callback(dec->ctx, decalfragmentverts, numtris, surfshader);
}

#endif

//this could be inlined, but I'm lazy.
static void Fragment_Mesh (fragmentdecal_t *dec, mesh_t *mesh, mtexinfo_t *texinfo)
{
	int i;
	vecV_t verts[3];
	shader_t *surfshader = texinfo->texture->shader;

	if ((surfshader->flags & SHADER_NOMARKS) || !mesh)
		return;

	if (dec->surfflagmask)
	{
		if ((texinfo->flags & dec->surfflagmask) != dec->surfflagmatch)
			return;
	}

	/*if its a triangle fan/poly/quad then we can just submit the entire thing without generating extra fragments*/
	if (mesh->istrifan)
	{
		Fragment_ClipPoly(dec, mesh->numvertexes, mesh->xyz_array[0], surfshader);
		return;
	}

	//Fixme: optimise q3 patches (quad strips with bends between each strip)

	/*otherwise it goes in and out in weird places*/
	for (i = 0; i < mesh->numindexes; i+=3)
	{
		VectorCopy(mesh->xyz_array[mesh->indexes[i+0]], verts[0]);
		VectorCopy(mesh->xyz_array[mesh->indexes[i+1]], verts[1]);
		VectorCopy(mesh->xyz_array[mesh->indexes[i+2]], verts[2]);
		Fragment_ClipPoly(dec, 3, verts[0], surfshader);
	}
}

#ifdef Q1BSPS
static void Q1BSP_ClipDecalToNodes (model_t *mod, fragmentdecal_t *dec, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int			i;

	if (node->contents < 0)
		return;

	splitplane = node->plane;
	dist = DotProduct (dec->center, splitplane->normal) - splitplane->dist;

	if (dist > dec->radius)
	{
		Q1BSP_ClipDecalToNodes (mod, dec, node->children[0]);
		return;
	}
	if (dist < -dec->radius)
	{
		Q1BSP_ClipDecalToNodes (mod, dec, node->children[1]);
		return;
	}

// mark the polygons
	surf = mod->surfaces + node->firstsurface;
	if (r_decal_noperpendicular.ival)
	{
		for (i=0 ; i<node->numsurfaces ; i++, surf++)
		{
			if (surf->flags & SURF_PLANEBACK)
			{
				if (-DotProduct(surf->plane->normal, dec->normal) > -0.5)
					continue;
			}
			else
			{
				if (DotProduct(surf->plane->normal, dec->normal) > -0.5)
					continue;
			}
			Fragment_Mesh(dec, surf->mesh, surf->texinfo);
		}
	}
	else
	{
		for (i=0 ; i<node->numsurfaces ; i++, surf++)
			Fragment_Mesh(dec, surf->mesh, surf->texinfo);
	}

	Q1BSP_ClipDecalToNodes (mod, dec, node->children[0]);
	Q1BSP_ClipDecalToNodes (mod, dec, node->children[1]);
}
#endif

#ifdef RTLIGHTS
extern int sh_shadowframe;
#else
static int sh_shadowframe;
#endif
#ifdef Q3BSPS
static void Q3BSP_ClipDecalToNodes (fragmentdecal_t *dec, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	**msurf;
	msurface_t	*surf;
	mleaf_t		*leaf;
	int			i;

	if (node->contents != -1)
	{
		leaf = (mleaf_t *)node;
	// mark the polygons
		msurf = leaf->firstmarksurface;
		for (i=0 ; i<leaf->nummarksurfaces ; i++, msurf++)
		{
			surf = *msurf;

			//only check each surface once. it can appear in multiple leafs.
			if (surf->shadowframe == sh_shadowframe)
				continue;
			surf->shadowframe = sh_shadowframe;

			Fragment_Mesh(dec, surf->mesh, surf->texinfo);
		}
		return;
	}

	splitplane = node->plane;
	dist = DotProduct (dec->center, splitplane->normal) - splitplane->dist;

	if (dist > dec->radius)
	{
		Q3BSP_ClipDecalToNodes (dec, node->children[0]);
		return;
	}
	if (dist < -dec->radius)
	{
		Q3BSP_ClipDecalToNodes (dec, node->children[1]);
		return;
	}
	Q3BSP_ClipDecalToNodes (dec, node->children[0]);
	Q3BSP_ClipDecalToNodes (dec, node->children[1]);
}
#endif

void Mod_ClipDecal(struct model_s *mod, vec3_t center, vec3_t normal, vec3_t tangent1, vec3_t tangent2, float size, unsigned int surfflagmask, unsigned int surfflagmatch, void (*callback)(void *ctx, vec3_t *fte_restrict points, size_t numpoints, shader_t *shader), void *ctx)
{	//quad marks a full, independant quad
	int p;
	float r;
	fragmentdecal_t dec;

	VectorCopy(center, dec.center);
	VectorCopy(normal, dec.normal);
	dec.radius = 0;
	dec.callback = callback;
	dec.ctx = ctx;
	dec.surfflagmask = surfflagmask;
	dec.surfflagmatch = surfflagmatch;

	VectorCopy(tangent1,	dec.planenorm[0]);
	VectorNegate(tangent1,	dec.planenorm[1]);
	VectorCopy(tangent2,	dec.planenorm[2]);
	VectorNegate(tangent2,	dec.planenorm[3]);
	VectorCopy(dec.normal,		dec.planenorm[4]);
	VectorNegate(dec.normal,	dec.planenorm[5]);
	for (p = 0; p < 6; p++)
	{
		r = sqrt(DotProduct(dec.planenorm[p], dec.planenorm[p]));
		VectorScale(dec.planenorm[p], 1/r, dec.planenorm[p]);
		r*= size/2;
		if (r > dec.radius)
			dec.radius = r;
		dec.planedist[p] = -(r - DotProduct(dec.center, dec.planenorm[p]));
	}
	dec.numplanes = 6;

	sh_shadowframe++;

	if (!mod || mod->loadstate != MLS_LOADED || mod->type != mod_brush)
	{
	}
#ifdef Q1BSPS
	else if (mod->fromgame == fg_quake || mod->fromgame == fg_halflife)
		Q1BSP_ClipDecalToNodes(mod, &dec, mod->rootnode);
#endif
#ifdef Q3BSPS
	else if (cl.worldmodel->fromgame == fg_quake3)
		Q3BSP_ClipDecalToNodes(&dec, mod->rootnode);
#endif

#ifdef TERRAIN
	if (cl.worldmodel && cl.worldmodel->terrain)
		Terrain_ClipDecal(&dec, center, dec.radius, mod);
#endif
}
#endif

/*
Decal functions

============================================================================

Physics functions (common)
*/
#ifdef Q1BSPS

void Q1BSP_CheckHullNodes(hull_t *hull)
{
	int num, c;
	mclipnode_t	*node;
	for (num = hull->firstclipnode; num < hull->lastclipnode; num++)
	{
		node = hull->clipnodes + num;
		for (c = 0; c < 2; c++)
			if (node->children[c] >= 0)
				if (node->children[c] < hull->firstclipnode || node->children[c] > hull->lastclipnode)
					Sys_Error ("Q1BSP_CheckHull: bad node number");

	}
}


static int Q1_ModelPointContents (mnode_t *node, vec3_t p)
{
	float d;
	mplane_t *plane;
	while(node->contents >= 0)
	{
		plane = node->plane;
		if (plane->type < 3)
			d = p[plane->type] - plane->dist;
		else
			d = DotProduct(plane->normal, p) - plane->dist;
		node = node->children[d<0];
	}
	return node->contents;
}

#endif//Q1BSPS
/*
==================
SV_HullPointContents

==================
*/
static int Q1_HullPointContents (hull_t *hull, int num, vec3_t p)
{
	float		d;
	mclipnode_t	*node;
	mplane_t	*plane;

	while (num >= 0)
	{
		node = hull->clipnodes + num;
		plane = hull->planes + node->planenum;

		if (plane->type < 3)
			d = p[plane->type] - plane->dist;
		else
			d = DotProduct (plane->normal, p) - plane->dist;
		if (d < 0)
			num = node->children[1];
		else
			num = node->children[0];
	}

	return num;
}

#define	DIST_EPSILON	(0.03125)
#if 1
enum
{
	rht_solid,
	rht_empty,
	rht_impact
};
struct rhtctx_s
{
	vec3_t start, end;
	mclipnode_t	*clipnodes;
	mplane_t	*planes;
};
static int Q1BSP_RecursiveHullTrace (struct rhtctx_s *ctx, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace)
{
	mclipnode_t	*node;
	mplane_t	*plane;
	float		t1, t2;
	vec3_t		mid;
	int			side;
	float		midf;
	int rht;

reenter:

	if (num < 0)
	{
		/*hit a leaf*/
		if (num == Q1CONTENTS_SOLID)
		{
			if (trace->allsolid)
				trace->startsolid = true;
			return rht_solid;
		}
		else
		{
			trace->allsolid = false;
			if (num == Q1CONTENTS_EMPTY)
				trace->inopen = true;
			else
				trace->inwater = true;
			return rht_empty;
		}
	}

	/*its a node*/

	/*get the node info*/
	node = ctx->clipnodes + num;
	plane = ctx->planes + node->planenum;

	if (plane->type < 3)
	{
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
	}
	else
	{
		t1 = DotProduct (plane->normal, p1) - plane->dist;
		t2 = DotProduct (plane->normal, p2) - plane->dist;
	}

	/*if its completely on one side, resume on that side*/
	if (t1 >= 0 && t2 >= 0)
	{
		//return Q1BSP_RecursiveHullTrace (hull, node->children[0], p1f, p2f, p1, p2, trace);
		num = node->children[0];
		goto reenter;
	}
	if (t1 < 0 && t2 < 0)
	{
		//return Q1BSP_RecursiveHullTrace (hull, node->children[1], p1f, p2f, p1, p2, trace);
		num = node->children[1];
		goto reenter;
	}

	if (plane->type < 3)
	{
		t1 = ctx->start[plane->type] - plane->dist;
		t2 = ctx->end[plane->type] - plane->dist;
	}
	else
	{
		t1 = DotProduct (plane->normal, ctx->start) - plane->dist;
		t2 = DotProduct (plane->normal, ctx->end) - plane->dist;
	}

	side = t1 < 0;

	midf = t1 / (t1 - t2);
	if (midf < p1f) midf = p1f;
	if (midf > p2f) midf = p2f;
	VectorInterpolate(ctx->start, midf, ctx->end, mid);

	rht = Q1BSP_RecursiveHullTrace(ctx, node->children[side], p1f, midf, p1, mid, trace);
	if (rht != rht_empty && !trace->allsolid)
		return rht;
	rht = Q1BSP_RecursiveHullTrace(ctx, node->children[side^1], midf, p2f, mid, p2, trace);
	if (rht != rht_solid)
		return rht;

	if (side)
	{
		/*we impacted the back of the node, so flip the plane*/
		trace->plane.dist = -plane->dist;
		VectorNegate(plane->normal, trace->plane.normal);
		midf = (t1 + DIST_EPSILON) / (t1 - t2);
	}
	else
	{
		/*we impacted the front of the node*/
		trace->plane.dist = plane->dist;
		VectorCopy(plane->normal, trace->plane.normal);
		midf = (t1 - DIST_EPSILON) / (t1 - t2);
	}

	t1 = DotProduct (trace->plane.normal, ctx->start) - trace->plane.dist;
	t2 = DotProduct (trace->plane.normal, ctx->end) - trace->plane.dist;
	midf = (t1 - DIST_EPSILON) / (t1 - t2);

	midf = bound(0, midf, 1);
	trace->fraction = midf;
	VectorCopy (mid, trace->endpos);
	VectorInterpolate(ctx->start, midf, ctx->end, trace->endpos);

	return rht_impact;
}

qboolean Q1BSP_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace)
{
	if (VectorEquals(p1, p2))
	{
		/*points cannot cross planes, so do it faster*/
		switch(Q1_HullPointContents(hull, num, p1))
		{
		case Q1CONTENTS_SOLID:
			trace->startsolid = true;
			break;
		case Q1CONTENTS_EMPTY:
			trace->allsolid = false;
			trace->inopen = true;
			break;
		default:
			trace->allsolid = false;
			trace->inwater = true;
			break;
		}
		return true;
	}
	else
	{
		struct rhtctx_s ctx;
		VectorCopy(p1, ctx.start);
		VectorCopy(p2, ctx.end);
		ctx.clipnodes = hull->clipnodes;
		ctx.planes = hull->planes;
		return Q1BSP_RecursiveHullTrace(&ctx, num, p1f, p2f, p1, p2, trace) != rht_impact;
	}
}

#else
qboolean Q1BSP_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace)
{
	mclipnode_t	*node;
	mplane_t	*plane;
	float		t1, t2;
	float		frac;
	int			i;
	vec3_t		mid;
	int			side;
	float		midf;

// check for empty
	if (num < 0)
	{
		if (num != Q1CONTENTS_SOLID)
		{
			trace->allsolid = false;
			if (num == Q1CONTENTS_EMPTY)
				trace->inopen = true;
			else
				trace->inwater = true;
		}
		else
			trace->startsolid = true;
		return true;		// empty
	}

//
// find the point distances
//
	node = hull->clipnodes + num;
	plane = hull->planes + node->planenum;

	if (plane->type < 3)
	{
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
	}
	else
	{
		t1 = DotProduct (plane->normal, p1) - plane->dist;
		t2 = DotProduct (plane->normal, p2) - plane->dist;
	}

#if 1
	if (t1 >= 0 && t2 >= 0)
		return Q1BSP_RecursiveHullCheck (hull, node->children[0], p1f, p2f, p1, p2, trace);
	if (t1 < 0 && t2 < 0)
		return Q1BSP_RecursiveHullCheck (hull, node->children[1], p1f, p2f, p1, p2, trace);
#else
	if ( (t1 >= DIST_EPSILON && t2 >= DIST_EPSILON) || (t2 > t1 && t1 >= 0) )
		return Q1BSP_RecursiveHullCheck (hull, node->children[0], p1f, p2f, p1, p2, trace);
	if ( (t1 <= -DIST_EPSILON && t2 <= -DIST_EPSILON) || (t2 < t1 && t1 <= 0) )
		return Q1BSP_RecursiveHullCheck (hull, node->children[1], p1f, p2f, p1, p2, trace);
#endif

// put the crosspoint DIST_EPSILON pixels on the near side
	if (t1 < 0)
		frac = (t1 + DIST_EPSILON)/(t1-t2);
	else
		frac = (t1 - DIST_EPSILON)/(t1-t2);
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;

	midf = p1f + (p2f - p1f)*frac;
	for (i=0 ; i<3 ; i++)
		mid[i] = p1[i] + frac*(p2[i] - p1[i]);

	side = (t1 < 0);

// move up to the node
	if (!Q1BSP_RecursiveHullCheck (hull, node->children[side], p1f, midf, p1, mid, trace) )
		return false;

#ifdef PARANOID
	if (Q1BSP_RecursiveHullCheck (sv_hullmodel, mid, node->children[side])
	== Q1CONTENTS_SOLID)
	{
		Con_Printf ("mid PointInHullSolid\n");
		return false;
	}
#endif

	if (Q1_HullPointContents (hull, node->children[side^1], mid)
	!= Q1CONTENTS_SOLID)
// go past the node
		return Q1BSP_RecursiveHullCheck (hull, node->children[side^1], midf, p2f, mid, p2, trace);

	if (trace->allsolid)
		return false;		// never got out of the solid area

//==================
// the other side of the node is solid, this is the impact point
//==================
	if (!side)
	{
		VectorCopy (plane->normal, trace->plane.normal);
		trace->plane.dist = plane->dist;
	}
	else
	{
		VectorNegate (plane->normal, trace->plane.normal);
		trace->plane.dist = -plane->dist;
	}

	while (Q1_HullPointContents (hull, hull->firstclipnode, mid)
	== Q1CONTENTS_SOLID)
	{ // shouldn't really happen, but does occasionally
		if (!(frac < 10000000) && !(frac > -10000000))
		{
			trace->fraction = 0;
			VectorClear (trace->endpos);
			Con_Printf ("nan in traceline\n");
			return false;
		}
		frac -= 0.1;
		if (frac < 0)
		{
			trace->fraction = midf;
			VectorCopy (mid, trace->endpos);
			Con_DPrintf ("backup past 0\n");
			return false;
		}
		midf = p1f + (p2f - p1f)*frac;
		for (i=0 ; i<3 ; i++)
			mid[i] = p1[i] + frac*(p2[i] - p1[i]);
	}

	trace->fraction = midf;
	VectorCopy (mid, trace->endpos);

	return false;
}
#endif

#ifdef Q1BSPS

/*
the bsp tree we're walking through is the renderable hull
we need to trace a box through the world.
by its very nature, this will reach more nodes than we really want, and as we can follow a node sideways, the underlying bsp structure is no longer 100% reliable (meaning we cross planes that are entirely to one side, and follow its children too)
so all contents and solidity must come from the brushes and ONLY the brushes.
*/
struct traceinfo_s
{
	unsigned int solidcontents;
	trace_t trace;

	qboolean capsule;
	float radius;
	/*set even for sphere traces (used for bbox tests)*/
	vec3_t mins;
	vec3_t maxs;

	vec3_t start;
	vec3_t end;

	vec3_t	up;
	vec3_t	capsulesize;
	vec3_t	extents;
};

static void Q1BSP_ClipToBrushes(struct traceinfo_s *traceinfo, mbrush_t *brush)
{
	struct mbrushplane_s *plane;
	struct mbrushplane_s *enterplane;
	int i, j;
	vec3_t ofs;
	qboolean startout, endout;
	float d1,d2,dist,enterdist=0;
	float f, enterfrac, exitfrac;

	for (; brush; brush = brush->next)
	{
		/*ignore if its not solid to us*/
		if (!(traceinfo->solidcontents & brush->contents))
			continue;

		startout = false;
		endout = false;
		enterplane= NULL;
		enterfrac = -1;
		exitfrac = 10;
		for (i = brush->numplanes, plane = brush->planes; i; i--, plane++)
		{
			/*calculate the distance based upon the shape of the object we're tracing for*/
			if (traceinfo->capsule)
			{
				dist = DotProduct(traceinfo->up, plane->normal);
				dist = dist*(traceinfo->capsulesize[(dist<0)?1:2]) - traceinfo->capsulesize[0];
				dist = plane->dist - dist;

				//dist = plane->dist + traceinfo->radius;
			}
			else
			{
				for (j=0 ; j<3 ; j++)
				{
					if (plane->normal[j] < 0)
						ofs[j] = traceinfo->maxs[j];
					else
						ofs[j] = traceinfo->mins[j];
				}
				dist = DotProduct (ofs, plane->normal);
				dist = plane->dist - dist;
			}

			d1 = DotProduct (traceinfo->start, plane->normal) - dist;
			d2 = DotProduct (traceinfo->end, plane->normal) - dist;

			if (d1 >= 0)
				startout = true;
			if (d2 > 0)
				endout = true;

			//if we're fully outside any plane, then we cannot possibly enter the brush, skip to the next one
			if (d1 > 0 && d2 >= 0)
				goto nextbrush;

			//if we're fully inside the plane, then whatever is happening is not relevent for this plane
			if (d1 < 0 && d2 <= 0)
				continue;

			f = d1 / (d1-d2);
			if (d1 > d2)
			{
				//entered the brush. favour the furthest fraction to avoid extended edges (yay for convex shapes)
				if (enterfrac < f)
				{
					enterfrac = f;
					enterplane = plane;
					enterdist = dist;
				}
			}
			else
			{
				//left the brush, favour the nearest plane (smallest frac)
				if (exitfrac > f)
				{
					exitfrac = f;
				}
			}
		}

		if (!startout)
		{
			traceinfo->trace.startsolid = true;
			if (!endout)
				traceinfo->trace.allsolid = true;
			traceinfo->trace.contents |= brush->contents;
			return;
		}
		if (enterfrac != -1 && enterfrac < exitfrac)
		{
			//impact!
			if (enterfrac < traceinfo->trace.fraction)
			{
				traceinfo->trace.fraction = enterfrac;
				traceinfo->trace.plane.dist = enterdist;
				VectorCopy(enterplane->normal, traceinfo->trace.plane.normal);
				traceinfo->trace.contents = brush->contents;
			}
		}
nextbrush:
		;
	}
}
static void Q1BSP_InsertBrush(mnode_t *node, mbrush_t *brush, vec3_t bmins, vec3_t bmaxs)
{
	vec3_t nearp, farp;
	float nd, fd;
	int i;
	while(1)
	{
		if (node->contents < 0) /*leaf, so no smaller node to put it in (I'd be surprised if it got this far)*/
		{
			brush->next = node->brushes;
			node->brushes = brush;
			return;
		}

		for (i = 0; i < 3; i++)
		{
			if (node->plane->normal[i] > 0)
			{
				nearp[i] = bmins[i];
				farp[i] = bmaxs[i];
			}
			else
			{
				nearp[i] = bmaxs[i];
				farp[i] = bmins[i];
			}
		}

		nd = DotProduct(node->plane->normal, nearp) - node->plane->dist;
		fd = DotProduct(node->plane->normal, farp) - node->plane->dist;

		/*if its fully on either side, continue walking*/
		if (nd < 0 && fd < 0)
			node = node->children[1];
		else if (nd > 0 && fd > 0)
			node = node->children[0];
		else
		{
			/*plane crosses bbox, so insert here*/
			brush->next = node->brushes;
			node->brushes = brush;
			return;
		}
	}
}
static void Q1BSP_RecursiveBrushCheck (struct traceinfo_s *traceinfo, mnode_t *node, float p1f, float p2f, vec3_t p1, vec3_t p2)
{
	mplane_t	*plane;
	float		t1, t2;
	float		frac;
	int			i;
	vec3_t		mid;
	int			side;
	float		midf;
	float		offset;

	if (node->brushes)
	{
		Q1BSP_ClipToBrushes(traceinfo, node->brushes);
	}

	if (traceinfo->trace.fraction < p1f)
	{
		//already hit something closer than this node
		return;
	}

	if (node->contents < 0)
	{
		//we're in a leaf
		return;
	}

//
// find the point distances
//
	plane = node->plane;

	if (plane->type < 3)
	{
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
		if (plane->normal[plane->type] < 0)
			offset = -traceinfo->mins[plane->type];
		else
			offset = traceinfo->maxs[plane->type];
	}
	else
	{
		t1 = DotProduct (plane->normal, p1) - plane->dist;
		t2 = DotProduct (plane->normal, p2) - plane->dist;
		offset = 0;
		for (i = 0; i < 3; i++)
		{
			if (plane->normal[i] < 0)
				offset += plane->normal[i] * -traceinfo->mins[i];
			else
				offset += plane->normal[i] * traceinfo->maxs[i];
		}
	}

	/*if we're fully on one side of the trace, go only down that side*/
	if (t1 >= offset && t2 >= offset)
	{
		Q1BSP_RecursiveBrushCheck (traceinfo, node->children[0], p1f, p2f, p1, p2);
		return;
	}
	if (t1 < -offset && t2 < -offset)
	{
		Q1BSP_RecursiveBrushCheck (traceinfo, node->children[1], p1f, p2f, p1, p2);
		return;
	}

// put the crosspoint DIST_EPSILON pixels on the near side
	if (t1 == t2)
	{
		side = 0;
		frac = 0;
	}
	else if (t1 < 0)
	{
		frac = (t1 + DIST_EPSILON)/(t1-t2);
		side = 1;
	}
	else
	{
		frac = (t1 - DIST_EPSILON)/(t1-t2);
		side = 0;
	}
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;

	midf = p1f + (p2f - p1f)*frac;
	for (i=0 ; i<3 ; i++)
		mid[i] = p1[i] + frac*(p2[i] - p1[i]);

// move up to the node
	Q1BSP_RecursiveBrushCheck (traceinfo, node->children[side], p1f, midf, p1, mid);

// go past the node
	Q1BSP_RecursiveBrushCheck (traceinfo, node->children[side^1], midf, p2f, mid, p2);
}
#endif	//Q1BSPS

static unsigned int Q1BSP_TranslateContents(int contents)
{
	switch(contents)
	{
	case Q1CONTENTS_EMPTY:
		return FTECONTENTS_EMPTY;
	case Q1CONTENTS_SOLID:
		return FTECONTENTS_SOLID;
	case Q1CONTENTS_WATER:
		return FTECONTENTS_WATER;
	case Q1CONTENTS_SLIME:
		return FTECONTENTS_SLIME;
	case Q1CONTENTS_LAVA:
		return FTECONTENTS_LAVA;
	case Q1CONTENTS_SKY:
		return FTECONTENTS_SKY;
	case Q1CONTENTS_LADDER:
		return FTECONTENTS_LADDER;
	case Q1CONTENTS_CLIP:
		return FTECONTENTS_PLAYERCLIP;
	case Q1CONTENTS_TRANS:
		return FTECONTENTS_SOLID;

	//q2 is better than nothing, right?
	case Q1CONTENTS_FLOW_1:
		return Q2CONTENTS_CURRENT_0;
	case Q1CONTENTS_FLOW_2:
		return Q2CONTENTS_CURRENT_90;
	case Q1CONTENTS_FLOW_3:
		return Q2CONTENTS_CURRENT_180;
	case Q1CONTENTS_FLOW_4:
		return Q2CONTENTS_CURRENT_270;
	case Q1CONTENTS_FLOW_5:
		return Q2CONTENTS_CURRENT_UP;
	case Q1CONTENTS_FLOW_6:
		return Q2CONTENTS_CURRENT_DOWN;

	default:
		Con_Printf("Q1BSP_TranslateContents: Unknown contents type - %i", contents);
		return FTECONTENTS_SOLID;
	}
}

int Q1BSP_HullPointContents(hull_t *hull, vec3_t p)
{
	return Q1BSP_TranslateContents(Q1_HullPointContents(hull, hull->firstclipnode, p));
}

#ifdef Q1BSPS
unsigned int Q1BSP_PointContents(model_t *model, vec3_t axis[3], vec3_t point)
{
	int contents;
	if (axis)
	{
		vec3_t transformed;
		transformed[0] = DotProduct(point, axis[0]);
		transformed[1] = DotProduct(point, axis[1]);
		transformed[2] = DotProduct(point, axis[2]);
		return Q1BSP_PointContents(model, NULL, transformed);
	}
	else
	{
		if (!model->firstmodelsurface)
		{
			contents = Q1BSP_TranslateContents(Q1_ModelPointContents(model->nodes, point));
		}
		else
			contents = Q1BSP_HullPointContents(&model->hulls[0], point);
	}
#ifdef TERRAIN
	if (model->terrain)
		contents |= Heightmap_PointContents(model, NULL, point);
#endif
	return contents;
}

void Q1BSP_LoadBrushes(model_t *model, bspx_header_t *bspx, void *mod_base)
{
	struct {
		unsigned int ver;
		unsigned int modelnum;
		unsigned int numbrushes;
		unsigned int numplanes;
	} *permodel;
	struct {
		float mins[3];
		float maxs[3];
		signed short contents;
		unsigned short numplanes;
	} *perbrush;
	/*
	Note to implementors:
	a pointy brush with angles pointier than 90 degrees will extend further than any adjacent brush, thus creating invisible walls with larger expansions.
	the engine inserts 6 axial planes acording to the bbox, thus the qbsp need not write any axial planes
	note that doing it this way probably isn't good if you want to query textures...
	*/
	struct {
		vec3_t normal;
		float dist;
	} *perplane;

	static vec3_t axis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	int br, pl, remainingplanes;
	mbrush_t *brush;
	mnode_t *rootnode;
	unsigned int lumpsizeremaining;

	model->engineflags &= ~MDLF_HASBRUSHES;

	permodel = BSPX_FindLump(bspx, mod_base, "BRUSHLIST", &lumpsizeremaining);
	if (!permodel)
		return;

	while (lumpsizeremaining)
	{
		if (lumpsizeremaining < sizeof(*permodel))
			return;
		permodel->ver = LittleLong(permodel->ver);
		permodel->modelnum = LittleLong(permodel->modelnum);
		permodel->numbrushes = LittleLong(permodel->numbrushes);
		permodel->numplanes = LittleLong(permodel->numplanes);
		if (permodel->ver != 1 || lumpsizeremaining < sizeof(*permodel) + permodel->numbrushes*sizeof(*perbrush) + permodel->numplanes*sizeof(*perplane))
			return;

		//find the correct rootnode for the submodel (submodels are not set up yet).
		rootnode = model->nodes;
		if (permodel->modelnum > model->numsubmodels)
			return;
		rootnode += model->submodels[permodel->modelnum].headnode[0];

		brush = ZG_Malloc(&model->memgroup, (sizeof(*brush) - sizeof(brush->planes[0]))*permodel->numbrushes + sizeof(brush->planes[0])*(permodel->numbrushes*6+permodel->numplanes));
		remainingplanes = permodel->numplanes;
		perbrush = (void*)(permodel+1);
		for (br = 0; br < permodel->numbrushes; br++)
		{
			/*byteswap it all in place*/
			perbrush->mins[0] = LittleFloat(perbrush->mins[0]);
			perbrush->mins[1] = LittleFloat(perbrush->mins[1]);
			perbrush->mins[2] = LittleFloat(perbrush->mins[2]);
			perbrush->maxs[0] = LittleFloat(perbrush->maxs[0]);
			perbrush->maxs[1] = LittleFloat(perbrush->maxs[1]);
			perbrush->maxs[2] = LittleFloat(perbrush->maxs[2]);
			perbrush->contents = LittleShort(perbrush->contents);
			perbrush->numplanes = LittleShort(perbrush->numplanes);

			/*make sure planes don't overflow*/
			if (perbrush->numplanes > remainingplanes)
				return;
			remainingplanes-=perbrush->numplanes;

			/*set up the mbrush from the file*/
			brush->contents = Q1BSP_TranslateContents(perbrush->contents);
			brush->numplanes = perbrush->numplanes;
			for (pl = 0, perplane = (void*)(perbrush+1); pl < perbrush->numplanes; pl++, perplane++)
			{
				brush->planes[pl].normal[0] = LittleFloat(perplane->normal[0]);
				brush->planes[pl].normal[1] = LittleFloat(perplane->normal[1]);
				brush->planes[pl].normal[2] = LittleFloat(perplane->normal[2]);
				brush->planes[pl].dist = LittleFloat(perplane->dist);
			}

			/*and add axial planes acording to the brush's bbox*/
			for (pl = 0; pl < 3; pl++)
			{
				VectorCopy(axis[pl], brush->planes[brush->numplanes].normal);
				brush->planes[brush->numplanes].dist = perbrush->maxs[pl];
				brush->numplanes++;
			}
			for (pl = 0; pl < 3; pl++)
			{
				VectorNegate(axis[pl], brush->planes[brush->numplanes].normal);
				brush->planes[brush->numplanes].dist = -perbrush->mins[pl];
				brush->numplanes++;
			}
			
			/*link it in to the bsp tree*/
			Q1BSP_InsertBrush(rootnode, brush, perbrush->mins, perbrush->maxs);

			/*set up for the next brush*/
			brush = (void*)&brush->planes[brush->numplanes];
			perbrush = (void*)perplane;
		}
		/*move on to the next model*/
		lumpsizeremaining -= sizeof(*permodel) + permodel->numbrushes*sizeof(*perbrush) + permodel->numplanes*sizeof(*perplane);
		permodel = (void*)((char*)permodel + sizeof(*permodel) + permodel->numbrushes*sizeof(*perbrush) + permodel->numplanes*sizeof(*perplane));
	}
	/*parsing was successful! flag it as okay*/
	model->engineflags |= MDLF_HASBRUSHES;
}

hull_t *Q1BSP_ChooseHull(model_t *model, int forcehullnum, vec3_t mins, vec3_t maxs, vec3_t offset)
{
	hull_t *hull;
	vec3_t size;
	VectorSubtract (maxs, mins, size);
	if (forcehullnum >= 1 && forcehullnum <= MAX_MAP_HULLSM && model->hulls[forcehullnum-1].available)
		hull = &model->hulls[forcehullnum-1];
	else
	{
#ifdef HEXEN2
		if (model->hulls[5].available)
		{	//choose based on hexen2 sizes.

			if (size[0] < 3) // Point
				hull = &model->hulls[0];
			else if (size[0] <= 8.1 && model->hulls[4].available)
				hull = &model->hulls[4];	//Pentacles
			else if (size[0] <= 32.1 && size[2] <= 28.1)  // Half Player
				hull = &model->hulls[3];
			else if (size[0] <= 32.1)  // Full Player
				hull = &model->hulls[1];
			else // Golumn
				hull = &model->hulls[5];
		}
		else
#endif
		{
			if (size[0] < 3 || !model->hulls[1].available)
				hull = &model->hulls[0];
			else if (size[0] <= 32.1 || !model->hulls[2].available)
			{
				if (size[2] < 54.1 && model->hulls[3].available)
					hull = &model->hulls[3]; // 32x32x36 (half-life's crouch)
				else if (model->hulls[1].available)
					hull = &model->hulls[1];
				else
					hull = &model->hulls[0];
			}
			else
				hull = &model->hulls[2];
		}
	}

	VectorSubtract (hull->clip_mins, mins, offset);
	return hull;
}
qboolean Q1BSP_Trace(model_t *model, int forcehullnum, framestate_t *framestate, vec3_t axis[3], vec3_t start, vec3_t end, vec3_t mins, vec3_t maxs, qboolean capsule, unsigned int hitcontentsmask, trace_t *trace)
{
	hull_t *hull;
	vec3_t start_l, end_l;
	vec3_t offset;

	if ((model->engineflags & MDLF_HASBRUSHES))// && (size[0] || size[1] || size[2]))
	{
		struct traceinfo_s traceinfo;
		memset (&traceinfo.trace, 0, sizeof(trace_t));
		traceinfo.trace.fraction = 1;
		traceinfo.trace.allsolid = false;
		VectorCopy(mins, traceinfo.mins);
		VectorCopy(maxs, traceinfo.maxs);
		VectorCopy(start, traceinfo.start);
		VectorCopy(end, traceinfo.end);
		traceinfo.capsule = capsule;

		if (traceinfo.capsule)
		{
			float ext;
			traceinfo.capsulesize[0] = ((maxs[0]-mins[0]) + (maxs[1]-mins[1]))/4.0;
			traceinfo.capsulesize[1] = maxs[2];
			traceinfo.capsulesize[2] = mins[2];
			ext = (traceinfo.capsulesize[1] > -traceinfo.capsulesize[2])?traceinfo.capsulesize[1]:-traceinfo.capsulesize[2];
			traceinfo.capsulesize[1] -= traceinfo.capsulesize[0];
			traceinfo.capsulesize[2] += traceinfo.capsulesize[0];
			traceinfo.extents[0] = ext+1;
			traceinfo.extents[1] = ext+1;
			traceinfo.extents[2] = ext+1;
			VectorSet(traceinfo.up, 0, 0, 1);
		}

/*		traceinfo.sphere = true;
		traceinfo.radius = 48;
		traceinfo.mins[0] = -traceinfo.radius;
		traceinfo.mins[1] = -traceinfo.radius;
		traceinfo.mins[2] = -traceinfo.radius;
		traceinfo.maxs[0] = traceinfo.radius;
		traceinfo.maxs[1] = traceinfo.radius;
		traceinfo.maxs[2] = traceinfo.radius;
*/
		traceinfo.solidcontents = hitcontentsmask;
		Q1BSP_RecursiveBrushCheck(&traceinfo, model->rootnode, 0, 1, start, end);
		memcpy(trace, &traceinfo.trace, sizeof(trace_t));
		trace->contents = FTECONTENTS_SOLID;
		if (trace->fraction < 1)
		{
			float d1 = DotProduct(start, trace->plane.normal) - trace->plane.dist;
			float d2 = DotProduct(end, trace->plane.normal) - trace->plane.dist;
			float f = (d1 - DIST_EPSILON) / (d1 - d2);
			if (f < 0)
				f = 0;
			trace->fraction = f;
		}
		VectorInterpolate(start, trace->fraction, end, trace->endpos);
		return trace->fraction != 1;
	}

	memset (trace, 0, sizeof(trace_t));
	trace->fraction = 1;
	trace->allsolid = true;

	hull = Q1BSP_ChooseHull(model, forcehullnum, mins, maxs, offset);

//fix for hexen2 monsters half-walking into walls.
//	if (forent.flags & FL_MONSTER)
//	{
//		offset[0] = 0;
//		offset[1] = 0;
//	}

	if (axis)
	{
		vec3_t tmp;
		VectorSubtract(start, offset, tmp);
		start_l[0] = DotProduct(tmp, axis[0]);
		start_l[1] = DotProduct(tmp, axis[1]);
		start_l[2] = DotProduct(tmp, axis[2]);
		VectorSubtract(end, offset, tmp);
		end_l[0] = DotProduct(tmp, axis[0]);
		end_l[1] = DotProduct(tmp, axis[1]);
		end_l[2] = DotProduct(tmp, axis[2]);
		trace->contents = FTECONTENTS_SOLID;
		Q1BSP_RecursiveHullCheck(hull, hull->firstclipnode, 0, 1, start_l, end_l, trace);

		if (trace->fraction == 1)
		{
			VectorCopy (end, trace->endpos);
		}
		else
		{
			vec3_t iaxis[3];
			vec3_t norm;
			Matrix3x3_RM_Invert_Simple((void *)axis, iaxis);
			VectorCopy(trace->plane.normal, norm);
			trace->plane.normal[0] = DotProduct(norm, iaxis[0]);
			trace->plane.normal[1] = DotProduct(norm, iaxis[1]);
			trace->plane.normal[2] = DotProduct(norm, iaxis[2]);

			/*just interpolate it, its easier than inverse matrix rotations*/
			VectorInterpolate(start, trace->fraction, end, trace->endpos);
		}
	}
	else
	{
		VectorSubtract(start, offset, start_l);
		VectorSubtract(end, offset, end_l);
		Q1BSP_RecursiveHullCheck(hull, hull->firstclipnode, 0, 1, start_l, end_l, trace);

		if (trace->fraction == 1)
		{
			VectorCopy (end, trace->endpos);
		}
		else
		{
			VectorAdd (trace->endpos, offset, trace->endpos);
		}
	}

#ifdef TERRAIN
	if (model->terrain && trace->fraction)
	{
		trace_t hmt;
		Heightmap_Trace(model, forcehullnum, framestate, axis, start, end, mins, maxs, capsule, hitcontentsmask, &hmt);
		if (hmt.fraction < trace->fraction)
			*trace = hmt;
	}
#endif

	return trace->fraction != 1;
}

/*
Physics functions (common)

============================================================================

Rendering functions (Client only)
*/
#ifndef SERVERONLY

extern int	r_dlightframecount;

//goes through the nodes marking the surfaces near the dynamic light as lit.
void Q1BSP_MarkLights (dlight_t *light, int bit, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int			i;

	float		l, maxdist;
	int			j, s, t;
	vec3_t		impact;

	if (node->contents < 0)
		return;

	splitplane = node->plane;
	if (splitplane->type < 3)
		dist = light->origin[splitplane->type] - splitplane->dist;
	else
		dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;

	if (dist > light->radius)
	{
		Q1BSP_MarkLights (light, bit, node->children[0]);
		return;
	}
	if (dist < -light->radius)
	{
		Q1BSP_MarkLights (light, bit, node->children[1]);
		return;
	}

	maxdist = light->radius*light->radius;

// mark the polygons
	surf = currentmodel->surfaces + node->firstsurface;
	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		//Yeah, you can blame LordHavoc for this alternate code here.
		for (j=0 ; j<3 ; j++)
			impact[j] = light->origin[j] - surf->plane->normal[j]*dist;

		// clamp center of light to corner and check brightness
		l = DotProduct (impact, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3] - surf->texturemins[0];
		s = l+0.5;if (s < 0) s = 0;else if (s > surf->extents[0]) s = surf->extents[0];
		s = (l - s)*surf->texinfo->vecscale[0];
		l = DotProduct (impact, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3] - surf->texturemins[1];
		t = l+0.5;if (t < 0) t = 0;else if (t > surf->extents[1]) t = surf->extents[1];
		t = (l - t)*surf->texinfo->vecscale[1];
		// compare to minimum light
		if ((s*s+t*t+dist*dist) < maxdist)
		{
			if (surf->dlightframe != r_dlightframecount)
			{
				surf->dlightbits = bit;
				surf->dlightframe = r_dlightframecount;
			}
			else
				surf->dlightbits |= bit;
		}
	}

	Q1BSP_MarkLights (light, bit, node->children[0]);
	Q1BSP_MarkLights (light, bit, node->children[1]);
}

#endif
/*
Rendering functions (Client only)

==============================================================================

Server only functions
*/
#ifndef CLIENTONLY
static qbyte *Q1BSP_ClusterPVS (model_t *model, int cluster, pvsbuffer_t *buffer, pvsmerge_t merge);

//does the recursive work of Q1BSP_FatPVS
static void SV_Q1BSP_AddToFatPVS (model_t *mod, vec3_t org, mnode_t *node, pvsbuffer_t *pvsbuffer)
{
	mplane_t	*plane;
	float	d;

	while (1)
	{
	// if this is a leaf, accumulate the pvs bits
		if (node->contents < 0)
		{
			if (node->contents != Q1CONTENTS_SOLID)
			{
				Q1BSP_ClusterPVS(mod, ((mleaf_t *)node - mod->leafs)-1, pvsbuffer, PVM_MERGE);
			}
			return;
		}

		plane = node->plane;
		d = DotProduct (org, plane->normal) - plane->dist;
		if (d > 8)
			node = node->children[0];
		else if (d < -8)
			node = node->children[1];
		else
		{	// go down both
			SV_Q1BSP_AddToFatPVS (mod, org, node->children[0], pvsbuffer);
			node = node->children[1];
		}
	}
}

/*
=============
Q1BSP_FatPVS

Calculates a PVS that is the inclusive or of all leafs within 8 pixels of the
given point.
=============
*/
static unsigned int Q1BSP_FatPVS (model_t *mod, vec3_t org, pvsbuffer_t *pvsbuffer, qboolean add)
{
	if (pvsbuffer->buffersize < mod->pvsbytes)
		pvsbuffer->buffer = BZ_Realloc(pvsbuffer->buffer, pvsbuffer->buffersize=mod->pvsbytes);
	if (!add)
		Q_memset (pvsbuffer->buffer, 0, mod->pvsbytes);
	SV_Q1BSP_AddToFatPVS (mod, org, mod->nodes, pvsbuffer);
	return mod->pvsbytes;
}

#endif
static qboolean Q1BSP_EdictInFatPVS(model_t *mod, struct pvscache_s *ent, qbyte *pvs)
{
	int i;

	if (ent->num_leafs == MAX_ENT_LEAFS+1)
		return true;	//it's in too many leafs for us to cope with. Just trivially accept it.

	for (i=0 ; i < ent->num_leafs ; i++)
		if (pvs[ent->leafnums[i] >> 3] & (1 << (ent->leafnums[i]&7) ))
			return true;	//we might be able to see this one.

	return false;	//none of this ents leafs were visible, so neither is the ent.
}

/*
===============
SV_FindTouchedLeafs

Links the edict to the right leafs so we can get it's potential visability.
===============
*/
static void Q1BSP_RFindTouchedLeafs (model_t *wm, struct pvscache_s *ent, mnode_t *node, float *mins, float *maxs)
{
	mplane_t	*splitplane;
	mleaf_t		*leaf;
	int			sides;
	int			leafnum;

// add an efrag if the node is a leaf

	if (node->contents < 0)
	{
		//ignore solid leafs. this should include leaf 0 (which has no pvs info)
		if (node->contents == Q1CONTENTS_SOLID)
			return;

		if (ent->num_leafs >= MAX_ENT_LEAFS)
		{
			ent->num_leafs = MAX_ENT_LEAFS+1;	//too many. mark it as such so we can trivially accept huge mega-big brush models.
			return;
		}

		leaf = (mleaf_t *)node;
		leafnum = leaf - wm->leafs - 1;

		ent->leafnums[ent->num_leafs] = leafnum;
		ent->num_leafs++;
		return;
	}

// NODE_MIXED

	splitplane = node->plane;
	sides = BOX_ON_PLANE_SIDE(mins, maxs, splitplane);

// recurse down the contacted sides
	if (sides & 1)
		Q1BSP_RFindTouchedLeafs (wm, ent, node->children[0], mins, maxs);

	if (sides & 2)
		Q1BSP_RFindTouchedLeafs (wm, ent, node->children[1], mins, maxs);
}
static void Q1BSP_FindTouchedLeafs(model_t *mod, struct pvscache_s *ent, float *mins, float *maxs)
{
	ent->num_leafs = 0;
	if (mins && maxs)
		Q1BSP_RFindTouchedLeafs (mod, ent, mod->nodes, mins, maxs);
}


/*
Server only functions

==============================================================================

PVS type stuff
*/

/*
===================
Mod_DecompressVis
===================
*/
static qbyte *Q1BSP_DecompressVis (qbyte *in, model_t *model, qbyte *decompressed, unsigned int buffersize, qboolean merge)
{
	int		c;
	qbyte	*out;
	int		row;

	row = (model->numclusters+7)>>3;
	out = decompressed;

	if (buffersize < row)
		row = buffersize;

	if (!in)
	{	// no vis info, so make all visible
		while (row)
		{
			*out++ = 0xff;
			row--;
		}
		return decompressed;
	}

	if (merge)
	{
		do
		{
			if (*in)
			{
				*out++ |= *in++;
				continue;
			}
			out += in[1];
			in += 2;
		} while (out - decompressed < row);
	}
	else
	{
		do
		{
			if (*in)
			{
				*out++ = *in++;
				continue;
			}

			c = in[1];
			in += 2;
			while (c)
			{
				*out++ = 0;
				c--;
			}
		} while (out - decompressed < row);
	}

	return decompressed;
}

static pvsbuffer_t	mod_novis;
static pvsbuffer_t	mod_tempvis;

void Q1BSP_Shutdown(void)
{
	Z_Free(mod_novis.buffer);
	memset(&mod_novis, 0, sizeof(mod_novis));
	Z_Free(mod_tempvis.buffer);
	memset(&mod_tempvis, 0, sizeof(mod_tempvis));
}

//pvs is 1-based. clusters are 0-based. otherwise, q1bsp has a 1:1 mapping.
static qbyte *Q1BSP_ClusterPVS (model_t *model, int cluster, pvsbuffer_t *buffer, pvsmerge_t merge)
{
	if (cluster == -1)
	{
		if (merge == PVM_FAST)
		{
			if (mod_novis.buffersize < model->pvsbytes)
			{
				mod_novis.buffer = BZ_Realloc(mod_novis.buffer, mod_novis.buffersize=model->pvsbytes);
				memset(mod_novis.buffer, 0xff, mod_novis.buffersize);
			}
			return mod_novis.buffer;
		}
		if (buffer->buffersize < model->pvsbytes)
			buffer->buffer = BZ_Realloc(buffer->buffer, buffer->buffersize=model->pvsbytes);
		memset(buffer->buffer, 0xff, model->pvsbytes);
		return buffer->buffer;
	}

	if (merge == PVM_FAST && model->pvs)
		return model->pvs + cluster * model->pvsbytes;

	cluster++;

	if (!buffer)
		buffer = &mod_tempvis;

	if (buffer->buffersize < model->pvsbytes)
		buffer->buffer = BZ_Realloc(buffer->buffer, buffer->buffersize=model->pvsbytes);

	return Q1BSP_DecompressVis (model->leafs[cluster].compressed_vis, model, buffer->buffer, buffer->buffersize, merge==PVM_MERGE);
}

/*static qbyte *Q1BSP_ClusterPHS (model_t *model, int cluster, pvsbuffer_t *buffer, pvsmerge_t merge)
{
	if (cluster == -1 || !model->phs)
	{
		if (merge == PVM_FAST)
		{
			if (mod_novis.buffersize < model->pvsbytes)
			{
				mod_novis.buffer = BZ_Realloc(mod_novis.buffer, mod_novis.buffersize=model->pvsbytes);
				memset(mod_novis.buffer, 0xff, mod_novis.buffersize);
			}
			return mod_novis.buffer;
		}
		if (buffer->buffersize < model->pvsbytes)
			buffer->buffer = BZ_Realloc(buffer->buffer, buffer->buffersize=model->pvsbytes);
		memset(buffer->buffer, 0xff, model->pvsbytes);
		return buffer->buffer;
	}

	if (merge == PVM_FAST)
		return model->pvs + cluster * model->pvsbytes;

	if (!buffer)
		buffer = &mod_tempvis;
	if (buffer->buffersize < model->pvsbytes)
		buffer->buffer = BZ_Realloc(buffer->buffer, buffer->buffersize=model->pvsbytes);
	memcpy(buffer->buffer, model->pvs + cluster * model->pvsbytes, model->pvsbytes);
	return buffer->buffer;
}*/

//returns the leaf number, which is used as a bit index into the pvs.
static int Q1BSP_LeafnumForPoint (model_t *model, vec3_t p)
{
	mnode_t		*node;
	float		d;
	mplane_t	*plane;

	if (!model)
	{
		Sys_Error ("Mod_PointInLeaf: bad model");
	}
	if (!model->nodes)
		return 0;

	node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return (mleaf_t *)node - model->leafs;
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}

	return 0;	// never reached
}

mleaf_t *Q1BSP_LeafForPoint (model_t *model, vec3_t p)
{
	return model->leafs + Q1BSP_LeafnumForPoint(model, p);
}

static void Q1BSP_ClustersInSphere_Union(mleaf_t *firstleaf, vec3_t center, float radius, mnode_t *node, qbyte *out, qbyte *unionwith)
{	//this is really for rtlights.
	float		t1, t2;
	mplane_t	*plane;
	while (1)
	{
		if (node->contents < 0)
		{	//leaf! mark/merge it.
			size_t c = (mleaf_t *)node - firstleaf;
			if (c == -1)
				return;
			if (unionwith)
				out[c>>3] |= (1<<(c&7)) & unionwith[c>>3];
			else
				out[c>>3] |= (1<<(c&7));
			return;
		}

		plane = node->plane;
		if (plane->type < 3)
			t1 = center[plane->type] - plane->dist;
		else
			t1 = DotProduct (plane->normal, center) - plane->dist;
		t2 = t1 - radius;
		t1 = t1 + radius;

		//if the sphere is fully to one side, only walk that side.
		if (t1 > 0 && t2 > 0)
		{
			node = node->children[0];
			continue;
		}
		if (t1 < 0 && t2 < 0)
		{
			node = node->children[1];
			continue;
		}

		//both sides are within the sphere
		Q1BSP_ClustersInSphere_Union(firstleaf, center, radius, node->children[0], out, unionwith);
		node = node->children[1];
		continue;
	}
}
static qbyte *Q1BSP_ClustersInSphere(model_t *mod, vec3_t center, float radius, pvsbuffer_t *pvsbuffer, qbyte *unionwith)
{
	if (!mod)
		Sys_Error ("Mod_PointInLeaf: bad model");
	if (!mod->nodes)
		return NULL;

	if (pvsbuffer->buffersize < mod->pvsbytes)
		pvsbuffer->buffer = BZ_Realloc(pvsbuffer->buffer, pvsbuffer->buffersize=mod->pvsbytes);
	Q_memset (pvsbuffer->buffer, 0, mod->pvsbytes);
	Q1BSP_ClustersInSphere_Union(mod->leafs+1, center, radius, mod->nodes, pvsbuffer->buffer, NULL);//unionwith);
	return pvsbuffer->buffer;
}

//returns the leaf number, which is used as a direct bit index into the pvs.
//-1 for invalid
static int Q1BSP_ClusterForPoint (model_t *model, vec3_t p)
{
	mnode_t		*node;
	float		d;
	mplane_t	*plane;

	if (!model)
	{
		Sys_Error ("Mod_PointInLeaf: bad model");
	}
	if (!model->nodes)
		return -1;

	node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return ((mleaf_t *)node - model->leafs) - 1;
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}

	return -1;	// never reached
}


/*
PVS type stuff

==============================================================================

Init stuff
*/


void Q1BSP_Init(void)
{
}

//sets up the functions a server needs.
//fills in bspfuncs_t
void Q1BSP_SetModelFuncs(model_t *mod)
{
#ifndef CLIENTONLY
	mod->funcs.FatPVS				= Q1BSP_FatPVS;
#endif
	mod->funcs.EdictInFatPVS		= Q1BSP_EdictInFatPVS;
	mod->funcs.FindTouchedLeafs		= Q1BSP_FindTouchedLeafs;
	mod->funcs.LightPointValues		= NULL;
	mod->funcs.StainNode			= NULL;
	mod->funcs.MarkLights			= NULL;

	mod->funcs.ClustersInSphere		= Q1BSP_ClustersInSphere;
	mod->funcs.ClusterForPoint		= Q1BSP_ClusterForPoint;
	mod->funcs.ClusterPVS			= Q1BSP_ClusterPVS;
//	mod->funcs.ClusterPHS			= Q1BSP_ClusterPHS;
	mod->funcs.NativeTrace			= Q1BSP_Trace;
	mod->funcs.PointContents		= Q1BSP_PointContents;
}
#endif

/*
Init stuff

==============================================================================

BSPX Stuff
*/

typedef struct {
    char lumpname[24]; // up to 23 chars, zero-padded
    int fileofs;  // from file start
    int filelen;
} bspx_lump_t;
struct bspx_header_s {
    char id[4];  // 'BSPX'
    int numlumps;
	bspx_lump_t lumps[1];
};
//supported lumps:
//RGBLIGHTING (.lit)
//LIGHTINGDIR (.lux)
void *BSPX_FindLump(bspx_header_t *bspxheader, void *mod_base, char *lumpname, int *lumpsize)
{
	int i;
	*lumpsize = 0;
	if (!bspxheader)
		return NULL;

	for (i = 0; i < bspxheader->numlumps; i++)
	{
		if (!strncmp(bspxheader->lumps[i].lumpname, lumpname, 24))
		{
			*lumpsize = bspxheader->lumps[i].filelen;
			return (char*)mod_base + bspxheader->lumps[i].fileofs;
		}
	}
	return NULL;
}
bspx_header_t *BSPX_Setup(model_t *mod, char *filebase, unsigned int filelen, lump_t *lumps, int numlumps)
{
	int i;
	int offs = 0;
	bspx_header_t *h;

	for (i = 0; i < numlumps; i++, lumps++)
	{
		if (offs < lumps->fileofs + lumps->filelen)
			offs = lumps->fileofs + lumps->filelen;
	}
	offs = (offs + 3) & ~3;
	if (offs + sizeof(*h) > filelen)
		return NULL; /*no space for it*/
	h = (bspx_header_t*)(filebase + offs);

	i = LittleLong(h->numlumps);
	/*verify the header*/
	if (*(int*)h->id != (('B'<<0)|('S'<<8)|('P'<<16)|('X'<<24)) ||
		i < 0 ||
		offs + sizeof(*h) + sizeof(h->lumps[0])*(i-1) > filelen)
		return NULL;
	h->numlumps = i;
	while(i-->0)
	{
		h->lumps[i].fileofs = LittleLong(h->lumps[i].fileofs);
		h->lumps[i].filelen = LittleLong(h->lumps[i].filelen);
		if (h->lumps[i].fileofs + h->lumps[i].filelen > filelen)
			return NULL;
	}

	return h;
}

#ifdef SERVERONLY
void BSPX_LoadEnvmaps(model_t *mod, bspx_header_t *bspx, void *mod_base)
{
}
#else
/*
void *SCR_ScreenShot_Capture(int fbwidth, int fbheight, int *stride, enum uploadfmt *fmt);
void BSPX_RenderEnvmaps(model_t *mod)
{
	int c, i;

	void *buffer;
	int stride, cubesize;
	uploadfmt_t fmt;
	char filename[MAX_QPATH];
	char olddrawviewmodel[64];	//hack, so we can set r_drawviewmodel to 0 so that it doesn't appear in screenshots even if the csqc is generating new data.
	vec3_t oldangles;
	const struct
	{
		vec3_t angle;
		const char *postfix;
		qboolean verticalflip;
		qboolean horizontalflip;
	} sides[] =
	{
		{{0, 0, 90}, "_px", true},
		{{0, 180, -90}, "_nx", true},
		{{0, 90, 0}, "_py", true},	//upside down
		{{0, 270, 0}, "_ny", false, true},
		{{-90, 0, 90}, "_pz", true},
		{{90, 0, 90}, "_nz", true},
	};
	char base[MAX_QPATH];
	COM_FileBase(cl.worldmodel->name, base, sizeof(base));

	r_refdef.stereomethod = STEREO_OFF;
	Q_strncpyz(olddrawviewmodel, r_drawviewmodel.string, sizeof(olddrawviewmodel));
	Cvar_Set(&r_drawviewmodel, "0");

	VectorCopy(cl.playerview->viewangles, oldangles);

	for (c = 0; c < mod->numenvmaps; c++)
	{
		cubesize = mod->envmaps[c].cubesize;
		if (cubesize < 1)
			cubesize = 32;

		VectorCopy(mod->envmaps[c].origin, r_refdef.vieworg);

		for (i = 0; i < 6; i++)
		{
			Q_snprintfz(filename, sizeof(filename), "%s/%i_%i_%i%s.tga", base, (int)mod->envmaps[c].origin[0], (int)mod->envmaps[c].origin[1], (int)mod->envmaps[c].origin[2], sides[i].postfix);

			VectorCopy(sides[i].angle, cl.playerview->simangles);
			VectorCopy(cl.playerview->simangles, cl.playerview->viewangles);

			buffer = SCR_ScreenShot_Capture(cubesize, cubesize, &stride, &fmt);
			if (buffer)
			{
				char			sysname[1024];
				if (sides[i].horizontalflip)
				{
					int y, x, p;
					int pxsize;
					char *bad = buffer;
					char *in = buffer, *out;
					switch(fmt)
					{
					case TF_RGBA32:
					case TF_BGRA32:
					case TF_RGBX32:
					case TF_BGRX32:
						pxsize = 4;
						break;
					case TF_RGB24:
					case TF_BGR24:
						pxsize = 3;
						break;
					case TF_RGBA16F:
						pxsize = 8;
						break;
					case TF_RGBA32F:
						pxsize = 16;
						break;
					default:	//erk!
						pxsize = 1;
						break;
					}
					buffer = out = BZ_Malloc(cubesize*cubesize*pxsize);
					for (y = 0; y < cubesize; y++, in += abs(stride), out += cubesize*pxsize)
					{
						for (x = 0; x < cubesize*pxsize; x+=pxsize)
						{
							for (p = 0; p < pxsize; p++)
								out[x+p] = in[(cubesize-1)*pxsize-x+p];
						}
					}
					BZ_Free(bad);
					if (stride < 0)
						stride = -cubesize*pxsize;
					else
						stride = cubesize*pxsize;
				}
				if (sides[i].verticalflip)
					stride = -stride;
				if (SCR_ScreenShot(filename, FS_GAMEONLY, &buffer, 1, stride, cubesize, cubesize, fmt))
				{
					FS_NativePath(filename, FS_GAMEONLY, sysname, sizeof(sysname));
					Con_Printf ("Wrote %s\n", sysname);
				}
				else
				{
					FS_NativePath(filename, FS_GAMEONLY, sysname, sizeof(sysname));
					Con_Printf ("Failed to write %s\n", sysname);
				}
				BZ_Free(buffer);
			}
		}
	}
	Cvar_Set(&r_drawviewmodel, olddrawviewmodel);

	VectorCopy(oldangles, cl.playerview->viewangles);
}
*/

void BSPX_LoadEnvmaps(model_t *mod, bspx_header_t *bspx, void *mod_base)
{
	unsigned int *envidx, idx;
	int i;
	char base[MAX_QPATH];
	char imagename[MAX_QPATH];
	menvmap_t *out;
	int count;
	denvmap_t *in = BSPX_FindLump(bspx, mod_base, "ENVMAP", &count);
	if (count%sizeof(*in))
		return;	//erk
	count /= sizeof(*in);
	if (!count)
		return;
	out = ZG_Malloc(&mod->memgroup, sizeof(*out)*count);

	mod->envmaps = out;
	mod->numenvmaps = count;

	COM_FileBase(mod->name, base, sizeof(base));
	for (i = 0; i < count; i++)
	{
		out[i].origin[0] = LittleFloat(in[i].origin[0]);
		out[i].origin[1] = LittleFloat(in[i].origin[1]);
		out[i].origin[2] = LittleFloat(in[i].origin[2]);
		out[i].cubesize = LittleLong(in[i].cubesize);

		Q_snprintfz(imagename, sizeof(imagename), "textures/env/%s_%i_%i_%i", base, (int)mod->envmaps[i].origin[0], (int)mod->envmaps[i].origin[1], (int)mod->envmaps[i].origin[2]);
		out[i].image = Image_GetTexture(imagename, NULL, IF_CUBEMAP|IF_NOREPLACE, NULL, NULL, out[i].cubesize, out[i].cubesize, PTI_INVALID);
	}



	//now update surface lists.
	envidx = BSPX_FindLump(bspx, mod_base, "SURFENVMAP", &i);
	if (i/sizeof(*envidx) == mod->numsurfaces)
	{
		for (i = 0; i < mod->numsurfaces; i++)
		{
			idx = LittleLong(envidx[i]);
			if (idx < (unsigned int)count)
				mod->surfaces[i].envmap = out[idx].image;
		}
	}
}

struct bspxrw
{
	fromgame_t fg;
	const char *fname;
	char *origfile;
	qofs_t origsize;
	int lumpofs;

	size_t corelumps;
	size_t totallumps;

	struct
	{
		char lumpname[24]; // up to 23 chars, zero-padded
		void *data;  // from file start
		qofs_t filelen;
	} *lumps;
};
void Mod_BSPXRW_Free(struct bspxrw *ctx)
{
	FS_FreeFile(ctx->origfile);
	Z_Free(ctx->lumps);
	ctx->corelumps = ctx->totallumps = 0;
	ctx->origfile = NULL;
}
void Mod_BSPXRW_Write(struct bspxrw *ctx)
{
#if 1
	vfsfile_t *f = FS_OpenVFS(ctx->fname, "wb", FS_GAMEONLY);
	if (f)
	{
		qofs_t bspxofs;
		size_t i, j;
		int pad, paddata = 0;
		int nxlumps = ctx->totallumps-ctx->corelumps;
		lump_t *lumps = alloca(sizeof(*lumps)*ctx->corelumps);
		bspx_lump_t *xlumps = alloca(sizeof(*xlumps)*(ctx->totallumps-ctx->corelumps));
		//bsp header info
		VFS_WRITE(f, ctx->origfile, ctx->lumpofs);
		VFS_WRITE(f, lumps, sizeof(lumps[0])*ctx->corelumps);	//placeholder
		//orig lumps
		for (i = 0; i < ctx->corelumps; i++)
		{
			lumps[i].fileofs = VFS_TELL(f);
			lumps[i].filelen = ctx->lumps[i].filelen;

			VFS_WRITE(f, ctx->lumps[i].data, ctx->lumps[i].filelen);
			//ALL lumps must be 4-aligned, so pad if needed.
			pad = ((ctx->lumps[i].filelen+3)&~3)-ctx->lumps[i].filelen;
			VFS_WRITE(f, &paddata, pad);
		}
		//bspx header
		VFS_WRITE(f, "BSPX", 4);
		VFS_WRITE(f, &nxlumps, sizeof(nxlumps));
		bspxofs = VFS_TELL(f);
		VFS_WRITE(f, xlumps, sizeof(xlumps[0])*(ctx->totallumps-ctx->corelumps)); //placeholder
		//bspx data
		for (i = 0; i < nxlumps; i++)
		{
			j = ctx->corelumps+i;
			xlumps[i].fileofs = VFS_TELL(f);
			xlumps[i].filelen = ctx->lumps[j].filelen;
			memcpy(xlumps[i].lumpname, ctx->lumps[j].lumpname, sizeof(xlumps[i].lumpname));

			VFS_WRITE(f, ctx->lumps[j].data, ctx->lumps[j].filelen);
			//ALL lumps must be 4-aligned, so pad if needed.
			pad = ((ctx->lumps[j].filelen+3)&~3)-ctx->lumps[j].filelen;
			VFS_WRITE(f, &paddata, pad);
		}

		//now rewrite both sets of offsets.
		VFS_SEEK(f, ctx->lumpofs);
		VFS_WRITE(f, lumps, sizeof(lumps[0])*ctx->corelumps);
		VFS_SEEK(f, bspxofs);
		VFS_WRITE(f, xlumps, sizeof(xlumps[0])*(ctx->totallumps-ctx->corelumps));

		VFS_CLOSE(f);
	}
#endif
	Mod_BSPXRW_Free(ctx);
}

void Mod_BSPXRW_SetLump(struct bspxrw *ctx, const char *lumpname, void *data, size_t datasize)
{
	int i;
	for (i = 0; i < ctx->totallumps; i++)
	{
		if (!strcmp(ctx->lumps[i].lumpname, lumpname))
		{	//replace the existing lump
			ctx->lumps[i].data = data;
			ctx->lumps[i].filelen = datasize;
			return;
		}
	}

	Z_ReallocElements((void**)&ctx->lumps, &ctx->totallumps, ctx->totallumps+1, sizeof(*ctx->lumps));
	Q_strncpyz(ctx->lumps[i].lumpname, lumpname, sizeof(ctx->lumps[i].lumpname));
	ctx->lumps[i].data = data;
	ctx->lumps[i].filelen = datasize;
}

qboolean Mod_BSPXRW_Read(struct bspxrw *ctx, const char *fname)
{
	int i;
	lump_t *l;
	const char **corelumpnames = NULL;
	bspx_header_t *bspxheader;
#ifdef Q3BSPS
	static const char *q3corelumpnames[Q3LUMPS_TOTAL] = {"entities","shaders","planes","nodes","leafs","leafsurfs","leafbrushes","submodels","brushes","brushsides","verts","indexes","fogs","surfaces","lightmaps","lightgrid","visibility"
		#ifdef RFBSPS
			,"lightgrididx"
		#endif
		};
#endif
	ctx->fname = fname;
	ctx->origfile = FS_MallocFile(ctx->fname, FS_GAME, &ctx->origsize);
	if (!ctx->origfile)
		return false;
	ctx->lumps = 0;
	ctx->totallumps = 0;

	i = LittleLong(*(int*)ctx->origfile);
	switch(i)
	{
	case 29:
	case 30:
		ctx->fg = ((i==30)?fg_halflife:fg_quake);
		ctx->lumpofs = 4;
		ctx->corelumps = 0;
		break;
	case IDBSPHEADER:
		i = LittleLong(*(int*)(ctx->origfile+4));
		ctx->lumpofs = 8;
		switch(i)
		{
#ifdef Q2BSPS
		case BSPVERSION_Q2:
//		case BSPVERSION_Q2W:
			ctx->fg = fg_quake2;
			ctx->corelumps = Q2HEADER_LUMPS;
			break;
#endif
#ifdef Q3BSPS
		case BSPVERSION_Q3:
		case BSPVERSION_RTCW:
			ctx->fg = fg_quake3;
			ctx->corelumps = 17;
			corelumpnames = q3corelumpnames;
			break;
#endif
		default:
			Mod_BSPXRW_Free(ctx);
			return false;
		}
		break;
#ifdef RFBSPS
	case ('R'<<0)+('B'<<8)+('S'<<16)+('P'<<24):
	case ('F'<<0)+('B'<<8)+('S'<<16)+('P'<<24):
		i = LittleLong(*(int*)(ctx->origfile+4));
		ctx->lumpofs = 8;
		switch(i)
		{
		case BSPVERSION_RBSP:
			ctx->fg = fg_quake3;
			ctx->corelumps = 18;
			corelumpnames = q3corelumpnames;
			break;
		default:
			Mod_BSPXRW_Free(ctx);
			return false;
		}
		break;
#endif
	default:
		Mod_BSPXRW_Free(ctx);
		return false;
	}

	l = (lump_t*)(ctx->origfile+ctx->lumpofs);
	for (i = 0; i < ctx->corelumps; i++)
	{
		Z_ReallocElements((void**)&ctx->lumps, &ctx->totallumps, ctx->totallumps+1, sizeof(*ctx->lumps));
		ctx->lumps[ctx->totallumps-1].data = ctx->origfile+l[i].fileofs;
		ctx->lumps[ctx->totallumps-1].filelen = l[i].filelen;
		if (corelumpnames)
			Q_snprintfz(ctx->lumps[ctx->totallumps-1].lumpname, sizeof(ctx->lumps[0].lumpname), "%s", corelumpnames[i]);
		else
			Q_snprintfz(ctx->lumps[ctx->totallumps-1].lumpname, sizeof(ctx->lumps[0].lumpname), "lump%u", i);
	}

	bspxheader = BSPX_Setup(NULL, ctx->origfile, ctx->origsize, l, ctx->corelumps);
	if (bspxheader)
	{
		for (i = 0; i < bspxheader->numlumps; i++)
		{
			Z_ReallocElements((void**)&ctx->lumps, &ctx->totallumps, ctx->totallumps+1, sizeof(*ctx->lumps));
			ctx->lumps[ctx->totallumps-1].data = ctx->origfile+bspxheader->lumps[i].fileofs;
			ctx->lumps[ctx->totallumps-1].filelen = bspxheader->lumps[i].filelen;
			memcpy(ctx->lumps[ctx->totallumps-1].lumpname, bspxheader->lumps[i].lumpname, sizeof(ctx->lumps[0].lumpname));
		}
	}
	return true;
}

unsigned int Mod_NearestCubeForSurf(msurface_t *surf, denvmap_t *envmap, size_t nenvmap)
{	//this is slow, yes.
	size_t n, v;
	unsigned int best = ~0;
	float bestdist = FLT_MAX, dist;
	vec3_t diff, mid;

	if (surf->mesh)
	{
		VectorClear(mid);
		for (v = 0; v < surf->mesh->numvertexes; v++)
			VectorAdd(mid, surf->mesh->xyz_array[v], mid);
		VectorScale(mid, 1.0/surf->mesh->numvertexes, mid);

		for (n = 0; n < nenvmap; n++)
		{
			VectorSubtract(mid, envmap[n].origin, diff);
#if 1
			//axial distance
			dist = min(min(fabs(diff[0]), fabs(diff[1])), fabs(diff[2]));
#else
			//radial distance (squared)
			dist = DotProduct(diff,diff);
#endif
			if (bestdist > dist)
			{
				best = n;
				bestdist = dist;
			}
		}
	}
	return best;
}

int QDECL envmapsort(const void *av, const void *bv)
{	//sorts cubemaps in order of size, to make texturearrays easier, if ever. The loader can then just make runs.
	const denvmap_t *a=av, *b=bv;
	if (a->cubesize == b->cubesize)
		return 0;
	if (a->cubesize > b->cubesize)
		return 1;
	return -1;
}
void Mod_FindCubemaps_f(void)
{
	struct bspxrw bspctx;
	if (Mod_BSPXRW_Read(&bspctx, cl.worldmodel->name))
	{
		const char *entlump = Mod_GetEntitiesString(cl.worldmodel), *lmp;
		int nest;
		char key[1024];
		char value[1024];

		qboolean isenvmap;
		float size;
		vec3_t origin;

		denvmap_t *envmap = NULL; //*nenvmap
		size_t	nenvmap = 0;
		unsigned int *envmapidx = NULL;	//*numsurfaces
		size_t nenvmapidx = 0, i;
		
		//find targetnames, and store their origins so that we can deal with spotlights.
		for (lmp = entlump; ;)
		{
			lmp = COM_Parse(lmp);
			if (com_token[0] != '{')
				break;

			isenvmap = false;
			size = 128;
			VectorClear(origin);

			nest = 1;
			while (1)
			{
				lmp = COM_ParseOut(lmp, key, sizeof(key));
				if (!lmp)
					break; // error
				if (key[0] == '{')
				{
					nest++;
					continue;
				}
				if (key[0] == '}')
				{
					nest--;
					if (!nest)
						break; // end of entity
					continue;
				}
				if (nest!=1)
					continue;
				if (key[0] == '_')
					memmove(key, key+1, strlen(key));
				while (key[strlen(key)-1] == ' ') // remove trailing spaces
					key[strlen(key)-1] = 0;
				lmp = COM_ParseOut(lmp, value, sizeof(value));
				if (!lmp)
					break; // error

				// now that we have the key pair worked out...
				if (!strcmp("classname", key) && !strcmp(value, "env_cubemap"))
					isenvmap = true;
				else if (!strcmp("origin", key))
					sscanf(value, "%f %f %f", &origin[0], &origin[1], &origin[2]);
				else if (!strcmp("size", key))
					sscanf(value, "%f", &size);
			}
			
			if (isenvmap)
			{
				int e = nenvmap;
				if (ZF_ReallocElements((void**)&envmap, &nenvmap, nenvmap+1, sizeof(*envmap)))
				{
					VectorCopy(origin, envmap[e].origin);
					envmap[e].cubesize = size;
				}
			}
		}

		if (nenvmap)
		{
			qsort(envmap, nenvmap, sizeof(*envmap), envmapsort);
			if (ZF_ReallocElements((void**)&envmapidx, &nenvmapidx, cl.worldmodel->numsurfaces, sizeof(*envmapidx)))
			{
				for(i = 0; i < cl.worldmodel->numsurfaces; i++)
					envmapidx[i] = Mod_NearestCubeForSurf(cl.worldmodel->surfaces+i, envmap, nenvmap);
			}

			Mod_BSPXRW_SetLump(&bspctx, "ENVMAP", envmap, nenvmap*sizeof(*envmap));
			Mod_BSPXRW_SetLump(&bspctx, "SURFENVMAP", envmapidx, cl.worldmodel->numsurfaces*sizeof(*envmapidx));
			Mod_BSPXRW_Write(&bspctx);
		}
		else
		{
			Con_Printf("No cubemaps found on map\n");
			Mod_BSPXRW_Free(&bspctx);
		}

		Z_Free(envmapidx);
		Z_Free(envmap);
	}
}
void Mod_Realign_f(void)
{
	struct bspxrw bspctx;
	if (Mod_BSPXRW_Read(&bspctx, cl.worldmodel->name))
		Mod_BSPXRW_Write(&bspctx);
}
void Mod_BSPX_List_f(void)
{
	int i;
	struct bspxrw ctx;
	char *fname = Cmd_Argv(1);
	if (!*fname && cl.worldmodel)
		fname = cl.worldmodel->name;
	if (Mod_BSPXRW_Read(&ctx, fname))
	{
		for (i = 0; i < ctx.corelumps; i++)
		{
			Con_Printf("%s: %u\n", ctx.lumps[i].lumpname, (unsigned int)ctx.lumps[i].filelen);
		}
		for (     ; i < ctx.totallumps; i++)
		{
			Con_Printf("%s: %u\n", ctx.lumps[i].lumpname, (unsigned int)ctx.lumps[i].filelen);
		}
		Mod_BSPXRW_Free(&ctx);
	}
}
void Mod_BSPX_Strip_f(void)
{
	int i;
	struct bspxrw ctx;
	qboolean found = false;
	if (Cmd_Argc() != 3)
		Con_Printf("%s FILENAME NAME: removes an extended lump from the named bsp file\n", Cmd_Argv(0));
	else if (Mod_BSPXRW_Read(&ctx, Cmd_Argv(1)))
	{
		for (i = ctx.corelumps; i < ctx.totallumps;)
		{
			if (!Q_strcasecmp(ctx.lumps[i].lumpname, Cmd_Argv(2)))
			{
				found = true;
				memmove(&ctx.lumps[i], &ctx.lumps[i+1], sizeof(ctx.lumps[0])*(ctx.totallumps-(i+1)));
				ctx.totallumps--;
			}
			else
				i++;
		}
		if (found)
			Mod_BSPXRW_Write(&ctx);
		else
			Mod_BSPXRW_Free(&ctx);
	}
}
#endif
