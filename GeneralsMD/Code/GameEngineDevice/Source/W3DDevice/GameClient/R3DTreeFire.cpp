/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: the map's trees catch fire, burn and stay burnt.
*/

// Ronin @feature 25/10/2026 DX9: see R3DTreeFire.h. The base game's tree fire: FlammableUpdate's timeline, FireSpreadUpdate's
// spread and the effects of its trees' W3DModelDraw states (its NatureProp.ini). Lighting_Work.md, "TREES ON FIRE".
#include "WW3D2/dx8todx9.h"
#include "WWLib/always.h"
#include "Lib/BaseType.h"
#include "Common/GameCommon.h"
#include "Common/GameUtility.h"
#include "Common/FileSystem.h"
#include "Common/Player.h"
#include "GameClient/ClientRandomValue.h"
#include "GameClient/ParticleSys.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/PartitionManager.h"
#include "WW3D2/assetmgr.h"
#include "WW3D2/rendobj.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "WW3D2/matinfo.h"
#include "WW3D2/texture.h"
#include "W3DDevice/GameClient/W3DTreeBuffer.h"
#include "W3DDevice/GameClient/Module/W3DTreeDraw.h"
#include "W3DDevice/GameClient/R3DTreeFire.h"
#include <cstdio>
#include <cstring>
#include <cctype>

// The base game's trees: FlammableUpdate BurnedDelay 2500 and AflameDuration 3500 ms, FireSpreadUpdate 1000-2000 ms over 50.
static const UnsignedInt BURNED_DELAY    = 2500 * LOGICFRAMES_PER_SECOND / 1000;	// the burnt look, under smaller flames
static const UnsignedInt AFLAME_DURATION = 3500 * LOGICFRAMES_PER_SECOND / 1000;	// the flames end, the smoke starts
static const UnsignedInt SMOKE_FRAMES    = 150;		// SmoulderTree2's own SystemLifetime: the fire's record ends with it
static const Int         SPREAD_MIN      = 1000 * LOGICFRAMES_PER_SECOND / 1000;
static const Int         SPREAD_MAX      = 2000 * LOGICFRAMES_PER_SECOND / 1000;
static const Real        SPREAD_RANGE    = 50.0f;
static const Real        TREE_REACH      = 4.0f;	// the trees' GeometryMajorRadius (3-4): damage reached a tree by its edge
static const Real        FIRE_HEIGHT     = 0.44f;	// no AFlame01 bone: the trunk at this share of the height (the 85 bones' median)
// The burn byte (TTree::m_burnChar, read back by Trees_vs.hlsl): CHARRED = the shader's char, the low 7 bits the canopy gone.
static const UnsignedByte CHARRED        = 0x80;
static const Real        ART_LOSS        = 0.53f;	// the artists' burnt textures already drop this share of the canopy (15 pairs)

enum { TYPE_SLOTS = 64, MAX_FIRES = 64, SYSTEMS = 3 };

// The base game's tree effects per FlammableUpdate state, at the AFlame01 bone.
static const char *const STAGE_SYSTEMS[3][SYSTEMS] =
{
	{ "FireTreeSmall",   "SmoulderTree1", "FireTreeSparks" },	// AFLAME
	{ "FireTreeSmaller", "SmoulderTree1", "FireTreeSparks" },	// AFLAME SMOLDERING
	{ "SmoulderTree2",   "SmoulderTree3", nullptr },			// SMOLDERING
};

struct TypeFire
{
	Int                    burntType;	// -1: none, the shader's char
	Int                    greenType;	// a burnt type: the green type it was made for; -1 on a green type
	Bool                   flammable;
	Real                   fire[3];		// the fire point in the model's space, before the tree's scale and turn
	W3DTreeDrawModuleData *owned;		// a burnt type's module data: its green one's, with the burnt names
};

struct Fire
{
	Int              tree;
	UnsignedInt      start;			// the logic frame it caught
	UnsignedInt      nextSpread;
	Int              stage;			// 0 flames, 1 flames on the burnt tree, 2 smoke
	ParticleSystemID sys[SYSTEMS];
};

static TypeFire       s_types[TYPE_SLOTS];
static Fire           s_fires[MAX_FIRES];
static Int            s_fireCount = 0;
static W3DTreeBuffer *s_buffer    = nullptr;
static UnsignedInt    s_lastFrame = 0xFFFFFFFF;
static Bool           s_prepared  = FALSE;		// this map's burnt types are in
static Bool           s_enabled   = TRUE;
static Bool           s_spread    = TRUE;
static Int            s_cap       = 20;			// trees with effects at once: about 50 particles each, of High's 3000
static Real           s_thin      = 0.6f;		// the burnt canopy's share gone; the trunk-safe most is 0.58

