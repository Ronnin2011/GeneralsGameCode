/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: the modern control bar's panels the 2003 bar never had.
*/

// Ronin @feature 28/10/2026 DX9: the selected units and the control groups (docs/UI_Work.md section 10). See RmlHudPanels.h.
#include "W3DDevice/GameClient/RmlUi/RmlHudPanels.h"

#include "Common/FileSystem.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingTemplate.h"
#include "GameClient/Drawable.h"
#include "GameClient/Image.h"
#include "GameClient/InGameUI.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Squad.h"
#include "GameLogic/Module/BodyModule.h"
#include "W3DDevice/GameClient/RmlUi/RmlUiText.h"

#include <RmlUi/Core/Element.h>

#include <algorithm>
#include <map>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <windows.h>

namespace
{
	enum { SEL_BOXES = 18, GROUPS = 10, DOUBLE_CLICK_MS = 350 };
	enum { FAC_BUILDER, FAC_BARRACKS, FAC_FACTORY, FAC_AIRFIELD, FAC_KINDS };
	const char *const kFacFiles[FAC_KINDS] = { "Dozer", "Barracks", "WarFactory", "Airfield" };
	enum EntryKind { ENTRY_UNIT, ENTRY_TYPE, ENTRY_KIND, ENTRY_BACK, ENTRY_MORE };
	enum { KIND_INFANTRY, KIND_VEHICLE, KIND_AIRCRAFT, KIND_STRUCTURE, KIND_OTHER, KIND_COUNT };
	const char *const kKindNames[KIND_COUNT] = { "Infantry", "Vehicles", "Aircraft", "Structures", "Other" };

	struct SelModel
	{
		bool shown = false;
		int kind = ENTRY_UNIT;
		Rml::String icon;
		int count = 0;
		Rml::String hp = "100%";
		int hpState = 0;	// 0 green, 1 yellow, 2 red
		bool operator==(const SelModel &o) const
		{
			return shown == o.shown && kind == o.kind && icon == o.icon && count == o.count && hp == o.hp && hpState == o.hpState;
		}
		bool operator!=(const SelModel &o) const { return !(*this == o); }
	};

	struct GroupModel
	{
		bool shown = false;
		int number = 0;	// the key: 1..9, 0
		Rml::String icon;
		int count = 0;
		bool operator==(const GroupModel &o) const { return shown == o.shown && number == o.number && icon == o.icon && count == o.count; }
		bool operator!=(const GroupModel &o) const { return !(*this == o); }
	};

	struct FacModel
	{
		bool shown = false;
		Rml::String icon;
		int count = 0;
		bool operator==(const FacModel &o) const { return shown == o.shown && icon == o.icon && count == o.count; }
		bool operator!=(const FacModel &o) const { return !(*this == o); }
	};

	struct Unit
	{
		ObjectID id;
		const ThingTemplate *type;
		int kind;
		Real health;
		Real maxHealth;
	};

	// a box as shown, with what a click on it acts on
	struct Box
	{
		EntryKind what;
		std::vector<ObjectID> ids;
		const ThingTemplate *type;
		int kind;
		Real health;
		Real maxHealth;
	};

	struct State
	{
		bool shown = false;
		Rml::Vector<SelModel> sel;
		Rml::String label;
		bool groupsShown = false;
		Rml::Vector<GroupModel> groups;

		std::vector<Box> boxes;
		const ThingTemplate *openType = nullptr;	// a type opened into the boxes
		int openKind = -1;							// a kind opened into the boxes
		int page = 0;
		Rml::String hoverLabel;
		int lastGroup = -1;
		DWORD lastGroupTime = 0;

		// the one selected thing's health, on its portrait as G2 shows it
		bool unitHpShown = false;
		Rml::String unitHp;
		int unitHpState = 0;

		// the factory tabs: a tab per type of production building, the oldest type first; a click goes to the next one
		bool facShown = false;
		Rml::Vector<FacModel> facs;
		std::vector<int> facKinds;				// the kind at each tab
		std::map<int, ObjectID> facLast;		// the one a kind's tab went to last
	};
	State s;

