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

 /***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WW3D                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/render2d.cpp                           $*
 *                                                                                             *
 *                   $Org Author:: Byon_g                                                                                                                                                                                  $Modtime:: 1/24/01 3:54p                                               $*
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 08/05/02 2:40p                                              $*
 *                                                                                             *
 *                    $Revision:: 48                                                          $*
 *                                                                                             *
 * 06/26/02 KM Matrix name change to avoid MAX conflicts                                       *
 * 08/05/02 KM Texture class redesign
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */


#include <d3d9.h>  // Native DX9

#include "always.h"
#include "render2d.h"
#include "mutex.h"
#include "ww3d.h"
#include "font3d.h"
#include "rect.h"
#include "texture.h"
#include "matrix4.h"
#include "matrix3d.h"
#include "dx8wrapper.h"
#include "dx8indexbuffer.h"
#include "dx8vertexbuffer.h"
#include "sortingrenderer.h"
#include "vertmaterial.h"
#include "dx8fvf.h"
#include "dx8caps.h"
#include "wwprofile.h"
#include "wwmemlog.h"
#include "assetmgr.h"

#include "Scoped2DStateGuard.h"

//#include <mmsystem.h>
//#pragma comment(lib, "winmm.lib")

RectClass							Render2DClass::ScreenResolution( 0,0,0,0 );


/*
** Render2DClass
*/
Render2DClass::Render2DClass( TextureClass* tex ) :
	CoordinateScale( 1, 1 ),
	CoordinateOffset( 0, 0 ),
	Texture(nullptr),
	ZValue(0),
	IsHidden( false ),
	IsGrayScale (false),
	Indices(sizeof(PreAllocatedIndices)/sizeof(unsigned short),PreAllocatedIndices),
	Vertices(sizeof(PreAllocatedVertices)/sizeof(Vector2),PreAllocatedVertices),
	UVCoordinates(sizeof(PreAllocatedUVCoordinates)/sizeof(Vector2),PreAllocatedUVCoordinates),
	Colors(sizeof(PreAllocatedColors)/sizeof(unsigned long),PreAllocatedColors)
{
	Set_Texture( tex );
   Shader = Get_Default_Shader();
	return ;
}

Render2DClass::~Render2DClass()
{
	REF_PTR_RELEASE(Texture);
}

void	Render2DClass::Set_Screen_Resolution( const RectClass & screen )
{
	ScreenResolution = screen;
#if 0
	// Fool into pixel doubling  - Byon..
	if ( screen.Width() >= 1280 ) {
		ScreenResolution.Scale( 0.5f );
	}
//	ScreenResolution = RectClass( 0, 0, 800, 600 );
#endif
}

ShaderClass
Render2DClass::Get_Default_Shader( void )
{
	ShaderClass shader;

   shader.Set_Depth_Mask( ShaderClass::DEPTH_WRITE_DISABLE );
	shader.Set_Depth_Compare( ShaderClass::PASS_ALWAYS );
	shader.Set_Dst_Blend_Func( ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA );
	shader.Set_Src_Blend_Func( ShaderClass::SRCBLEND_SRC_ALPHA );
	shader.Set_Fog_Func( ShaderClass::FOG_DISABLE );
	shader.Set_Primary_Gradient( ShaderClass::GRADIENT_MODULATE );
	shader.Set_Texturing( ShaderClass::TEXTURING_ENABLE );

	return shader;
}

void	Render2DClass::Reset(void)
{
	Vertices.Reset_Active();
	UVCoordinates.Reset_Active();
	Colors.Reset_Active();
	Indices.Reset_Active();

	Update_Bias(); // Keep the bias updated
}

void Render2DClass::Set_Texture(TextureClass* tex)
{
	REF_PTR_SET(Texture,tex);
}

void Render2DClass::Set_Texture( const char * filename)
{
	TextureClass * tex = WW3DAssetManager::Get_Instance()->Get_Texture( filename, MIP_LEVELS_1 );
	Set_Texture( tex );
	if ( tex != nullptr ) {
		SET_REF_OWNER( tex );
		tex->Release_Ref();
	}
}

/**added for generals to draw disabled button states - MW*/
void Render2DClass::Enable_Grayscale(bool b)
{
	IsGrayScale = b;
}

void Render2DClass::Enable_Alpha(bool b)
{
	IsGrayScale = false;
	if (b) {
		Shader.Set_Dst_Blend_Func( ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA );
		Shader.Set_Src_Blend_Func( ShaderClass::SRCBLEND_SRC_ALPHA );
	}
	else {
		Shader.Set_Src_Blend_Func( ShaderClass::SRCBLEND_ONE);
		Shader.Set_Dst_Blend_Func( ShaderClass::DSTBLEND_ZERO );
	}
}