// A burnt tree's byte: the share gone, all of it on a charred tree; on burnt art only what the art has not dropped already.
static UnsignedByte burnCode(Bool charred)
{
	Real thin = charred ? s_thin : 1.0f - (1.0f - s_thin) / (1.0f - ART_LOSS);
	thin = (thin < 0.0f) ? 0.0f : (thin > 1.0f ? 1.0f : thin);
	return (UnsignedByte)((UnsignedByte)(thin * 127.0f + 0.5f) | (charred ? CHARRED : 0));
}

static void clearType(TypeFire &tf)
{
	tf.burntType = -1;
	tf.greenType = -1;
	tf.flammable = FALSE;
	tf.fire[0] = tf.fire[1] = tf.fire[2] = 0.0f;
	tf.owned = nullptr;
}

static Bool hasBurntSuffix(const char *name)
{
	char low[_MAX_PATH];
	Int i = 0;
	for (; name[i] != 0 && i < _MAX_PATH - 1; ++i)
		low[i] = (char)tolower((unsigned char)name[i]);
	low[i] = 0;
	return strstr(low, "_b.") != nullptr || strstr(low, "_d.") != nullptr;
}

// The tree atlas reads TGA only, from Art/Terrain then Art/Textures (W3DTreeBuffer::updateTexture).
static Bool atlasCanRead(const char *texture)
{
	const size_t n = strlen(texture);
	if (n < 4 || _stricmp(texture + n - 4, ".tga") != 0 || TheFileSystem == nullptr)
		return FALSE;
	char path[_MAX_PATH];
	snprintf(path, sizeof(path), "%s%s", TERRAIN_TGA_DIR_PATH, texture);
	if (TheFileSystem->doesFileExist(path))
		return TRUE;
	snprintf(path, sizeof(path), "%s%s", TGA_DIR_PATH, texture);
	return TheFileSystem->doesFileExist(path);
}

// A model's own texture: its UVs are laid out for it, so a burnt model brings its own.
static Bool modelTexture(const char *model, char *out, Int size)
{
	out[0] = 0;
	RenderObjClass *robj = WW3DAssetManager::Get_Instance()->Create_Render_Obj(model);
	if (robj != nullptr && robj->Class_ID() == RenderObjClass::CLASSID_HLOD)
	{
		RenderObjClass *sub = robj->Get_Sub_Object(0);
		REF_PTR_RELEASE(robj);
		robj = sub;
	}
	if (robj != nullptr && robj->Class_ID() == RenderObjClass::CLASSID_MESH)
	{
		MaterialInfoClass *info = robj->Get_Material_Info();
		TextureClass *tex = (info != nullptr && info->Texture_Count() > 0) ? info->Peek_Texture(0) : nullptr;
		if (tex != nullptr)
			snprintf(out, size, "%s", (const char *)tex->Get_Texture_Name());
		REF_PTR_RELEASE(info);
	}
	REF_PTR_RELEASE(robj);
	return out[0] != 0;
}

static void stopSystems(Fire &f)
{
	for (Int i = 0; i < SYSTEMS; ++i)
	{
		// destroy() stops the emitting and lets the last particles fade.
		if (f.sys[i] != INVALID_PARTICLE_SYSTEM_ID && TheParticleSystemManager != nullptr)
			TheParticleSystemManager->destroyParticleSystemByID(f.sys[i]);
		f.sys[i] = INVALID_PARTICLE_SYSTEM_ID;
	}
}

// A stage's effects at the fire point - only where the player sees the ground, as FXList::doFXPos does.
static void startSystems(Fire &f, Int stage, const Coord3D *pos)
{
	stopSystems(f);
	if (TheParticleSystemManager == nullptr || ThePartitionManager == nullptr)
		return;
	const Int player = rts::getObservedOrLocalPlayer()->getPlayerIndex();
	if (ThePartitionManager->getShroudStatusForPlayer(player, pos) != CELLSHROUD_CLEAR)
		return;
	for (Int i = 0; i < SYSTEMS; ++i)
	{
		if (STAGE_SYSTEMS[stage][i] == nullptr)
			continue;
		const ParticleSystemTemplate *tmpl = TheParticleSystemManager->findTemplate(AsciiString(STAGE_SYSTEMS[stage][i]));
		ParticleSystem *sys = (tmpl != nullptr) ? TheParticleSystemManager->createParticleSystem(tmpl) : nullptr;
		if (sys == nullptr)
			continue;
		sys->setPosition(pos);
		sys->setSaveable(FALSE);	// a save's trees load green and without their fires
		f.sys[i] = sys->getSystemID();
	}
}