	int kindOf(const Object *obj)
	{
		if (obj->isKindOf(KINDOF_INFANTRY)) return KIND_INFANTRY;
		if (obj->isKindOf(KINDOF_AIRCRAFT)) return KIND_AIRCRAFT;	// before vehicles: aircraft are vehicles too
		if (obj->isKindOf(KINDOF_VEHICLE)) return KIND_VEHICLE;
		if (obj->isKindOf(KINDOF_STRUCTURE)) return KIND_STRUCTURE;
		return KIND_OTHER;
	}

	Rml::String iconOf(const ThingTemplate *type)
	{
		const Image *image = type ? type->getButtonImage() : nullptr;
		return image ? Rml::String(image->getName().str()) : Rml::String();
	}

	Rml::String nameOf(const ThingTemplate *type)
	{
		return type ? unicodeToUtf8(type->getDisplayName()) : Rml::String();
	}

	// by kind, then by type, so the boxes keep their order while the selection lives
	bool unitOrder(const Unit &a, const Unit &b)
	{
		if (a.kind != b.kind)
			return a.kind < b.kind;
		if (a.type != b.type)
			return a.type && b.type && a.type->getName().compare(b.type->getName()) < 0;
		return a.id < b.id;
	}

	void collect(std::vector<Unit> &units)
	{
		units.clear();
		const DrawableList *list = TheInGameUI ? TheInGameUI->getAllSelectedLocalDrawables() : nullptr;
		if (!list)
			return;
		for (DrawableList::const_iterator it = list->begin(); it != list->end(); ++it)
		{
			const Object *obj = *it ? (*it)->getObject() : nullptr;
			if (!obj || obj->isEffectivelyDead())
				continue;
			const BodyModuleInterface *body = obj->getBodyModule();
			Unit u = { obj->getID(), obj->getTemplate(), kindOf(obj), body ? body->getHealth() : 1.0f, body ? body->getMaxHealth() : 1.0f };
			units.push_back(u);
		}
		std::sort(units.begin(), units.end(), unitOrder);
	}

	Box unitBox(const Unit &u)
	{
		Box b = { ENTRY_UNIT, std::vector<ObjectID>(1, u.id), u.type, u.kind, u.health, u.maxHealth };
		return b;
	}

	// the selection's types in order, each with its units; a type of one is a unit
	void typeBoxes(const std::vector<Unit> &units, int onlyKind, std::vector<Box> &out)
	{
		for (size_t i = 0; i < units.size();)
		{
			size_t j = i;
			Box b = { ENTRY_TYPE, std::vector<ObjectID>(), units[i].type, units[i].kind, 0.0f, 0.0f };
			for (; j < units.size() && units[j].type == units[i].type; ++j)
			{
				b.ids.push_back(units[j].id);
				b.health += units[j].health;
				b.maxHealth += units[j].maxHealth;
			}
			if (onlyKind < 0 || units[i].kind == onlyKind)
				out.push_back(b.ids.size() == 1 ? unitBox(units[i]) : b);
			i = j;
		}
	}