void Render2DClass::Enable_Additive(bool b)
{
	IsGrayScale = false;

	if (b) {
		Shader.Set_Dst_Blend_Func( ShaderClass::DSTBLEND_ONE );
		Shader.Set_Src_Blend_Func( ShaderClass::SRCBLEND_ONE );
	}
	else {
		Shader.Set_Src_Blend_Func( ShaderClass::SRCBLEND_ONE);
		Shader.Set_Dst_Blend_Func( ShaderClass::DSTBLEND_ZERO );
	}
}

void Render2DClass::Enable_Texturing(bool b)
{
	if (b) {
		Shader.Set_Texturing( ShaderClass::TEXTURING_ENABLE );
	}
	else {
		Shader.Set_Texturing( ShaderClass::TEXTURING_DISABLE );
	}
}

void	Render2DClass::Set_Coordinate_Range( const RectClass & range )
{
	// default range is (-1,1)-(1,-1)
	CoordinateScale.X = 2 / range.Width();
	CoordinateScale.Y = -2 / range.Height();
	CoordinateOffset.X = -(CoordinateScale.X * range.Left) - 1;
	CoordinateOffset.Y = -(CoordinateScale.Y * range.Top) + 1;

	Update_Bias();
}

void	  Render2DClass::Update_Bias( void )
{

	BiasedCoordinateOffset = CoordinateOffset;

	if ( WW3D::Is_Screen_UV_Biased() ) {	// Global bais setting
		Vector2 bais_add( -0.5f ,-0.5f );	// offset by -0.5,-0.5 in pixels

		// Convert from pixels to (-1,1)-(1,-1) units
		bais_add.X = bais_add.X / (Get_Screen_Resolution().Width() * 0.5f);
		bais_add.Y = bais_add.Y / (Get_Screen_Resolution().Height() * -0.5f);

		BiasedCoordinateOffset += bais_add;
	}
}

#if 0
Vector2 Render2DClass::Convert_Vert( const Vector2 & v )
{
	Vector2 out;

	// Convert to (-1,1)-(1,-1)
	out.X = v.X * CoordinateScale.X + CoordinateOffset.X;
	out.Y = v.Y * CoordinateScale.Y + CoordinateOffset.Y;

	// Convert to pixels
	out.X = (out.X + 1.0f) * (Get_Screen_Resolution().Width() * 0.5f);
	out.Y = (out.Y - 1.0f) * (Get_Screen_Resolution().Height() * -0.5f);

	// Round to nearest pixel
	out.X = WWMath::Floor( out.X + 0.5f );
	out.Y = WWMath::Floor( out.Y + 0.5f );

	// Bias
	if ( WW3D::Is_Screen_UV_Biased() ) {	// Global bais setting
		out.X -= 0.5f;
		out.Y -= 0.5f;
	}


	// Convert back to (-1,1)-(1,-1)
	out.X = out.X / (Get_Screen_Resolution().Width() * 0.5f) - 1.0f;
	out.Y = out.Y / (Get_Screen_Resolution().Height() * -0.5f) + 1.0f;

	return out;
}
#else
/*
** Convert Vert must convert from the convention defined by Set_Coordinate_Range
** into the convention (-1,1)-(1,-1), which is needed by the renderer.
// NOPE ** In addition, it rounds all coordinates off to the nearest pixel
** Also, it offsets the coordinates as need for Screen_UV_Bias
*/
void Render2DClass::Convert_Vert( Vector2 & vert_out, const Vector2 & vert_in )
{
	// Convert to (-1,1)-(1,-1)
	vert_out.X = vert_in.X * CoordinateScale.X + BiasedCoordinateOffset.X;
	vert_out.Y = vert_in.Y * CoordinateScale.Y + BiasedCoordinateOffset.Y;
}

void Render2DClass::Convert_Vert( Vector2 & vert_out, float x_in, float y_in )
{
	// Convert to (-1,1)-(1,-1)
	vert_out.X = x_in * CoordinateScale.X + BiasedCoordinateOffset.X;
	vert_out.Y = y_in * CoordinateScale.Y + BiasedCoordinateOffset.Y;
}

#endif

void	Render2DClass::Move( const Vector2 & move )	// Move all verts
{
	Vector2 scaled_move;
	scaled_move.X = move.X * CoordinateScale.X;
	scaled_move.Y = move.Y * CoordinateScale.Y;
	for ( int i = 0; i < Vertices.Count(); i++ ) {
		Vertices[i] += scaled_move;
	}
}

void	Render2DClass::Force_Alpha( float alpha )		// Force all alphas
{
	unsigned long a = (unsigned)(WWMath::Clamp( alpha, 0, 1 ) * 255.0f);
	a <<= 24;
	for ( int i = 0; i < Colors.Count(); i++ ) {
		Colors[i] = (Colors[i] & 0x00FFFFFF) | a;
	}
}


void	Render2DClass::Force_Color( int color )		// Force all alphas
{
	for ( int i = 0; i < Colors.Count(); i++ ) {
		Colors[i] = color;
	}
}


