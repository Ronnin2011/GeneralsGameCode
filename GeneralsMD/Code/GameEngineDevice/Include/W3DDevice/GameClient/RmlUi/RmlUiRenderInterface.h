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

// Ronin @feature 28/10/2026 DX9: RmlUi's renderer on the D3D9 device, fixed function. Ported from tintinhamans'
// GeneralsGameCode fork (arctic/go/feat/rmlui); every state it sets is read first and put back after, so the wrapper's cache holds.
#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <map>

struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;
struct IDirect3DTexture9;

// The size every engine texture reports to RmlUi: a mapped image's sprite rect is its uv x this, whatever its page's size.
static const int RmlEngineTextureSpace = 4096;

// A sprite rect (RmlEngineTextureSpace units) pulled in by half a texel of its page, so linear filtering never takes the
// neighbouring image's edge. The page size is the one its MappedImage INI declares.
inline void RmlInsetTexelRect(int &left, int &top, int &right, int &bottom, int pageWidth, int pageHeight)
{
	if (pageWidth <= 0 || pageHeight <= 0)
		return;
	const int dx = (int)(0.5f * RmlEngineTextureSpace / pageWidth + 0.5f);
	const int dy = (int)(0.5f * RmlEngineTextureSpace / pageHeight + 0.5f);
	if (right - left > 2 * dx)
	{
		left += dx;
		right -= dx;
	}
	if (bottom - top > 2 * dy)
	{
		top += dy;
		bottom -= dy;
	}
}

class RmlUiRenderInterface : public Rml::RenderInterface
{
public:
	RmlUiRenderInterface();
	virtual ~RmlUiRenderInterface() override;

	void releaseAll();	// every buffer and texture, while the device exists

	// Bracket the context's Render(): beginFrame reads each state it changes and sets up premultiplied-alpha 2D drawing
	// over the whole target; endFrame puts every one back.
	void beginFrame(int width, int height);
	void endFrame();

	// Ronin @feature 28/10/2026 DX9: hands the device back to the engine for a drawing of its own inside a document (the
	// radar): the engine's state goes back first, RmlUi's comes again after, the scissor and the element's transform kept.
	void drawEngine(void (*draw)(void *), void *context);

	// Ronin @feature 29/10/2026 DX9: a texture the engine keeps writing (a movie's buffer), drawn as it is: video has no alpha,
	// so it samples opaque and needs no premultiply. Takes its own reference.
	Rml::TextureHandle registerVideoTexture(IDirect3DTexture9 *texture);

	virtual Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
	virtual void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
	virtual void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

	// "mapped:Name" is an INI MappedImage's page; .dds and .tga load through the engine; anything else (.png, .jpg) through stb.
	virtual Rml::TextureHandle LoadTexture(Rml::Vector2i &texture_dimensions, const Rml::String &source) override;
	virtual Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
	virtual void ReleaseTexture(Rml::TextureHandle texture) override;

	virtual void EnableScissorRegion(bool enable) override;
	virtual void SetScissorRegion(Rml::Rectanglei region) override;

	// An element's CSS transform rides on the world matrix. A transformed element's clipping is not done (no clip mask).
	virtual void SetTransform(const Rml::Matrix4f *transform) override;

private:
	struct CompiledGeometry
	{
		IDirect3DVertexBuffer9 *vb;
		IDirect3DIndexBuffer9 *ib;
		int numVertices;
		int numIndices;
	};

	struct LoadedTexture
	{
		IDirect3DTexture9 *texture;	// our own reference
		bool premultiplied;			// engine pages are straight alpha: the draw premultiplies them
	};

	Rml::TextureHandle registerTexture(IDirect3DTexture9 *texture, bool premultiplied);
	Rml::TextureHandle loadEngineTexture(Rml::Vector2i &dimensions, const Rml::String &path);
	Rml::TextureHandle loadImageFile(Rml::Vector2i &dimensions, const Rml::String &path);

	std::map<Rml::CompiledGeometryHandle, CompiledGeometry> m_geometry;
	std::map<Rml::TextureHandle, LoadedTexture> m_textures;
	Rml::CompiledGeometryHandle m_nextGeometry;
	Rml::TextureHandle m_nextTexture;

	bool m_inFrame;
	int m_width;
	int m_height;
	bool m_scissorEmpty;		// the scissor region has nothing on screen: draws are skipped
	bool m_hasTransform;
	float m_transform[4][4];	// the element's transform, rows for D3D's row vectors
};