	void build(const std::vector<Unit> &units)
	{
		// an opened level whose units all left the selection closes
		if (s.openType && std::find_if(units.begin(), units.end(), [](const Unit &u) { return u.type == s.openType; }) == units.end())
			s.openType = nullptr;
		if (s.openKind >= 0 && std::find_if(units.begin(), units.end(), [](const Unit &u) { return u.kind == s.openKind; }) == units.end())
			s.openKind = -1;

		std::vector<Box> entries;
		if (s.openType)
		{
			for (size_t i = 0; i < units.size(); ++i)
				if (units[i].type == s.openType)
					entries.push_back(unitBox(units[i]));
		}
		else if (s.openKind >= 0)
			typeBoxes(units, s.openKind, entries);
		else if (units.size() <= SEL_BOXES)
		{
			for (size_t i = 0; i < units.size(); ++i)
				entries.push_back(unitBox(units[i]));
		}
		else
		{
			typeBoxes(units, -1, entries);
			if (entries.size() > SEL_BOXES)	// more types than boxes: by kind
			{
				entries.clear();
				std::vector<Box> types;
				typeBoxes(units, -1, types);
				for (int k = 0; k < KIND_COUNT; ++k)
				{
					Box b = { ENTRY_KIND, std::vector<ObjectID>(), nullptr, k, 0.0f, 0.0f };
					for (size_t t = 0; t < types.size(); ++t)
					{
						if (types[t].kind != k)
							continue;
						if (!b.type)
							b.type = types[t].type;	// its first type gives the kind its picture
						b.ids.insert(b.ids.end(), types[t].ids.begin(), types[t].ids.end());
						b.health += types[t].health;
						b.maxHealth += types[t].maxHealth;
					}
					if (!b.ids.empty())
						entries.push_back(b);
				}
			}
		}

		// an opened level starts with its way back; more entries than boxes page, the last box turns them
		const bool opened = s.openType || s.openKind >= 0;
		const int room = SEL_BOXES - (opened ? 1 : 0);
		s.boxes.clear();
		if (opened)
		{
			Box back = { ENTRY_BACK, std::vector<ObjectID>(), nullptr, 0, 0.0f, 0.0f };
			s.boxes.push_back(back);
		}
		if ((int)entries.size() <= room)
		{
			s.page = 0;
			s.boxes.insert(s.boxes.end(), entries.begin(), entries.end());
		}
		else
		{
			const int perPage = room - 1;
			const int pages = ((int)entries.size() + perPage - 1) / perPage;
			s.page %= pages;
			const int first = s.page * perPage;
			const int last = std::min((int)entries.size(), first + perPage);
			s.boxes.insert(s.boxes.end(), entries.begin() + first, entries.begin() + last);
			Box more = { ENTRY_MORE, std::vector<ObjectID>(), nullptr, 0, 0.0f, 0.0f };
			s.boxes.push_back(more);
		}
	}

	SelModel modelOf(const Box &b)
	{
		SelModel m;
		m.shown = true;
		m.kind = b.what;
		m.icon = iconOf(b.type);
		m.count = (b.what == ENTRY_TYPE || b.what == ENTRY_KIND) ? (int)b.ids.size() : 0;
		if (b.what == ENTRY_UNIT || b.what == ENTRY_TYPE || b.what == ENTRY_KIND)
		{
			const float f = b.maxHealth > 0.0f ? b.health / b.maxHealth : 1.0f;
			char text[16];
			_snprintf_s(text, sizeof(text), _TRUNCATE, "%.0f%%", f * 100.0f);
			m.hp = text;
			m.hpState = f > 0.5f ? 0 : (f > 0.25f ? 1 : 2);
		}
		return m;
	}

	Rml::String labelOf(const Box &b)
	{
		char text[160];
		switch (b.what)
		{
			case ENTRY_TYPE:
				_snprintf_s(text, sizeof(text), _TRUNCATE, "%s x%d", nameOf(b.type).c_str(), (int)b.ids.size());
				return text;
			case ENTRY_KIND:
				_snprintf_s(text, sizeof(text), _TRUNCATE, "%s x%d", kKindNames[b.kind], (int)b.ids.size());
				return text;
			case ENTRY_BACK:
				return "Back";
			case ENTRY_MORE:
				return "More";
			default:
				return nameOf(b.type);
		}
	}

	// the boxes' own label when the pointer is on none: the count, or the opened level
	Rml::String restingLabel(const std::vector<Unit> &units)
	{
		char text[160];
		if (s.openType)
		{
			int n = 0;
			for (size_t i = 0; i < units.size(); ++i)
				n += units[i].type == s.openType ? 1 : 0;
			_snprintf_s(text, sizeof(text), _TRUNCATE, "%s x%d", nameOf(s.openType).c_str(), n);
		}
		else if (s.openKind >= 0)
			_snprintf_s(text, sizeof(text), _TRUNCATE, "%s", kKindNames[s.openKind]);
		else
			_snprintf_s(text, sizeof(text), _TRUNCATE, "%d selected", (int)units.size());
		return text;
	}