/*
** Internal Add Quad Elements
** Caller must mutex lock
*/
void	Render2DClass::Internal_Add_Quad_Vertices( const Vector2 & v0, const Vector2 & v1, const Vector2 & v2, const Vector2 & v3 )
{
	Convert_Vert( *Vertices.Uninitialized_Add(), v0 );
	Convert_Vert( *Vertices.Uninitialized_Add(), v1 );
	Convert_Vert( *Vertices.Uninitialized_Add(), v2 );
	Convert_Vert( *Vertices.Uninitialized_Add(), v3 );
}

void	Render2DClass::Internal_Add_Quad_Vertices( const RectClass & screen )
{
	Convert_Vert( *Vertices.Uninitialized_Add(), screen.Left,  screen.Top );
	Convert_Vert( *Vertices.Uninitialized_Add(), screen.Left,  screen.Bottom );
	Convert_Vert( *Vertices.Uninitialized_Add(), screen.Right, screen.Top );
	Convert_Vert( *Vertices.Uninitialized_Add(), screen.Right, screen.Bottom );

}

void	Render2DClass::Internal_Add_Quad_UVs( const RectClass & uv )
{
	Vector2* uvs;

	uvs=UVCoordinates.Uninitialized_Add();
	uvs->X = uv.Left;		uvs->Y = uv.Top;
	uvs=UVCoordinates.Uninitialized_Add();
	uvs->X = uv.Left;		uvs->Y = uv.Bottom;
	uvs=UVCoordinates.Uninitialized_Add();
	uvs->X = uv.Right;	uvs->Y = uv.Top;
	uvs=UVCoordinates.Uninitialized_Add();
	uvs->X = uv.Right;	uvs->Y = uv.Bottom;

}

void	Render2DClass::Internal_Add_Quad_Colors( unsigned long color )
{
	unsigned long* colors;

	colors=Colors.Uninitialized_Add();
	*colors=color;
	colors=Colors.Uninitialized_Add();
	*colors=color;
	colors=Colors.Uninitialized_Add();
	*colors=color;
	colors=Colors.Uninitialized_Add();
	*colors=color;
}

void	Render2DClass::Internal_Add_Quad_VColors( unsigned long color1, unsigned long color2 )
{
	unsigned long* colors;

	colors=Colors.Uninitialized_Add();
	*colors=color1;
	colors=Colors.Uninitialized_Add();
	*colors=color2;
	colors=Colors.Uninitialized_Add();
	*colors=color1;
	colors=Colors.Uninitialized_Add();
	*colors=color2;

}

void	Render2DClass::Internal_Add_Quad_HColors( unsigned long color1, unsigned long color2 )
{
	unsigned long* colors;

	colors=Colors.Uninitialized_Add();
	*colors=color1;
	colors=Colors.Uninitialized_Add();
	*colors=color1;
	colors=Colors.Uninitialized_Add();
	*colors=color2;
	colors=Colors.Uninitialized_Add();
	*colors=color2;
}


void	Render2DClass::Internal_Add_Quad_Indicies( int start_vert_index, bool backfaced )
{
	unsigned short * indices;

	if (backfaced ^ (CoordinateScale.X * CoordinateScale.Y > 0)) {
		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 1;
		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 0;
		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 2;

		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 1;
		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 2;
		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 3;
	} else {
		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 0;
		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 1;
		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 2;

		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 2;
		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 1;
		indices=Indices.Uninitialized_Add();
		*indices = start_vert_index + 3;
	}

}


void	Render2DClass::Add_Quad( const Vector2 & v0, const Vector2 & v1, const Vector2 & v2, const Vector2 & v3, const RectClass & uv, unsigned long color )
{
	Internal_Add_Quad_Indicies( Vertices.Count() );
	Internal_Add_Quad_Vertices( v0, v1, v2, v3 );
	Internal_Add_Quad_UVs( uv );
	Internal_Add_Quad_Colors( color );
}

void	Render2DClass::Add_Quad_Backfaced( const Vector2 & v0, const Vector2 & v1, const Vector2 & v2, const Vector2 & v3, const RectClass & uv, unsigned long color )
{
	Internal_Add_Quad_Indicies( Vertices.Count(), true );
	Internal_Add_Quad_Vertices( v0, v1, v2, v3 );
	Internal_Add_Quad_UVs( uv );
	Internal_Add_Quad_Colors( color );
}

void	Render2DClass::Add_Quad_VGradient( const Vector2 & v0, const Vector2 & v1, const Vector2 & v2, const Vector2 & v3, const RectClass & uv, unsigned long top_color, unsigned long bottom_color )
{
	Internal_Add_Quad_Indicies( Vertices.Count() );
	Internal_Add_Quad_Vertices( v0, v1, v2, v3 );
	Internal_Add_Quad_UVs( uv );
	Internal_Add_Quad_VColors( top_color, bottom_color );
}

