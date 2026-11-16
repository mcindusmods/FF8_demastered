#version 120

// DEMASTER (FIELD_PBR): the vertex shader DEMASTER draws the game's PBR field pages with. The game sends them already on its
// screen (TLVERTEX: x, y, the depth buffer value and 1 / depth), exactly as main.vert takes them.

uniform int demasterBlendMode; // the game's blend_mode of the draw

void main()
{
	vec4 pos = gl_Vertex;
	gl_FrontColor.rgba = gl_Color.bgra;
	pos.w = 1.0 / pos.w;
	pos.xyz *= pos.w;
	gl_Position = gl_ProjectionMatrix * pos;
	// BLEND_NONE
	if (demasterBlendMode == 4) gl_FrontColor.a = 1.0;
	// BLEND_25P
	else if (demasterBlendMode == 3) gl_FrontColor.a = 0.25;
	gl_TexCoord[0] = gl_MultiTexCoord0;
}