	// as a click in the world does: the logic's group is replaced, then the drawables are marked
	void selectOnly(const std::vector<ObjectID> &ids)
	{
		if (ids.empty() || !TheInGameUI || !TheMessageStream || !TheGameLogic)
			return;
		TheInGameUI->deselectAllDrawables();
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
		msg->appendBooleanArgument(TRUE);
		for (size_t i = 0; i < ids.size(); ++i)
			msg->appendObjectIDArgument(ids[i]);
		for (size_t i = 0; i < ids.size(); ++i)
		{
			Object *obj = TheGameLogic->findObjectByID(ids[i]);
			if (obj && obj->getDrawable())
				TheInGameUI->selectDrawable(obj->getDrawable());
		}
		s.openType = nullptr;
		s.openKind = -1;
		s.page = 0;
	}

	void removeFromSelection(const std::vector<ObjectID> &ids)
	{
		if (ids.empty() || !TheInGameUI || !TheMessageStream || !TheGameLogic)
			return;
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_REMOVE_FROM_SELECTED_GROUP);
		for (size_t i = 0; i < ids.size(); ++i)
			msg->appendObjectIDArgument(ids[i]);
		for (size_t i = 0; i < ids.size(); ++i)
		{
			Object *obj = TheGameLogic->findObjectByID(ids[i]);
			if (obj && obj->getDrawable())
				TheInGameUI->deselectDrawable(obj->getDrawable());
		}
	}

	std::vector<ObjectID> idsOfType(const ThingTemplate *type)
	{
		std::vector<Unit> units;
		collect(units);
		std::vector<ObjectID> ids;
		for (size_t i = 0; i < units.size(); ++i)
			if (units[i].type == type)
				ids.push_back(units[i].id);
		return ids;
	}

	void refreshGroups(bool &dirty)
	{
		Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		bool any = false;
		for (int j = 0; j < GROUPS; ++j)
		{
			const int number = (j + 1) % 10;	// shown 1..9 then 0, as the keys run
			GroupModel g;
			g.number = number;
			Squad *squad = player ? player->getHotkeySquad(number) : nullptr;
			if (squad)
			{
				const VecObjectPtr &objects = squad->getLiveObjects();
				std::map<const ThingTemplate *, int> counts;
				const ThingTemplate *most = nullptr;
				for (size_t i = 0; i < objects.size(); ++i)
				{
					const ThingTemplate *type = objects[i] ? objects[i]->getTemplate() : nullptr;
					if (!type)
						continue;
					const int n = ++counts[type];
					if (!most || n > counts[most])
						most = type;
					++g.count;
				}
				g.icon = iconOf(most);
			}
			g.shown = g.count > 0;
			any = any || g.shown;
			if (g != s.groups[j])
			{
				s.groups[j] = g;
				dirty = true;
			}
		}
		if (any != s.groupsShown)
		{
			s.groupsShown = any;
			dirty = true;
		}
	}

	// "250/250" for the one selected thing, whoever owns it; rounded up, so a living unit never reads 0
	void refreshUnitHealth(Rml::DataModelHandle &model)
	{
		bool shown = false;
		Rml::String text;
		int state = 0;
		const DrawableList *all = TheInGameUI ? TheInGameUI->getAllSelectedDrawables() : nullptr;
		if (all && all->size() == 1)
		{
			const Object *obj = all->front() ? all->front()->getObject() : nullptr;
			const BodyModuleInterface *body = obj ? obj->getBodyModule() : nullptr;
			if (body && body->getMaxHealth() > 0.0f)
			{
				const Real health = body->getHealth();
				const Real maxHealth = body->getMaxHealth();
				char buffer[48];
				_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%d/%d", (int)ceilf(health), (int)ceilf(maxHealth));
				text = buffer;
				const Real f = health / maxHealth;
				state = f > 0.5f ? 0 : (f > 0.25f ? 1 : 2);
				shown = true;
			}
		}
		if (shown != s.unitHpShown)
		{
			s.unitHpShown = shown;
			model.DirtyVariable("unit_hp_shown");
		}
		if (text != s.unitHp)
		{
			s.unitHp = text;
			model.DirtyVariable("unit_hp");
		}
		if (state != s.unitHpState)
		{
			s.unitHpState = state;
			model.DirtyVariable("unit_hp_state");
		}
	}

	// Ronin @feature 29/10/2026 DX9: the factory tabs are four kinds, whatever the side: the builders (dozers, workers), the
	// barracks, the war factories (arms dealers too) and the airfields - one tab each, even across factions.
	int factoryKind(const Object *obj)
	{
		if (!obj || obj->isEffectivelyDead() || obj->testStatus(OBJECT_STATUS_SOLD) || obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			return -1;
		if (obj->isKindOf(KINDOF_STRUCTURE))
		{
			if (obj->isKindOf(KINDOF_FS_BARRACKS)) return FAC_BARRACKS;
			if (obj->isKindOf(KINDOF_FS_WARFACTORY)) return FAC_FACTORY;
			if (obj->isKindOf(KINDOF_FS_AIRFIELD)) return FAC_AIRFIELD;
			return -1;
		}
		return (obj->isKindOf(KINDOF_DOZER) && !obj->getContainedBy()) ? FAC_BUILDER : -1;	// one in a transport cannot be gone to
	}

	void collectFactory(Object *obj, void *userData)
	{
		std::vector<Object *> *out = (std::vector<Object *> *)userData;
		if (factoryKind(obj) >= 0)
			out->push_back(obj);
	}

	bool idOrder(const Object *a, const Object *b)
	{
		return a->getID() < b->getID();
	}

	void factories(std::vector<Object *> &out)
	{
		out.clear();
		Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		if (player)
			player->iterateObjects(collectFactory, &out);
		std::sort(out.begin(), out.end(), idOrder);	// the oldest first: a tab goes round them in that order
	}

	// A kind's picture: G2's production button in Data/UI/Local when there (art that never ships), else the first one's cameo.
	Rml::String factoryIcon(int kind, const Object *first)
	{
		static int s_local[FAC_KINDS] = { -1, -1, -1, -1 };	// -1 not looked for yet
		if (s_local[kind] < 0)
		{
			char path[128];
			_snprintf_s(path, sizeof(path), _TRUNCATE, "Data/UI/Local/ProductionButtons/%s.png", kFacFiles[kind]);
			s_local[kind] = (TheFileSystem && TheFileSystem->doesFileExist(path)) ? 1 : 0;
		}
		if (s_local[kind] == 1)
		{
			char source[128];
			_snprintf_s(source, sizeof(source), _TRUNCATE, "Local/ProductionButtons/%s.png", kFacFiles[kind]);	// from Data/UI
			return source;
		}
		return iconOf(first ? first->getTemplate() : nullptr);
	}

	void refreshFactories(Rml::DataModelHandle &model)
	{
		std::vector<Object *> objects;
		factories(objects);
		int counts[FAC_KINDS] = { 0, 0, 0, 0 };
		const Object *firsts[FAC_KINDS] = { nullptr, nullptr, nullptr, nullptr };
		for (size_t i = 0; i < objects.size(); ++i)
		{
			const int kind = factoryKind(objects[i]);
			if (kind < 0)
				continue;
			if (!firsts[kind])
				firsts[kind] = objects[i];
			++counts[kind];
		}

		// the tabs in the kinds' order, the ones there are
		s.facKinds.clear();
		bool dirty = false;
		for (int kind = 0; kind < FAC_KINDS; ++kind)
		{
			if (counts[kind] == 0)
				continue;
			FacModel f;
			f.shown = true;
			f.icon = factoryIcon(kind, firsts[kind]);
			f.count = counts[kind];
			const int tab = (int)s.facKinds.size();
			s.facKinds.push_back(kind);
			if (f != s.facs[tab])
			{
				s.facs[tab] = f;
				dirty = true;
			}
		}
		for (int tab = (int)s.facKinds.size(); tab < FAC_KINDS; ++tab)
		{
			if (s.facs[tab] != FacModel())
			{
				s.facs[tab] = FacModel();
				dirty = true;
			}
		}
		if (dirty)
			model.DirtyVariable("facs");
		const bool shown = !s.facKinds.empty();
		if (shown != s.facShown)
		{
			s.facShown = shown;
			model.DirtyVariable("fac_shown");
		}
	}

	// a tab: the next of its kind after the one it went to last, selected and looked at
	void goToNextFactory(int tab)
	{
		if (tab < 0 || tab >= (int)s.facKinds.size())
			return;
		const int kind = s.facKinds[tab];
		std::vector<Object *> objects;
		factories(objects);
		Object *first = nullptr;
		Object *next = nullptr;
		const ObjectID last = s.facLast.count(kind) ? s.facLast[kind] : INVALID_ID;
		for (size_t i = 0; i < objects.size(); ++i)
		{
			if (factoryKind(objects[i]) != kind)
				continue;
			if (!first)
				first = objects[i];
			if (!next && objects[i]->getID() > last)
				next = objects[i];
		}
		Object *target = next ? next : first;
		if (!target)
			return;
		s.facLast[kind] = target->getID();
		selectOnly(std::vector<ObjectID>(1, target->getID()));
		if (TheTacticalView)
			TheTacticalView->userLookAt(target->getPosition());
	}

	// data-hud="s3": selection box 3; "g5": control group box 5; "f2": factory tab 2
	bool hudSlot(Rml::Element *element, char &panel, int &index)
	{
		for (Rml::Element *e = element; e; e = e->GetParentNode())
		{
			const Rml::Variant *attribute = e->GetAttribute("data-hud");
			if (!attribute)
				continue;
			const Rml::String value = attribute->Get<Rml::String>();
			if (value.size() < 2)
				return false;
			panel = value[0];
			index = atoi(value.c_str() + 1);
			return true;
		}
		return false;
	}
}