void	Render2DClass::Add_Quad_HGradient( const Vector2 & v0, const Vector2 & v1, const Vector2 & v2, const Vector2 & v3, const RectClass & uv, unsigned long left_color, unsigned long right_color )
{
	Internal_Add_Quad_Indicies( Vertices.Count() );
	Internal_Add_Quad_Vertices( v0, v1, v2, v3 );
	Internal_Add_Quad_UVs( uv );
	Internal_Add_Quad_HColors( left_color, right_color );
}


void	Render2DClass::Add_Quad_VGradient( const RectClass & screen, unsigned long top_color, unsigned long bottom_color )
{
	Internal_Add_Quad_Indicies( Vertices.Count() );
	Internal_Add_Quad_Vertices( screen );
	Internal_Add_Quad_UVs( RectClass( 0,0,1,1 ) );
	Internal_Add_Quad_VColors( top_color, bottom_color );
}

void	Render2DClass::Add_Quad_HGradient( const RectClass & screen, unsigned long left_color, unsigned long right_color )
{
	Internal_Add_Quad_Indicies( Vertices.Count() );
	Internal_Add_Quad_Vertices( screen );
	Internal_Add_Quad_UVs( RectClass( 0,0,1,1 ) );
	Internal_Add_Quad_HColors( left_color, right_color );
}


void	Render2DClass::Add_Quad( const RectClass & screen, const RectClass & uv, unsigned long color )
{
	Internal_Add_Quad_Indicies( Vertices.Count() );
	Internal_Add_Quad_Vertices( screen );
	Internal_Add_Quad_UVs( uv );
	Internal_Add_Quad_Colors( color );
}

void	Render2DClass::Add_Quad( const Vector2 & v0, const Vector2 & v1, const Vector2 & v2, const Vector2 & v3, unsigned long color )
{
	Internal_Add_Quad_Indicies( Vertices.Count() );
	Internal_Add_Quad_Vertices( v0, v1, v2, v3 );
	Internal_Add_Quad_UVs( RectClass( 0,0,1,1 ) );
	Internal_Add_Quad_Colors( color );
}

void	Render2DClass::Add_Quad( const RectClass & screen, unsigned long color )
{
	Internal_Add_Quad_Indicies( Vertices.Count() );
	Internal_Add_Quad_Vertices( screen );
	Internal_Add_Quad_UVs( RectClass( 0,0,1,1 ) );
	Internal_Add_Quad_Colors( color );
}

/*
** Add Tri
*/
void	Render2DClass::Add_Tri( const Vector2 & v0, const Vector2 & v1, const Vector2 & v2, const Vector2 & uv0, const Vector2 & uv1, const Vector2 & uv2, unsigned long color )
{
	int old_vert_count = Vertices.Count();

	// Add the vertices (translated to new coordinates)
#if 0
	Vertices.Add( Convert_Vert( v0 ), new_vert_count );
	Vertices.Add( Convert_Vert( v1 ), new_vert_count );
	Vertices.Add( Convert_Vert( v2 ), new_vert_count );
#else
	Convert_Vert( *Vertices.Uninitialized_Add(), v0 );
	Convert_Vert( *Vertices.Uninitialized_Add(), v1 );
	Convert_Vert( *Vertices.Uninitialized_Add(), v2 );

#endif

	// Add the uv coordinates

	*UVCoordinates.Uninitialized_Add()=uv0;
	*UVCoordinates.Uninitialized_Add()=uv1;
	*UVCoordinates.Uninitialized_Add()=uv2;

	// Add the colors
	*Colors.Uninitialized_Add()=color;
	*Colors.Uninitialized_Add()=color;
	*Colors.Uninitialized_Add()=color;

	// Add the faces
	*Indices.Uninitialized_Add()=old_vert_count + 0;
	*Indices.Uninitialized_Add()=old_vert_count + 1;
	*Indices.Uninitialized_Add()=old_vert_count + 2;

}

void	Render2DClass::Add_Line( const Vector2 & a, const Vector2 & b, float width, unsigned long color )
{
	Add_Line( a, b, width, RectClass( 0,0,1,1 ), color );
}

void	Render2DClass::Add_Line( const Vector2 & a, const Vector2 & b, float width, const RectClass & uv, unsigned long color )
{
	Vector2	corner_offset = a - b;				// get line relative to b
	float temp = corner_offset.X;					// Rotate 90
	corner_offset.X = corner_offset.Y;
	corner_offset.Y = -temp;
	corner_offset.Normalize();						// scale to length width/2
	corner_offset *= width / 2;

	Add_Quad( a - corner_offset, a + corner_offset, b - corner_offset, b + corner_offset, uv, color );
	//Add_Quad_HGradient(RectClass( a.X -corner_offset.X ,a.Y,b.X,b.Y ), color, color2);
	//Add_Quad_HGradient( const RectClass & screen, unsigned long left_color, unsigned long right_color );
}