//-------------------------------------------------------------------------------------------------
void R3DTreeFire::attach(W3DTreeBuffer *buffer)
{
	s_buffer = buffer;
}

void R3DTreeFire::detach(W3DTreeBuffer *buffer)
{
	reset(buffer, TRUE);
	if (s_buffer == buffer)
		s_buffer = nullptr;
}

void R3DTreeFire::reset(W3DTreeBuffer *, Bool types)
{
	for (Int i = 0; i < s_fireCount; ++i)
		stopSystems(s_fires[i]);
	s_fireCount = 0;
	s_lastFrame = 0xFFFFFFFF;
	if (!types)
		return;
	// The buffer's types are gone: so are the burnt module data they pointed at.
	for (Int t = 0; t < TYPE_SLOTS; ++t)
	{
		delete s_types[t].owned;
		clearType(s_types[t]);
	}
	s_prepared = FALSE;
}

//-------------------------------------------------------------------------------------------------
void R3DTreeFire::typeAdded(W3DTreeBuffer *buffer, Int type)
{
	// Only the type just added: addTreeType answers 0 when it fails.
	if (type < 0 || type >= TYPE_SLOTS || type != buffer->m_numTreeTypes - 1)
		return;
	TypeFire &tf = s_types[type];
	const TTreeType &tt = buffer->m_treeTypes[type];
	const W3DTreeDrawModuleData *data = tt.m_data;
	clearType(tf);
	// Burnt art (the burnt stump) does not catch fire again.
	tf.flammable = data->m_flammable && !hasBurntSuffix(data->m_textureName.str());

	// The fire point: the model's AFlame01 bone, as the base game's trees used; else the trunk at FIRE_HEIGHT of its height.
	Bool found = FALSE;
	RenderObjClass *robj = WW3DAssetManager::Get_Instance()->Create_Render_Obj(data->m_modelName.str());
	if (robj != nullptr)
	{
		const Int bone = robj->Get_Bone_Index("AFlame01");
		if (bone > 0)
		{
			const Vector3 p = robj->Get_Bone_Transform(bone).Get_Translation();
			tf.fire[0] = p.X;
			tf.fire[1] = p.Y;
			tf.fire[2] = p.Z;
			found = TRUE;
		}
		REF_PTR_RELEASE(robj);
	}
	if (!found && tt.m_mesh != nullptr)
	{
		const Int count = tt.m_mesh->Peek_Model()->Get_Vertex_Count();
		const Vector3 *verts = tt.m_mesh->Peek_Model()->Get_Vertex_Array();
		Real lo = 0.0f, hi = 0.0f;
		for (Int i = 0; i < count; ++i)
		{
			if (i == 0 || verts[i].Z < lo) lo = verts[i].Z;
			if (i == 0 || verts[i].Z > hi) hi = verts[i].Z;
		}
		tf.fire[0] = tt.m_offset.X;
		tf.fire[1] = tt.m_offset.Y;
		tf.fire[2] = tt.m_offset.Z + lo + FIRE_HEIGHT * (hi - lo);
	}
	// A type added after the map's first draw gets its burnt type at once, if a slot is left.
	if (s_prepared && tf.flammable)
		addBurntType(buffer, type);
}