//-------------------------------------------------------------------------------------------------
void RmlHudPanels::bind(Rml::DataModelConstructor &constructor)
{
	if (Rml::StructHandle<SelModel> sel = constructor.RegisterStruct<SelModel>())
	{
		sel.RegisterMember("shown", &SelModel::shown);
		sel.RegisterMember("kind", &SelModel::kind);
		sel.RegisterMember("icon", &SelModel::icon);
		sel.RegisterMember("count", &SelModel::count);
		sel.RegisterMember("hp", &SelModel::hp);
		sel.RegisterMember("hp_state", &SelModel::hpState);
	}
	constructor.RegisterArray<Rml::Vector<SelModel>>();
	if (Rml::StructHandle<GroupModel> group = constructor.RegisterStruct<GroupModel>())
	{
		group.RegisterMember("shown", &GroupModel::shown);
		group.RegisterMember("number", &GroupModel::number);
		group.RegisterMember("icon", &GroupModel::icon);
		group.RegisterMember("count", &GroupModel::count);
	}
	constructor.RegisterArray<Rml::Vector<GroupModel>>();

	s.sel.assign(SEL_BOXES, SelModel());
	s.groups.assign(GROUPS, GroupModel());
	constructor.Bind("sel_shown", &s.shown);
	constructor.Bind("sel", &s.sel);
	constructor.Bind("sel_label", &s.label);
	constructor.Bind("groups_shown", &s.groupsShown);
	constructor.Bind("groups", &s.groups);
	constructor.Bind("unit_hp_shown", &s.unitHpShown);
	constructor.Bind("unit_hp", &s.unitHp);
	constructor.Bind("unit_hp_state", &s.unitHpState);

	if (Rml::StructHandle<FacModel> fac = constructor.RegisterStruct<FacModel>())
	{
		fac.RegisterMember("shown", &FacModel::shown);
		fac.RegisterMember("icon", &FacModel::icon);
		fac.RegisterMember("count", &FacModel::count);
	}
	constructor.RegisterArray<Rml::Vector<FacModel>>();
	s.facs.assign(FAC_KINDS, FacModel());
	constructor.Bind("fac_shown", &s.facShown);
	constructor.Bind("facs", &s.facs);
}