void	Render2DClass::Add_Line( const Vector2 & a, const Vector2 & b, float width, unsigned long color, unsigned long color2 )
{
	Add_Line( a, b, width, RectClass( 0,0,1,1 ), color, color2 );
}

void Render2DClass::Add_Line( const Vector2 & a, const Vector2 & b, float width, const RectClass & uv, unsigned long color , unsigned long color2)
{
	Vector2	corner_offset = a - b;				// get line relative to b
	float temp = corner_offset.X;					// Rotate 90
	corner_offset.X = corner_offset.Y;
	corner_offset.Y = -temp;
	corner_offset.Normalize();						// scale to length width/2
	corner_offset *= width / 2;

	Add_Quad_HGradient( a - corner_offset, a + corner_offset, b - corner_offset, b + corner_offset, uv, color,color2);
	//Add_Quad_HGradient(RectClass( a.X -corner_offset.X ,a.Y,b.X,b.Y ), color, color2);
}


void	Render2DClass::Add_Rect( const RectClass & rect, float border_width, uint32 border_color, uint32 fill_color )
{
	//
	//	First add the outline
	//
	if( border_width > 0 )
	Add_Outline( rect, border_width, border_color );

	//
	//	Next, fill the contents
	//
	RectClass fill_rect = rect;
	if( border_width > 0 )
	{
		fill_rect.Left += border_width + 1;
		fill_rect.Top += border_width + 1;
		fill_rect.Right	-= border_width - 1;
		fill_rect.Bottom	-= border_width - 1;
	}
	Add_Quad (fill_rect, fill_color);
	return ;
}

void	Render2DClass::Add_Outline( const RectClass & rect, float width, unsigned long color )
{
	// This prevents D3DTOP_MODULATE from sampling stale textures when drawing untextured outlines
	Add_Outline(rect, width, RectClass(0, 0, 1, 1), color);

}

void	Render2DClass::Add_Outline( const RectClass & rect, float width, const RectClass & uv, unsigned long color )
{
	//
	//	Pretty straight forward, simply add the four side of the rectangle as lines.
	//
	/** @todo colin, I had to tweak these to get precise line drawing, as we want
	the UV bias on, but it just isn't lining up */
	Add_Line (Vector2 (rect.Left + 1, rect.Bottom),	Vector2 (rect.Left + 1, rect.Top + 1),		width, color);
	Add_Line (Vector2 (rect.Left, rect.Top + 1),		Vector2 (rect.Right - 1, rect.Top + 1),			width, color);
	Add_Line (Vector2 (rect.Right, rect.Top),		Vector2 (rect.Right, rect.Bottom - 1),		width, color);
	Add_Line (Vector2 (rect.Right, rect.Bottom),	Vector2 (rect.Left + 1, rect.Bottom),	width, color);
}