// The burnt type of a green one: the INI's BurntModelName / BurntTextureName, else the art's own names - the model's
// _b model (the desert plants': _d), else the texture's _b / _d. Nothing the atlas can read: the shader chars the tree.
void R3DTreeFire::addBurntType(W3DTreeBuffer *buffer, Int type)
{
	if (buffer->m_numTreeTypes >= W3DTreeBuffer::MAX_TYPES || buffer->m_numTreeTypes >= TYPE_SLOTS)
		return;
	const W3DTreeDrawModuleData *data = buffer->m_treeTypes[type].m_data;
	static const char *const SUFFIXES[2] = { "_b", "_d" };
	char model[_MAX_PATH], texture[_MAX_PATH], path[_MAX_PATH];
	snprintf(model, sizeof(model), "%s", data->m_burntModelName.str());
	snprintf(texture, sizeof(texture), "%s", data->m_burntTextureName.str());
	if (model[0] == 0 && texture[0] == 0)
	{
		for (Int s = 0; s < 2 && model[0] == 0; ++s)
		{
			snprintf(path, sizeof(path), "%s%s%s.w3d", W3D_DIR_PATH, data->m_modelName.str(), SUFFIXES[s]);
			if (TheFileSystem != nullptr && TheFileSystem->doesFileExist(path))
				snprintf(model, sizeof(model), "%s%s", data->m_modelName.str(), SUFFIXES[s]);
		}
		if (model[0] == 0)
		{
			char base[_MAX_PATH];
			snprintf(base, sizeof(base), "%s", data->m_textureName.str());
			char *dot = strrchr(base, '.');
			if (dot != nullptr)
				*dot = 0;
			for (Int s = 0; s < 2 && texture[0] == 0; ++s)
			{
				snprintf(path, sizeof(path), "%s%s.tga", base, SUFFIXES[s]);
				if (atlasCanRead(path))
					snprintf(texture, sizeof(texture), "%s", path);
			}
		}
	}
	if (model[0] != 0 && texture[0] == 0)
		modelTexture(model, texture, sizeof(texture));
	if (model[0] == 0)
		snprintf(model, sizeof(model), "%s", data->m_modelName.str());
	if (texture[0] == 0 || !atlasCanRead(texture))
		return;

	// Each green type gets its own burnt type, so a save can always name a burnt tree's green type. The atlas holds a
	// texture once, whoever shares it.
	W3DTreeDrawModuleData *burnt = new W3DTreeDrawModuleData(*data);
	burnt->m_modelName.set(model);
	burnt->m_textureName.set(texture);
	const Int before = buffer->m_numTreeTypes;
	const Int added = buffer->addTreeType(burnt);
	if (buffer->m_numTreeTypes != before + 1 || added != before)
	{
		delete burnt;
		return;
	}
	TypeFire &bt = s_types[added];
	clearType(bt);
	bt.greenType = type;
	bt.owned = burnt;
	bt.fire[0] = s_types[type].fire[0];
	bt.fire[1] = s_types[type].fire[1];
	bt.fire[2] = s_types[type].fire[2];
	s_types[type].burntType = added;
}

//-------------------------------------------------------------------------------------------------
Bool R3DTreeFire::canBurn(W3DTreeBuffer *buffer, Int tree)
{
	const TTree &t = buffer->m_trees[tree];
	return t.treeType >= 0 && t.treeType < TYPE_SLOTS && s_types[t.treeType].flammable
		&& t.m_burnState == BURN_GREEN && t.m_toppleState == TOPPLE_UPRIGHT;
}

void R3DTreeFire::firePosition(W3DTreeBuffer *buffer, Int tree, Coord3D *pos)
{
	const TTree &t = buffer->m_trees[tree];
	const Real *p = s_types[t.treeType].fire;
	pos->x = t.location.X + (p[0] * t.cos - p[1] * t.sin) * t.scale;
	pos->y = t.location.Y + (p[1] * t.cos + p[0] * t.sin) * t.scale;
	pos->z = t.location.Z + p[2] * t.scale;
}

// The burnt look: the burnt type, or the shader's char; the canopy thins on both. Once.
void R3DTreeFire::charTree(W3DTreeBuffer *buffer, Int tree)
{
	TTree &t = buffer->m_trees[tree];
	if (t.treeType < 0 || t.treeType >= TYPE_SLOTS || s_types[t.treeType].greenType >= 0 || (t.m_burnChar & CHARRED) != 0)
		return;
	const Int burnt = s_types[t.treeType].burntType;
	if (burnt >= 0)
	{
		t.treeType = burnt;
		// The bounds the cull reads, from the burnt mesh, as addTree makes them.
		t.bounds = buffer->m_treeTypes[burnt].m_bounds;
		t.bounds.Center *= t.scale;
		t.bounds.Radius *= t.scale;
		t.bounds.Center += t.location;
		t.m_burnChar = burnCode(FALSE);
	}
	else
	{
		t.m_burnChar = burnCode(TRUE);
	}
	buffer->m_anythingChanged = TRUE;
}

