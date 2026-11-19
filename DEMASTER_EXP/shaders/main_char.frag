#version 120

// DEMASTER: the game's main_char.frag (field characters and models drawn by the game) with the time of day's colour grade
// (LUT) at the end. Everything else is the game's own, unchanged.

uniform int vertextype;
uniform sampler2D tex;
uniform vec3 ambientColor;

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

void main()
{
	vec4 color = gl_Color;
	vec4 texture_color = texture2D(tex, gl_TexCoord[0].st);

	if(texture_color.a <= 0.0) discard;

	color.rgb *= texture_color.rgb;
	color.rgb += clamp(ambientColor, vec3(-1.0, -1.0, -1.0), vec3(1.0, 1.0, 1.0));
	color.rgb = clamp(color.rgb, vec3(0.0, 0.0, 0.0), vec3(1.0, 1.0, 1.0));
	color.a = texture_color.a;
	color.rgb = demasterGrade(color.rgb);

	gl_FragColor = color;
}
