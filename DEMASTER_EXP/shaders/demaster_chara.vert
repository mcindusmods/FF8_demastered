#version 120

// DEMASTER (FIELD_PBR_CHARACTERS, _SHADOWS, _REFLECTIONS, FIELD_AVATARS): the field's characters and 3D models, drawn by
// DEMASTER from the triangles the game sent (the whole model: back faces too).
//   gl_Vertex           the game's vertex on its screen (TLVERTEX: x, y, depth buffer value, 1 / depth)
//   gl_Color            the game's vertex colour (blue, green, red, alpha, as main.vert takes it)
//   gl_MultiTexCoord0   the game's texture coordinates
//   gl_MultiTexCoord1   where the vertex is in view space (x right, y down, z away from the camera; see field_pbr.h)
//   gl_Normal           its smooth normal, view space
// demasterCharMode.x: 0 = on the game's screen, as the game would have drawn it; 1 = through demasterViewMatrix (the
// pictures reflections use, the avatars, the lights' shadow maps).
// Instancing (DEMASTER puts DEMASTER_INSTANCED and GL_ARB_draw_instanced above when the driver has it) with
// demasterCharMode.z = 1: one draw makes several pictures or shadow maps of the model. Copy i goes through
// demasterInstanceMatrix[i] into its own tile of the target (demasterInstanceTile[i]); the fragment shaders throw away what
// falls outside that tile (the part a viewport of the tile's size would have cut off).

uniform vec4 demasterCharMode;     // x: mode; y: 1 = the picture is mirrored (seen from behind); z: 1 = instanced
uniform mat4 demasterScreenMatrix; // the game's projection
uniform mat4 demasterViewMatrix;   // view space to clip space (mode 1)
uniform int demasterBlendMode;     // the game's blend_mode of the draw
uniform vec4 demasterViewer;       // who looks: xyz a position (w 1) or the direction it looks along (w 0), view space
#ifdef DEMASTER_INSTANCED
uniform mat4 demasterInstanceMatrix[8]; // view space to the copy's own clip space
uniform vec4 demasterInstanceTile[8];   // the copy's tile in the target's clip space: xy its size (0-1), zw its centre
uniform vec4 demasterInstanceLook[8];   // pictures: xyz the direction the copy's camera looks along; w 1 = mirrored
#endif

varying vec3 demasterView;
varying vec3 demasterNormal;
varying vec4 demasterLook;     // who looks (demasterViewer, or the copy's camera)
varying float demasterMirror;  // 1: the picture is mirrored
varying vec3 demasterTileClip; // x, y, w in the copy's own clip space (outside -w..w: another copy's tile)

void main()
{
	gl_FrontColor.rgba = gl_Color.bgra;
	demasterLook = demasterViewer;
	demasterMirror = demasterCharMode.y;
	if (demasterCharMode.x < 0.5)
	{
		vec4 pos = gl_Vertex;
		pos.w = 1.0 / pos.w;
		pos.xyz *= pos.w;
		gl_Position = demasterScreenMatrix * pos;
		demasterTileClip = gl_Position.xyw;
	}
	else
	{
		vec4 clip = demasterViewMatrix * vec4(gl_MultiTexCoord1.xyz, 1.0);
		vec4 tile = vec4(1.0, 1.0, 0.0, 0.0);
#ifdef DEMASTER_INSTANCED
		if (demasterCharMode.z > 0.5)
		{
			int i = gl_InstanceIDARB;
			clip = demasterInstanceMatrix[i] * vec4(gl_MultiTexCoord1.xyz, 1.0);
			tile = demasterInstanceTile[i];
			demasterLook = vec4(demasterInstanceLook[i].xyz, 0.0);
			demasterMirror = demasterInstanceLook[i].w;
		}
#endif
		demasterTileClip = clip.xyw;
		gl_Position = vec4(clip.xy * tile.xy + tile.zw * clip.w, clip.zw);
	}
	// BLEND_NONE
	if (demasterBlendMode == 4) gl_FrontColor.a = 1.0;
	// BLEND_25P
	else if (demasterBlendMode == 3) gl_FrontColor.a = 0.25;
	gl_TexCoord[0] = gl_MultiTexCoord0;
	demasterView = gl_MultiTexCoord1.xyz;
	demasterNormal = gl_Normal;
}