void R3DTreeFire::startFire(W3DTreeBuffer *buffer, Int tree)
{
	TTree &t = buffer->m_trees[tree];
	if (s_fireCount >= s_cap || s_fireCount >= MAX_FIRES || TheGameLogic == nullptr)
	{
		// More fires than the particles allow: it chars without flames, as the nuke's trees do.
		charTree(buffer, tree);
		t.m_burnState = BURN_BURNT;
		return;
	}
	const UnsignedInt now = TheGameLogic->getFrame();
	Fire &f = s_fires[s_fireCount++];
	f.tree = tree;
	f.start = now;
	f.nextSpread = now + GameClientRandomValue(SPREAD_MIN, SPREAD_MAX);
	f.stage = 0;
	for (Int i = 0; i < SYSTEMS; ++i)
		f.sys[i] = INVALID_PARTICLE_SYSTEM_ID;
	t.m_burnState = BURN_ALIGHT;
	Coord3D pos;
	firePosition(buffer, tree, &pos);
	startSystems(f, 0, &pos);
}

Int R3DTreeFire::closestToBurn(W3DTreeBuffer *buffer, const Coord3D *pos, Real range, Int skip)
{
	Int best = -1;
	Real bestSqr = range * range;
	for (Int i = 0; i < buffer->m_numTrees; ++i)
	{
		if (i == skip || !canBurn(buffer, i))
			continue;
		const TTree &t = buffer->m_trees[i];
		const Real dx = t.location.X - pos->x;
		const Real dy = t.location.Y - pos->y;
		const Real dz = t.location.Z - pos->z;
		const Real d = dx * dx + dy * dy + dz * dz;
		if (d <= bestSqr)
		{
			bestSqr = d;
			best = i;
		}
	}
	return best;
}

//-------------------------------------------------------------------------------------------------
void R3DTreeFire::update(W3DTreeBuffer *buffer, Bool mainPass)
{
	if (!s_prepared)
	{
		// Every map tree is in by the first draw, so the burnt types take only the slots the green ones left.
		s_prepared = TRUE;
		const Int greens = buffer->m_numTreeTypes;
		for (Int t = 0; t < greens && t < TYPE_SLOTS; ++t)
			if (s_types[t].greenType < 0 && s_types[t].flammable)
				addBurntType(buffer, t);
	}
	if (!mainPass || s_fireCount == 0 || TheGameLogic == nullptr)
		return;
	const UnsignedInt now = TheGameLogic->getFrame();
	if (now == s_lastFrame)
		return;
	s_lastFrame = now;

	for (Int i = 0; i < s_fireCount; )
	{
		Fire &f = s_fires[i];
		TTree &t = buffer->m_trees[f.tree];
		if (now < f.start)
			f.start = now;		// the clock went back (a replay restarted)
		const UnsignedInt age = now - f.start;
		Bool done = FALSE;
		if (t.treeType < 0 || t.m_toppleState != TOPPLE_UPRIGHT)
		{
			// Knocked down or gone: the flames go out with it.
			charTree(buffer, f.tree);
			done = TRUE;
		}
		else if (f.stage == 0 && age >= BURNED_DELAY)
		{
			charTree(buffer, f.tree);
			Coord3D pos;
			firePosition(buffer, f.tree, &pos);
			startSystems(f, 1, &pos);
			f.stage = 1;
		}
		else if (f.stage == 1 && age >= AFLAME_DURATION)
		{
			Coord3D pos;
			firePosition(buffer, f.tree, &pos);
			startSystems(f, 2, &pos);
			f.stage = 2;
			t.m_burnState = BURN_BURNT;	// burnt out; it stays under the cap while it smokes
		}
		else if (f.stage == 2 && age >= AFLAME_DURATION + SMOKE_FRAMES)
		{
			done = TRUE;
		}

		// While the flames last, the fire passes to the closest tree that can burn (FireSpreadUpdate).
		if (!done && f.stage < 2 && s_spread && now >= f.nextSpread)
		{
			f.nextSpread = now + GameClientRandomValue(SPREAD_MIN, SPREAD_MAX);
			if (s_fireCount < s_cap)
			{
				Coord3D at;
				at.set(t.location.X, t.location.Y, t.location.Z);
				const Int next = closestToBurn(buffer, &at, SPREAD_RANGE, f.tree);
				if (next >= 0)
					startFire(buffer, next);	// appends: f stays where it is
			}
		}

		if (done)
		{
			stopSystems(f);
			if (t.treeType >= 0 && t.m_burnState == BURN_ALIGHT)
				t.m_burnState = BURN_BURNT;
			s_fires[i] = s_fires[--s_fireCount];
			continue;
		}
		++i;
	}
}

