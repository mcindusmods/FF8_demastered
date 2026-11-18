#version 120

uniform int vertextype;
uniform sampler2D tex;
uniform int effecttype;
uniform int effectoption;

// DEMASTER (FIELD_LAYERS + FIELD_SMOOTH): set only while a field page DEMASTER put together from layer images is drawn,
// zero otherwise. x, y: one tile (16 texels) in texture coordinates; z, w: one texel of the page.
// Such a page is filtered smoothly (bilinear, weighted by alpha so transparent texels do not darken edges), but every
// lookup stays inside the tile's own 16x16 cell: the cells around it on the page belong to other tiles, and sampling
// them is what made seams. The texture itself stays point-filtered; the four lookups land on texel centres.
uniform vec4 demasterTileClamp;

// DEMASTER (FIELD_PBR) draws its PBR pages with its own program (demaster_field.vert/.frag), not with this one.

// DEMASTER (SCENE_LIGHT): battles and the world map. All zero unless DEMASTER lights this draw (a battle stage, the
// fighters, the world map's ground): then the picture is multiplied by a colour in linear light (the time of day and the
// place's rules in DEMASTER_EXP\scene_light.txt), and a battle stage page can bring a picture of its glows and an ORM map.
uniform vec4 demasterScene;        // rgb: what the picture is multiplied by; a: 1 = on
uniform vec4 demasterSceneMaps;    // x: 1 = a picture of the glows is on demasterSceneEC; y: how much of it shows (how
                                   // night it is) times its brightness in z; w: how much of the AO in demasterSceneORM is
                                   // used (0: no ORM map)
uniform sampler2D demasterSceneEC; // the glows: colour (sRGB, times its coverage) and coverage (alpha), as in fields
uniform sampler2D demasterSceneORM;

vec3 demasterSceneToLinear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 demasterSceneToSrgb(vec3 c)
{
	c = clamp(c, 0.0, 1.0);
	return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec4 demasterTileTexture(vec2 uv)
{
	vec2 texel = demasterTileClamp.zw;
	vec2 cell = floor(uv / demasterTileClamp.xy);
	vec2 lo = cell * demasterTileClamp.xy + 0.5 * texel;
	vec2 hi = (cell + 1.0) * demasterTileClamp.xy - 0.5 * texel;
	vec2 p = uv / texel - 0.5;
	vec2 f = fract(p);
	vec2 c0 = clamp((floor(p) + 0.5) * texel, lo, hi);
	vec2 c1 = clamp((floor(p) + 1.5) * texel, lo, hi);
	vec4 t00 = texture2D(tex, c0);
	vec4 t10 = texture2D(tex, vec2(c1.x, c0.y));
	vec4 t01 = texture2D(tex, vec2(c0.x, c1.y));
	vec4 t11 = texture2D(tex, c1);
	float w00 = (1.0 - f.x) * (1.0 - f.y) * t00.a;
	float w10 = f.x * (1.0 - f.y) * t10.a;
	float w01 = (1.0 - f.x) * f.y * t01.a;
	float w11 = f.x * f.y * t11.a;
	float a = w00 + w10 + w01 + w11;
	vec3 rgb = (t00.rgb * w00 + t10.rgb * w10 + t01.rgb * w01 + t11.rgb * w11) / max(a, 0.00001);
	return vec4(rgb, a);
}

void main()
{
	vec4 color = gl_Color;
	vec4 texture_color = demasterTileClamp.x > 0.0 ? demasterTileTexture(gl_TexCoord[0].st) : texture2D(tex, gl_TexCoord[0].st);

	if(texture_color.a <= 0.0) discard;

	if (effecttype != 0)
	{
		// weapon
		if (effectoption == 1 && gl_TexCoord[0].s <= 0.5 && gl_TexCoord[0].t >= 0.875)
		{
			color.rgb *= texture_color.rgb;
		}
		// petrify
		else if (effecttype == 1)
		{
			float gray = texture_color.r * 0.3 + texture_color.g * 0.59 + texture_color.b * 0.11;
			color.rgb *= vec3(gray, gray, gray);
		}
		// zombie
		else if (effecttype == 2)
		{
			color.rgb *= vec3(texture_color.r * 0.1, texture_color.g * 0.4, texture_color.b * 0.1);
		}
	}
	else
	{
		color.rgb *= texture_color.rgb;
	}
	color.a = texture_color.a;

	if (demasterScene.a > 0.0)
	{
		vec3 lit = demasterSceneToLinear(clamp(color.rgb, 0.0, 1.0));
		if (demasterSceneMaps.w > 0.0)
			lit *= mix(1.0, texture2D(demasterSceneORM, gl_TexCoord[0].st).r, demasterSceneMaps.w);
		lit *= demasterScene.rgb;
		if (demasterSceneMaps.x > 0.5)
		{
			// a lit window (coverage 1) takes the darkened picture's place, a halo (coverage 0) only adds its light
			vec4 glows = texture2D(demasterSceneEC, gl_TexCoord[0].st);
			float showing = clamp(demasterSceneMaps.y, 0.0, 1.0);
			lit = lit * (1.0 - glows.a * showing) + demasterSceneToLinear(glows.rgb) * (showing * demasterSceneMaps.z);
		}
		color.rgb = demasterSceneToSrgb(lit);
	}

	gl_FragColor = color;
}