void Render2DClass::Render(void)
{
	if (!Indices.Count() || IsHidden) {
		return;
	}

	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (!dev) {
		WWDEBUG_SAY(("Render2DClass::Render - Failed to get D3D device"));
		return;
	}


	// Ronin @bugfix 02/12/2025: RAII guard protects 2D rendering state
	// Automatically saves/restores IA state, transforms, viewport, and render states
	// Exception-safe: destructor runs even on early returns or crashes
	Scoped2DStateGuard stateGuard(dev, "Render2DClass::Render");

	// ========== SET UP 2D RENDERING ==========

	//
	// Configure the viewport for entire screen
	//
	int width, height, bits;
	bool windowed;
	WW3D::Get_Device_Resolution(width, height, bits, windowed);
	D3DVIEWPORT9 vp = { 0 };
	vp.X = 0;
	vp.Y = 0;
	vp.Width = width;
	vp.Height = height;
	vp.MinZ = 0.0f;
	vp.MaxZ = 1.0f;
	dev->SetViewport(&vp);

	//DX8Wrapper::Set_Texture(0, Texture);
	//VertexMaterialClass* vm = VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	//DX8Wrapper::Set_Material(vm);
	//REF_PTR_RELEASE(vm);

	Matrix4x4 identity(true);
	DX8Wrapper::Set_World_Identity();
	DX8Wrapper::Set_View_Identity();
	DX8Wrapper::Set_Transform(D3DTS_PROJECTION, identity);

	// Ronin @bugfix 14/11/2025 DX9: Disable depth testing/writing for 2D rendering
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_LIGHTING, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);

	// pick the correct FVF for 2D
	const DWORD render2d_fvf = DX8_FVF_XYZDUV1; // 0x142, XYZ + DIFFUSE + TEX1

	DynamicVBAccessClass vb(BUFFER_TYPE_DYNAMIC_DX8, render2d_fvf, Vertices.Count());
	{
/*#ifdef DEBUG

		WWDEBUG_SAY(("Render2D created VB: Requested FVF=0x%08X, Actual FVF=0x%08X, Stride=%d",
			render2d_fvf,
			vb.FVF_Info().Get_FVF(),
			vb.Get_Stride()));
#endif // DEBUG*/

		DynamicVBAccessClass::WriteLockClass Lock(&vb);
		const FVFInfoClass& fi = vb.FVF_Info();
		unsigned char* va = (unsigned char*)Lock.Get_Formatted_Vertex_Array();

		for (int i = 0; i < Vertices.Count(); i++)
		{
			Vector3 temp(Vertices[i].X, Vertices[i].Y, ZValue);
			*(Vector3*)(va + fi.Get_Location_Offset()) = temp;
			*(unsigned int*)(va + fi.Get_Diffuse_Offset()) = Colors[i];
			*(Vector2*)(va + fi.Get_Tex_Offset(0)) = UVCoordinates[i];
			va += fi.Get_FVF_Size();
		}
	}

	DynamicIBAccessClass ib(BUFFER_TYPE_DYNAMIC_DX8, Indices.Count());
	{
		DynamicIBAccessClass::WriteLockClass Lock(&ib);
		unsigned short* mem = Lock.Get_Index_Array();
		// Ronin @perf 18/12/2025: Use memcpy for bulk index copy (POD type)
		memcpy(mem, &Indices[0], Indices.Count() * sizeof(unsigned short));
	}



	// Ronin @bugfix 12/12/2025: Force fixed-function pipeline for 2D rendering
	// Declaration pollution from previous passes causes vertex layout misinterpretation
	// Use wrapper helpers to ensure clear of declarations and proper FVF binding

	// Step 1: Call fixed-function layout
	DX8Wrapper::BindLayoutFVF(render2d_fvf, "Render2D");

	// Step 2: Bind buffers (Set_Vertex_Buffer internally calls Apply)
	DX8Wrapper::Set_Vertex_Buffer(vb);
	DX8Wrapper::Set_Index_Buffer(ib, 0, "Render2DClass::Render");

	// Ronin @bugfix 15/01/2026: Keep wrapper texture cache coherent for 2D pass
	// Even if this path still uses direct dev->SetTexture for the actual draw (for now),
	// the wrapper must track what Render2D intends to use to prevent cross-pass pollution.
	DX8Wrapper::Set_Texture(0, Texture);


	// In Render2DClass::Render(), after Set_Index_Buffer:
	/*IDirect3DIndexBuffer9* activeIB = nullptr;
	DX8Wrapper::_Get_D3D_Device8()->GetIndices(&activeIB);
	WWDEBUG_SAY(("📊 Index Buffer after Set: %p (should be %p)", activeIB, ib));
	if (activeIB) activeIB->Release();*/

	// Step 3: Set 2D shader (marks SHADER_CHANGED flag)
	if (IsGrayScale)
	{
		// Special case added to draw grayscale non-alpha blended images
		DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueShader);

		if (DX8Wrapper::Get_Current_Caps()->Support_Dot3())
		{
			// Override W3D states with customizations for grayscale
			DX8Wrapper::Set_DX8_Render_State(D3DRS_TEXTUREFACTOR, 0x80A5CA8E);
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG0, D3DTA_TFACTOR | D3DTA_ALPHAREPLICATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG2, D3DTA_TFACTOR | D3DTA_ALPHAREPLICATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_MULTIPLYADD);

			DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
			DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
		}
		else
		{
			// Doesn't have DOT3 blend mode so fake it another way
			DX8Wrapper::Set_DX8_Render_State(D3DRS_TEXTUREFACTOR, 0x60606060);
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		}
	}
	else

	//DX8Wrapper::Set_Shader(Shader);

	// Step 4: Explicitly apply all pending changes
	DX8Wrapper::Apply_Render_State_Changes();