Int R3DTreeFire::typeToSave(W3DTreeBuffer *buffer, Int tree)
{
	const TTree &t = buffer->m_trees[tree];
	if (t.treeType >= 0 && t.treeType < TYPE_SLOTS && t.m_burnState != BURN_GREEN && s_types[t.treeType].greenType >= 0)
		return s_types[t.treeType].greenType;
	return t.treeType;
}

//-------------------------------------------------------------------------------------------------
void R3DTreeFire::onFire(const Coord3D *pos, Real radius, Bool scorch)
{
	W3DTreeBuffer *buffer = s_buffer;
	if (buffer == nullptr || pos == nullptr || radius <= 0.0f || !s_enabled)
		return;
	const Real reach = radius + TREE_REACH;
	const Real reachSqr = reach * reach;
	for (Int i = 0; i < buffer->m_numTrees; ++i)
	{
		TTree &t = buffer->m_trees[i];
		if (t.treeType < 0 || t.treeType >= TYPE_SLOTS)
			continue;
		const Real dx = t.location.X - pos->x;
		const Real dy = t.location.Y - pos->y;
		if (dx * dx + dy * dy > reachSqr)
			continue;
		if (!scorch)
		{
			if (canBurn(buffer, i))
				startFire(buffer, i);
			continue;
		}
		// The nuke: burnt at once, no flames - its own 2003 note, "flames eat too many particles". A fire there goes out.
		if (t.m_burnState == BURN_BURNT || (t.m_burnState == BURN_GREEN && !s_types[t.treeType].flammable))
			continue;
		for (Int f = 0; f < s_fireCount; ++f)
		{
			if (s_fires[f].tree == i)
			{
				stopSystems(s_fires[f]);
				s_fires[f] = s_fires[--s_fireCount];
				break;
			}
		}
		charTree(buffer, i);
		t.m_burnState = BURN_BURNT;
	}
}

//-------------------------------------------------------------------------------------------------
void R3DTreeFire::setEnabled(Bool on)	{ s_enabled = on; }
Bool R3DTreeFire::isEnabled(void)		{ return s_enabled; }
void R3DTreeFire::setSpread(Bool on)	{ s_spread = on; }
Bool R3DTreeFire::getSpread(void)		{ return s_spread; }
Int  R3DTreeFire::getCap(void)			{ return s_cap; }

void R3DTreeFire::setCap(Int fires)
{
	s_cap = (fires < 0) ? 0 : (fires > MAX_FIRES ? (Int)MAX_FIRES : fires);
}

Real R3DTreeFire::getThin(void)			{ return s_thin; }

// The trees already burnt take the new share too.
void R3DTreeFire::setThin(Real share)
{
	s_thin = (share < 0.0f) ? 0.0f : (share > 1.0f ? 1.0f : share);
	W3DTreeBuffer *buffer = s_buffer;
	if (buffer == nullptr)
		return;
	for (Int i = 0; i < buffer->m_numTrees; ++i)
	{
		TTree &t = buffer->m_trees[i];
		if (t.treeType < 0 || t.treeType >= TYPE_SLOTS)
			continue;
		if (s_types[t.treeType].greenType >= 0)
			t.m_burnChar = burnCode(FALSE);
		else if ((t.m_burnChar & CHARRED) != 0)
			t.m_burnChar = burnCode(TRUE);
		else
			continue;
		buffer->m_anythingChanged = TRUE;
	}
}

Bool R3DTreeFire::igniteNearest(const Coord3D *pos, Real range)
{
	W3DTreeBuffer *buffer = s_buffer;
	if (buffer == nullptr || pos == nullptr)
		return FALSE;
	const Int tree = closestToBurn(buffer, pos, range, -1);
	if (tree < 0)
		return FALSE;
	startFire(buffer, tree);
	return TRUE;
}

void R3DTreeFire::stats(Int *fires, Int *burnt, Int *burntTypes, Int *types)
{
	*fires = s_fireCount;
	*burnt = 0;
	*burntTypes = 0;
	*types = 0;
	W3DTreeBuffer *buffer = s_buffer;
	if (buffer == nullptr)
		return;
	for (Int i = 0; i < buffer->m_numTrees; ++i)
		if (buffer->m_trees[i].treeType >= 0 && buffer->m_trees[i].m_burnState == BURN_BURNT)
			++*burnt;
	*types = buffer->m_numTreeTypes;
	for (Int t = 0; t < buffer->m_numTreeTypes && t < TYPE_SLOTS; ++t)
		if (s_types[t].greenType >= 0)
			++*burntTypes;
}
