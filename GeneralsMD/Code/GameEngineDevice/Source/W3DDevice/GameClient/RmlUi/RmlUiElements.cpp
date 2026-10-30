/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Ronin @feature 28/10/2026 DX9: the game's own elements for RmlUi documents. Ported from tintinhamans' GeneralsGameCode fork
// (arctic/go/feat/rmlui).
#include "W3DDevice/GameClient/RmlUi/RmlUiElements.h"

#include "Common/AsciiString.h"
#include "GameClient/GameText.h"
#include "GameClient/Image.h"
#include "W3DDevice/GameClient/RmlUi/RmlUiRenderInterface.h"
#include "W3DDevice/GameClient/RmlUi/RmlUiText.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/texture.h"
#include "W3DDevice/GameClient/W3DVideoBuffer.h"	// after the WW3D2 headers: it needs WW3DFormat

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/PropertyIdSet.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StringUtilities.h>

#include <map>

void RmlGameTextElement::OnAttributeChange(const Rml::ElementAttributes &changed_attributes)
{
	Rml::Element::OnAttributeChange(changed_attributes);
	if (changed_attributes.find("key") != changed_attributes.end())
		m_dirty = true;
}

void RmlGameTextElement::OnUpdate()
{
	Rml::Element::OnUpdate();
	if (!m_dirty)
		return;
	m_dirty = false;
	const Rml::String key = GetAttribute<Rml::String>("key", "");
	if (key.empty() || !TheGameText)
		return;
	SetInnerRML(Rml::StringUtilities::EncodeRml(unicodeToUtf8(TheGameText->fetch(AsciiString(key.c_str())))));
}

//-------------------------------------------------------------------------------------------------
// Ronin @feature 29/10/2026 DX9: <mappedimage>, his. The <img> rect is in the fixed space LoadTexture reports for engine
// textures; a leading '/' keeps RmlUi from resolving the texture name against the document's folder.
void RmlMappedImageElement::OnAttributeChange(const Rml::ElementAttributes &changed_attributes)
{
	Rml::Element::OnAttributeChange(changed_attributes);
	if (changed_attributes.find("name") != changed_attributes.end())
		m_dirty = true;
}

void RmlMappedImageElement::OnUpdate()
{
	Rml::Element::OnUpdate();
	if (m_dirty)
	{
		m_dirty = false;
		refresh();
	}
}

void RmlMappedImageElement::refresh()
{
	const Rml::String name = GetAttribute<Rml::String>("name", "");
	if (name.empty() || name == m_shownName || !TheMappedImageCollection)
		return;
	const Image *image = TheMappedImageCollection->findImageByName(AsciiString(name.c_str()));
	if (!image)
		return;

	const Region2D *uv = image->getUV();
	const float space = (float)RmlEngineTextureSpace;
	int left = (int)(uv->lo.x * space + 0.5f);
	int top = (int)(uv->lo.y * space + 0.5f);
	int right = (int)(uv->hi.x * space + 0.5f);
	int bottom = (int)(uv->hi.y * space + 0.5f);
	RmlInsetTexelRect(left, top, right, bottom, image->getTextureSize()->x, image->getTextureSize()->y);
	char rect[64];
	_snprintf_s(rect, sizeof(rect), _TRUNCATE, "%d %d %d %d", left, top, right - left, bottom - top);
	SetInnerRML("<img style=\"width:100%;height:100%;\" src=\"/" + Rml::String(image->getFilename().str()) + "\" rect=\"" + rect + "\"/>");
	m_shownName = name;
}

//-------------------------------------------------------------------------------------------------
// Ronin @feature 29/10/2026 DX9: <video>, ported from the same fork; the texture is the buffer's own D3D9 one.
static std::map<Rml::String, VideoBuffer *> &videoSources()
{
	static std::map<Rml::String, VideoBuffer *> s_sources;
	return s_sources;
}

void RmlVideoElement::setSource(const Rml::String &name, VideoBuffer *buffer)
{
	if (buffer)
		videoSources()[name] = buffer;
	else
		videoSources().erase(name);
}

RmlVideoElement::~RmlVideoElement()
{
	bind(nullptr);
}

void RmlVideoElement::OnAttributeChange(const Rml::ElementAttributes &changed_attributes)
{
	Rml::Element::OnAttributeChange(changed_attributes);
	if (changed_attributes.find("fit") != changed_attributes.end())
		m_geometryDirty = true;
}

void RmlVideoElement::OnPropertyChange(const Rml::PropertyIdSet &changed_properties)
{
	Rml::Element::OnPropertyChange(changed_properties);
	if (changed_properties.Contains(Rml::PropertyId::Opacity))
		m_geometryDirty = true;
}