/*#ifdef _DEBUG
// Check again after Apply (reuse 'dev' - don't redeclare!)
UINT checkStride = 0;
UINT checkOffset = 0;
IDirect3DVertexBuffer9* checkVB = nullptr;
dev->GetStreamSource(0, &checkVB, &checkOffset, &checkStride);
WWDEBUG_SAY(("📊 After Apply_Render_State_Changes: VB=%p, Stride=%u (expected 24)", checkVB, checkStride));
if (checkVB) checkVB->Release();
#endif*/


	// Ronin @bugfix 14/12/2025: Direct draw after Apply_Render_State_Changes
	// We bypass the wrapper here for precise control, but MUST sync cache after

	{
		// Force fixed-function pipeline via wrapper (keeps wrapper/device coherent)
		//DX8Wrapper::BindLayoutFVF(render2d_fvf, "Render2DClass::Render(direct-DIP)");
		DX8Wrapper::Set_Vertex_Shader(0);
		DX8Wrapper::Set_Pixel_Shader(0);

		// Ronin @bugfix 18/12/2025: Check for valid D3D texture, not texture name
		// Font textures are dynamically created and have empty names but are valid
		bool hasTexture = (Texture != nullptr && Texture->Peek_D3D_Texture() != nullptr);

		// Ronin @bugfix 16/12/2025: BYPASS WRAPPER COMPLETELY for textures
		// We clear D3D stages directly and DON'T sync with wrapper until AFTER we're done.
		// This ensures our 2D texture actually gets bound regardless of wrapper's stale cache.
		//dev->SetTexture(0, nullptr);

		// Ronin @refactor 15/01/2026: Avoid zeroing texture stage 0 behind wrapper’s back.
		// Scoped2DStateGuard already clears all textures on entry, and we already set wrapper texture intent earlier.
		// Only clear stage 0 explicitly when we are doing untextured rendering.
		if (!hasTexture) {
			DX8Wrapper::Set_Texture(0, nullptr);
		}

		if (hasTexture) {
			// Textured rendering: bind texture and modulate with vertex color
			dev->SetTexture(0, Texture->Peek_D3D_Texture());
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

		}
		else {
			// Untextured rendering: use vertex color ONLY
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

		}

		// Ronin @bugfix 18/12/2025: MUST disable stage 1 to prevent stale multi-texture state
		dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

		// Ronin @refactor 15/01/2026: Sync wrapper texture intent with what we actually bound via dev calls
		DX8Wrapper::Set_Texture(0, hasTexture ? Texture : nullptr);

		// ========== CRITICAL: Set up 2D render states directly ==========
		// Alpha blending for 2D UI
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

		// Sampler state for texture filtering
		dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

		// Get D3D resources using public accessors
		IDirect3DVertexBuffer9* d3dVB = vb.Get_D3D_VB();
		IDirect3DIndexBuffer9* d3dIB = ib.Get_D3D_IB();

		if (d3dVB && d3dIB) {
			UINT stride = vb.Get_Stride();  // 24
			UINT vbOffsetBytes = vb.Get_VB_Offset() * stride;

			dev->SetStreamSource(0, d3dVB, vbOffsetBytes, stride);
			dev->SetIndices(d3dIB);

			// Direct draw - bypasses wrapper's Apply
			HRESULT hr = dev->DrawIndexedPrimitive(
				D3DPT_TRIANGLELIST,
				0,                          // BaseVertexIndex (indices are already offset)
				0,                          // MinVertexIndex  
				Vertices.Count(),           // NumVertices
				ib.Get_IB_Offset(),         // StartIndex
				Indices.Count() / 3         // PrimitiveCount
			);
/*#ifdef WWDEBUG
			{
				IDirect3DBaseTexture9* devT0 = nullptr;
				dev->GetTexture(0, &devT0); // AddRef'd
				IDirect3DBaseTexture9* expected = (Texture != nullptr) ? Texture->Peek_D3D_Texture() : nullptr;

				WWDEBUG_SAY((
					"🎨 [Render2D] after DIP: hasTexture=%d expectedD3D=%p devT0=%p match=%d",
					hasTexture ? 1 : 0,
					expected,
					devT0,
					(devT0 == expected) ? 1 : 0));

				if (devT0) devT0->Release();
			}
#endif*/
			//WWDEBUG_SAY(("🎨 DIRECT DRAW: HR=0x%08X, Polys=%d", hr, Indices.Count() / 3));
		}
		else {
			WWDEBUG_SAY(("❌ DIRECT DRAW FAILED: VB=%p, IB=%p", d3dVB, d3dIB));
		}
	}

	// ========== DRAW 2D GEOMETRY ==========
	// @Ronin @bugfix 14/12/2025: Direct draw done above - NO LONGER use wrapper draw call. FUCK THAT SHIT!
	// DX8Wrapper::Draw_Triangles(0, Indices.Count() / 3, 0, Vertices.Count());

// ========== AUTOMATIC STATE RESTORATION ==========
// Guard destructor automatically:
//   1. Clears wrapper's cached decl/FVF (forces next pass to rebind)
//   2. Restores saved VertexDeclaration (if was non-null on entry)
//   3. Restores saved Vertex buffer stream 0 (if was non-null on entry)
//   4. Restores saved Index buffer (if was non-null on entry)
//   5. Restores WORLD/VIEW/PROJECTION transforms
//   6. Restores Viewport
//   7. Restores render states (ZENABLE, ZWRITEENABLE, LIGHTING, CULLMODE)
//   8. Clears all texture stages 0-7 (prevents pollution to next 3D pass)
//   9. Invalidates wrapper's cached render states

	// Ronin @bugfix 18/12/2025: NOW sync wrapper cache to match reality(only stages we might have used)
	//DX8Wrapper::Set_Texture(0, nullptr);
	//DX8Wrapper::Set_Texture(1, nullptr);  // Just in case grayscale used it

	if (IsGrayScale)
		ShaderClass::Invalidate();	// Force both stages to be reset

	// No manual cleanup needed - guard handles everything!
}


