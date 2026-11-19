#version 120

// DEMASTER (main_notex.frag): the game's main_notex.frag (plain colour, no texture: the world map's sky and haze, the
// round shadows under field characters) with two of DEMASTER's additions, both zero unless DEMASTER says the draw is the
// world map's scene: SCENE_LIGHT's night (the colour times demasterScene, in linear light) and the colour grade (LUT).
// Otherwise exactly the game's.

uniform int vertextype;
uniform sampler2D tex;

uniform vec4 demasterScene; // rgb: the colour is multiplied by this, in linear light; a: 1 = on

// DEMASTER (LUT): the time of day's colour grade (DEMASTER_EXP\lut\, see lut_grade.h). All zero unless this draw is part
// of the scene and a grade is on: then the finished colour goes through two LUTs (colour look-up tables), mixed.
uniform vec4 demasterLut;       // x: how much of the second LUT (0-1); y: how much of the grade (0: none); z: points along each colour
uniform sampler2D demasterLutA; // the LUTs as strips: n squares of n x n side by side, red across a square, green down it,
uniform sampler2D demasterLutB; // blue's steps from square to square

vec3 demasterLutRead(sampler2D lut, vec3 c)
{
	float n = demasterLut.z;
	float b = c.b * (n - 1.0);
	float slice = min(floor(b), n - 1.0);
	float next = min(slice + 1.0, n - 1.0);
	vec2 inSquare = c.rg * (n - 1.0) + 0.5;
	vec3 low = texture2D(lut, vec2((slice * n + inSquare.x) / (n * n), inSquare.y / n)).rgb;
	vec3 high = texture2D(lut, vec2((next * n + inSquare.x) / (n * n), inSquare.y / n)).rgb;
	return mix(low, high, b - slice);
}

vec3 demasterGrade(vec3 c)
{
	if (demasterLut.y <= 0.0)
		return c;
	c = clamp(c, 0.0, 1.0);
	vec3 graded = demasterLutRead(demasterLutA, c);
	if (demasterLut.x > 0.0)
		graded = mix(graded, demasterLutRead(demasterLutB, c), demasterLut.x);
	return mix(c, graded, demasterLut.y);
}

vec3 demasterSceneToLinear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 demasterSceneToSrgb(vec3 c)
{
	c = clamp(c, 0.0, 1.0);
	return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main()
{
	vec4 color = gl_Color;
	if (demasterScene.a > 0.0)
		color.rgb = demasterSceneToSrgb(demasterSceneToLinear(clamp(color.rgb, 0.0, 1.0)) * demasterScene.rgb);
	color.rgb = demasterGrade(color.rgb);
	gl_FragColor = color;
}