void RmlHudPanels::refresh(Rml::DataModelHandle &model)
{
	if (!model)
		return;
	std::vector<Unit> units;
	collect(units);

	// two or more: the boxes take the portrait's place
	const bool shown = units.size() >= 2;
	if (shown != s.shown)
	{
		s.shown = shown;
		model.DirtyVariable("sel_shown");
	}
	if (!shown)
	{
		s.openType = nullptr;
		s.openKind = -1;
		s.page = 0;
		s.boxes.clear();
	}
	else
		build(units);

	bool dirty = false;
	for (int i = 0; i < SEL_BOXES; ++i)
	{
		const SelModel m = i < (int)s.boxes.size() ? modelOf(s.boxes[i]) : SelModel();
		if (m != s.sel[i])
		{
			s.sel[i] = m;
			dirty = true;
		}
	}
	if (dirty)
		model.DirtyVariable("sel");

	const Rml::String label = !s.hoverLabel.empty() ? s.hoverLabel : (shown ? restingLabel(units) : Rml::String());
	if (label != s.label)
	{
		s.label = label;
		model.DirtyVariable("sel_label");
	}

	dirty = false;
	const bool groupsShown = s.groupsShown;
	refreshGroups(dirty);
	if (dirty)
		model.DirtyVariable("groups");
	if (groupsShown != s.groupsShown)
		model.DirtyVariable("groups_shown");

	refreshUnitHealth(model);
	refreshFactories(model);
}