/*
** Render2DTextClass
*/
Render2DTextClass::Render2DTextClass(Font3DInstanceClass *font) :
	Location(0.0f,0.0f),
	Cursor(0.0f,0.0f),
	Font(nullptr),
	WrapWidth(0),
	ClipRect(0, 0, 0, 0),
	IsClippedEnabled(false)
{
	Set_Coordinate_Range( RectClass( -320, -240, 320, 240 ) );
	Set_Font( font );

	Reset();
}

Render2DTextClass::~Render2DTextClass()
{
	REF_PTR_RELEASE(Font);
}

void	Render2DTextClass::Reset(void)
{
	Render2DClass::Reset();
	Cursor = Location;
	WrapWidth = 0;
	DrawExtents = RectClass( 0,0,0,0 );
	TotalExtents = RectClass( 0,0,0,0 );
	ClipRect.Set (0, 0, 0, 0);
	IsClippedEnabled = false;
}

void	Render2DTextClass::Set_Font( Font3DInstanceClass *font )
{
	REF_PTR_SET(Font,font);

	if ( Font != nullptr ) {
		Set_Texture( Font->Peek_Texture() );

	#define	BLOCK_CHAR	0
		BlockUV = Font->Char_UV( BLOCK_CHAR );
		// Inset it a bit to be sure we have no edge problems
		BlockUV.Inflate( Vector2(-BlockUV.Width()/4, -BlockUV.Height()/4) );
	}
}


/*
**
*/
void	Render2DTextClass::Draw_Char( WCHAR ch, unsigned long color )
{
	float char_spacing	= Font->Char_Spacing( ch );
	float char_height		= Font->Char_Height();

	//
	//	Check to see if this character is clipped
	//
	bool is_clipped = false;
	if (	IsClippedEnabled &&
			(Cursor.X < ClipRect.Left ||
			 Cursor.X + char_spacing > ClipRect.Right ||
			 Cursor.Y < ClipRect.Top ||
			 Cursor.Y + char_height > ClipRect.Bottom))
	{
		is_clipped = true;
	}

	if ( ch != (WCHAR)' ' && !is_clipped ) {
		RectClass screen( Cursor.X, Cursor.Y, Cursor.X + Font->Char_Width(ch), Cursor.Y + char_height );

		Internal_Add_Quad_Indicies( Vertices.Count() );
		Internal_Add_Quad_Vertices( screen );
		Internal_Add_Quad_UVs( Font->Char_UV( ch ) );
		Internal_Add_Quad_Colors( color );

		DrawExtents += screen;
		TotalExtents += screen;
	}
	Cursor.X += char_spacing;
}

void	Render2DTextClass::Draw_Text( const char * text, unsigned long color )
{
	WWMEMLOG(MEM_GEOMETRY);
	WideStringClass wide(0,true);
	wide.Convert_From( text );
	Draw_Text( wide, color );
}

void	Render2DTextClass::Draw_Text( const WCHAR * text, unsigned long color )
{
	WWMEMLOG(MEM_GEOMETRY);

	// Reset the Extents
	DrawExtents = RectClass( Location, Location );
	if ( TotalExtents.Width() == 0 ) {
		TotalExtents = RectClass( Location, Location );
	}

	while (*text) {
		WCHAR ch = *text++;

		//	Check to see if we need to move to a newline or not
		bool wrap = ( ch == (WCHAR)'\n' );

		// if the current char is a space, and the next word length puts us past our Width, wrap
		if ( ch == (WCHAR)' ' && WrapWidth > 0 ) {
			const WCHAR * word = text;
			float word_width = Font->Char_Spacing(ch);
			while ( (*word != (WCHAR)0) && (*word > (WCHAR)' ') ) {
				word_width += Font->Char_Spacing(*word++);
			}
			wrap = ( (Cursor.X + word_width) >= (Location.X + WrapWidth) );
		}

		if ( wrap ) {
			Cursor.Y += Font->Char_Height();
			Cursor.X = Location.X;
		} else {

			// Draw char at cursor, update cursor and extents
			Draw_Char( ch, color );
		}
	}
}

void	Render2DTextClass::Draw_Block( const RectClass & screen, unsigned long color )
{
	Internal_Add_Quad_Indicies( Vertices.Count() );
	Internal_Add_Quad_Vertices( screen );
	Internal_Add_Quad_UVs( BlockUV );
	Internal_Add_Quad_Colors( color );

	TotalExtents += screen;
}

Vector2	Render2DTextClass::Get_Text_Extents( const WCHAR * text )
{
	Vector2 extent (0, Font->Char_Height());

	if (text) {
		while (*text) {
			WCHAR ch = *text++;

			if ( ch != (WCHAR)'\n' ) {
				extent.X += Font->Char_Spacing( ch );
			}
		}
	}

	return extent;
}