void RmlVideoElement::OnResize()
{
	Rml::Element::OnResize();
	m_geometryDirty = true;
}

void RmlVideoElement::bind(VideoBuffer *buffer)
{
	m_texture = Rml::CallbackTexture();
	if (m_d3dTexture)
	{
		m_d3dTexture->Release();
		m_d3dTexture = nullptr;
	}
	m_buffer = buffer;
	m_geometryDirty = true;
	if (!buffer)
		return;

	TextureClass *texture = static_cast<W3DVideoBuffer *>(buffer)->texture();
	IDirect3DTexture9 *d3dTexture = texture ? texture->Peek_D3D_Texture() : nullptr;
	Rml::RenderManager *renderManager = GetRenderManager();
	if (!d3dTexture || !renderManager || buffer->textureWidth() == 0 || buffer->textureHeight() == 0)
		return;

	m_d3dTexture = d3dTexture;
	m_d3dTexture->AddRef();
	m_textureSize = Rml::Vector2i((int)buffer->textureWidth(), (int)buffer->textureHeight());
	m_videoSize = Rml::Vector2f((float)buffer->width(), (float)buffer->height());

	const Rml::Vector2i textureSize = m_textureSize;
	m_texture = renderManager->MakeCallbackTexture([d3dTexture, textureSize](const Rml::CallbackTextureInterface &textureInterface) -> bool {
		RmlUiRenderInterface *renderInterface = static_cast<RmlUiRenderInterface *>(Rml::GetRenderInterface());
		Rml::TextureHandle handle = renderInterface ? renderInterface->registerVideoTexture(d3dTexture) : 0;
		if (!handle)
			return false;
		textureInterface.SetTextureHandle(handle, textureSize);
		return true;
	});
}

// The movie fills only the top left of its power-of-two texture: the uv stops half a texel short of the unused part.
// fit: "fill" stretches (the .wnd way), "contain" letterboxes, "cover" crops, both keeping the movie's aspect.
void RmlVideoElement::buildGeometry()
{
	m_geometryDirty = false;
	const Rml::Vector2f box = GetBox().GetSize(Rml::BoxArea::Content);
	if (box.x <= 0.0f || box.y <= 0.0f || m_videoSize.x <= 0.0f || m_videoSize.y <= 0.0f)
	{
		m_geometry = Rml::Geometry();
		return;
	}

	Rml::Vector2f uvMax(1.0f, 1.0f);
	if (m_videoSize.x < m_textureSize.x)
		uvMax.x = (m_videoSize.x - 0.5f) / m_textureSize.x;
	if (m_videoSize.y < m_textureSize.y)
		uvMax.y = (m_videoSize.y - 0.5f) / m_textureSize.y;
	Rml::Vector2f uvMin(0.0f, 0.0f);

	Rml::Vector2f origin(0.0f, 0.0f);
	Rml::Vector2f size = box;
	const float videoAspect = m_videoSize.x / m_videoSize.y;
	const float boxAspect = box.x / box.y;
	const Rml::String fit = GetAttribute<Rml::String>("fit", "fill");
	if (fit == "contain")
	{
		if (boxAspect > videoAspect)
			size.x = box.y * videoAspect;
		else
			size.y = box.x / videoAspect;
		origin = (box - size) * 0.5f;
	}
	else if (fit == "cover")
	{
		if (boxAspect > videoAspect)
		{
			const float shown = videoAspect / boxAspect;
			uvMin.y = uvMax.y * (1.0f - shown) * 0.5f;
			uvMax.y = uvMax.y * (1.0f + shown) * 0.5f;
		}
		else
		{
			const float shown = boxAspect / videoAspect;
			uvMin.x = uvMax.x * (1.0f - shown) * 0.5f;
			uvMax.x = uvMax.x * (1.0f + shown) * 0.5f;
		}
	}

	const Rml::ColourbPremultiplied colour = Rml::Colourb(255, 255, 255, 255).ToPremultiplied(GetComputedValues().opacity());
	Rml::Mesh mesh;
	Rml::MeshUtilities::GenerateQuad(mesh, origin, size, colour, uvMin, uvMax);
	m_geometry = GetRenderManager()->MakeGeometry(std::move(mesh));
}

void RmlVideoElement::OnRender()
{
	Rml::Element::OnRender();
	std::map<Rml::String, VideoBuffer *>::const_iterator it = videoSources().find(GetAttribute<Rml::String>("source", ""));
	VideoBuffer *buffer = it != videoSources().end() ? it->second : nullptr;
	if (buffer != m_buffer)
		bind(buffer);
	if (!m_d3dTexture)
		return;
	if (m_geometryDirty)
		buildGeometry();
	if (m_geometry)
		m_geometry.Render(GetAbsoluteOffset(Rml::BoxArea::Content), m_texture);
}