void RmlHudPanels::hover(Rml::Element *element)
{
	char panel = 0;
	int index = 0;
	s.hoverLabel.clear();
	if (!hudSlot(element, panel, index))
		return;
	if (panel == 's' && index >= 0 && index < (int)s.boxes.size())
		s.hoverLabel = labelOf(s.boxes[index]);
}

bool RmlHudPanels::click(Rml::Element *element, int button)
{
	char panel = 0;
	int index = 0;
	if (!hudSlot(element, panel, index))
		return false;
	if (button != 0)
		return true;	// ours, but only the left button does anything
	const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
	const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

	if (panel == 'g' && index >= 0 && index < GROUPS && TheMessageStream)
	{
		if (!s.groups[index].shown)
			return true;	// an empty group's tab marks its place; its key would only clear the selection
		// as the number key: select; with Shift add; a second click soon after looks at the group
		const int number = s.groups[index].number;
		const DWORD now = ::GetTickCount();
		const bool twice = index == s.lastGroup && now - s.lastGroupTime < DOUBLE_CLICK_MS;
		s.lastGroup = index;
		s.lastGroupTime = now;
		const GameMessage::Type base = twice ? GameMessage::MSG_META_VIEW_TEAM0 : (shift ? GameMessage::MSG_META_ADD_TEAM0 : GameMessage::MSG_META_SELECT_TEAM0);
		TheMessageStream->appendMessage((GameMessage::Type)(base + number));
		return true;
	}

	if (panel == 'f')
	{
		goToNextFactory(index);
		return true;
	}

	if (panel != 's' || index < 0 || index >= (int)s.boxes.size())
		return true;
	const Box b = s.boxes[index];
	switch (b.what)
	{
		case ENTRY_BACK:	// a type opened from a kind goes back to the kind
			if (s.openType)
				s.openType = nullptr;
			else
				s.openKind = -1;
			s.page = 0;
			break;
		case ENTRY_MORE:
			++s.page;
			break;
		case ENTRY_KIND:
			if (ctrl)
				selectOnly(b.ids);
			else if (shift)
				removeFromSelection(b.ids);
			else
			{
				s.openKind = b.kind;
				s.page = 0;
			}
			break;
		case ENTRY_TYPE:	// opens into the boxes; Ctrl keeps only these, Shift drops them
			if (ctrl)
				selectOnly(b.ids);
			else if (shift)
				removeFromSelection(b.ids);
			else
			{
				s.openType = b.type;
				s.page = 0;
			}
			break;
		default:			// a unit: only it; Shift drops it; Ctrl keeps its whole type
			if (shift)
				removeFromSelection(b.ids);
			else if (ctrl)
				selectOnly(idsOfType(b.type));
			else
				selectOnly(b.ids);
			break;
	}
	return true;
}

void RmlHudPanels::reset()
{
	s.openType = nullptr;
	s.openKind = -1;
	s.page = 0;
	s.hoverLabel.clear();
	s.boxes.clear();
}
