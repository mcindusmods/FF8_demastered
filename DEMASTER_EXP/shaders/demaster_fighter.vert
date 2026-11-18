#version 120

// DEMASTER (SCENE_LIGHT): a battle fighter DEMASTER draws itself, with main.frag (DEMASTER_FIGHTER), the game's texture and
// state. The game's main.vert for its corners already on the screen (TLVERTEX), and the corners' smooth normals (view
// space, worked out by DEMASTER the way field characters get theirs) handed on to main.frag.

uniform int vertextype;
uniform int blend_mode;

varying vec3 demasterVertexNormal;

void main()
{
	vec4 pos = gl_Vertex;

	gl_FrontColor.rgba = gl_Color.bgra;

	// TLVERTEX (only those come here)
	pos.w = 1.0 / pos.w;
	pos.xyz *= pos.w;
	pos = gl_ProjectionMatrix * pos;

	// BLEND_NONE
	if (blend_mode == 4) gl_FrontColor.a = 1.0;
	// BLEND_25P
	else if (blend_mode == 3) gl_FrontColor.a = 0.25;

	gl_TexCoord[0] = gl_MultiTexCoord0;
	demasterVertexNormal = gl_Normal;

	gl_Position = pos;
}
