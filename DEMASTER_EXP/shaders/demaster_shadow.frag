#version 120

// DEMASTER (FIELD_PBR_SHADOWS): the characters as a light sees them, depth only (with demaster_chara.vert, mode 1).
// Cut-out parts (hair, fringes) cast shadows only where they are drawn.

uniform sampler2D tex;
uniform vec4 demasterCharMode;  // z: 1 = instanced (see demaster_chara.vert)
varying vec3 demasterTileClip;  // instanced: x, y, w in the copy's own clip space

void main()
{
	// instanced: outside this copy's tile is another copy's
	if (demasterCharMode.z > 0.5 && (abs(demasterTileClip.x) > demasterTileClip.z || abs(demasterTileClip.y) > demasterTileClip.z))
		discard;
	if (texture2D(tex, gl_TexCoord[0].st).a < 0.44)
		discard;
	gl_FragColor = vec4(1.0);
}
