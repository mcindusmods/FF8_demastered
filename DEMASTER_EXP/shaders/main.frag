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
// Battles, lit from the stage's own space. The game hands OpenGL corners that are already on the screen; with the numbers
// its projection used (DEMASTER reads them from the game each frame) every pixel's place in front of the camera comes
// back: z = 1 / rhw, and x, y from the screen. The surface's direction is what that place does between neighbouring
// pixels; a normal map is bent onto it the way the texture lies there (a cotangent frame, as on field models).
uniform vec4 demasterSceneProj;    // x, y: PlayStation pixels per framebuffer pixel; z: the viewport's height in pixels;
                                   // w: the projection's distance (0: no camera, nothing below is used)
uniform vec4 demasterSceneCentre;  // xy: the projection's screen centre (PlayStation pixels); z: specular; w: how far from
                                   // the stage's middle its sky stands (from there on nothing is lit)
uniform mat3 demasterSceneRot;     // into the stage's space: stage = demasterSceneRot * (view - demasterSceneTrans)
uniform vec3 demasterSceneTrans;
uniform vec4 demasterSceneSun;     // xyz: towards the sun or moon, view space; w: how much turning away from it darkens
uniform vec4 demasterSceneSunLight; // rgb: its light (colour x strength); w: relief (how strongly a normal map shows)
uniform vec4 demasterSceneDraw;    // this draw: x 1 = the stage (the sun's part; fighters only get the lights), y 1 = a
                                   // normal map on demasterSceneN, z 1 = an ORM map on demasterSceneORM; w: a fighter
                                   // DEMASTER draws (DEMASTER_FIGHTER): its share of the sun's darkening
uniform vec4 demasterSceneLightAt[4];     // the stage's own lights: xyz view space, w range (0: none)
uniform vec4 demasterSceneLightColour[4]; // rgb, linear
uniform sampler2D demasterSceneN;
// The fighters' shadows on the stage: last frame's fighters as the sun sees them (a depth map)
uniform sampler2D demasterSceneShadowMap;
uniform mat4 demasterSceneShadowMatrix; // the stage's space to the map: x, y 0-1 across it, z its depth (0 nearest the sun)
uniform vec4 demasterSceneShadowInfo;   // x: how much of the sun a shadow takes; y: the depth's bias; z: one texel; w: 1 = on
#ifdef DEMASTER_FIGHTER
// DEMASTER's fighter program (demaster_fighter.vert): the corners' smooth normals, view space
varying vec3 demasterVertexNormal;
#endif

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
		vec3 base = lit; // the picture's own colour: what the stage's lights fall on
		vec3 added = vec3(0.0);
		if (demasterSceneProj.w > 0.0)
		{
			float z = 1.0 / gl_FragCoord.w;
			vec2 screen = vec2(gl_FragCoord.x * demasterSceneProj.x, (demasterSceneProj.z - gl_FragCoord.y) * demasterSceneProj.y);
			vec3 p = vec3((screen - demasterSceneCentre.xy) * (z / demasterSceneProj.w), z);
			vec3 stage = demasterSceneRot * (p - demasterSceneTrans);
			if (dot(stage.xz, stage.xz) < demasterSceneCentre.w * demasterSceneCentre.w)
			{
				vec3 dp1 = dFdx(p), dp2 = dFdy(p);
				vec3 face = cross(dp1, dp2);
				float area = dot(face, face);
				if (area > 0.0)
				{
					face *= inversesqrt(area);
					if (dot(face, p) > 0.0)
						face = -face; // towards the camera
					vec3 n = face;
#ifdef DEMASTER_FIGHTER
					// a fighter: its smooth normal (on the side the camera sees) for the lights, and its share of the sun's darkening
					vec3 smoothNormal = normalize(demasterVertexNormal);
					n = dot(smoothNormal, face) < 0.0 ? -smoothNormal : smoothNormal;
					lit *= 1.0 - demasterSceneDraw.w * demasterSceneSun.w * (0.5 - 0.5 * dot(n, demasterSceneSun.xyz));
#endif
					if (demasterSceneDraw.y > 0.5)
					{
						vec3 mapped = texture2D(demasterSceneN, gl_TexCoord[0].st).rgb * 2.0 - 1.0;
						vec2 duv1 = dFdx(gl_TexCoord[0].st), duv2 = dFdy(gl_TexCoord[0].st);
						vec3 dp2perp = cross(dp2, face), dp1perp = cross(face, dp1);
						vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
						vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
						float size = max(dot(t, t), dot(b, b));
						if (size > 1e-24)
						{
							float invmax = inversesqrt(size);
							// (the image's up runs along -v, as on field models)
							n = normalize(t * invmax * mapped.x - b * invmax * mapped.y + face * mapped.z);
						}
					}
					if (demasterSceneDraw.x > 0.5)
					{
						// the sun: a surface turned away from it darkens (half of it at a right angle), and a normal map's
						// slopes brighten and darken around that
						float towards = dot(face, demasterSceneSun.xyz), bent = dot(n, demasterSceneSun.xyz);
						lit *= 1.0 - demasterSceneSun.w * (0.5 - 0.5 * towards);
						// the fighters' shadows: where the map has something nearer the sun than this place (four taps)
						if (demasterSceneShadowInfo.w > 0.5 && towards > 0.0)
						{
							vec4 s = demasterSceneShadowMatrix * vec4(stage, 1.0);
							if (s.x > 0.0 && s.x < 1.0 && s.y > 0.0 && s.y < 1.0 && s.z < 1.0)
							{
								float texel = demasterSceneShadowInfo.z, depth = s.z - demasterSceneShadowInfo.y;
								float dark = step(texture2D(demasterSceneShadowMap, s.xy + vec2(-0.5, -0.5) * texel).r, depth)
									+ step(texture2D(demasterSceneShadowMap, s.xy + vec2(0.5, -0.5) * texel).r, depth)
									+ step(texture2D(demasterSceneShadowMap, s.xy + vec2(-0.5, 0.5) * texel).r, depth)
									+ step(texture2D(demasterSceneShadowMap, s.xy + vec2(0.5, 0.5) * texel).r, depth);
								lit *= 1.0 - demasterSceneShadowInfo.x * 0.25 * dark * min(towards * 4.0, 1.0);
							}
						}
						lit *= clamp(1.0 + demasterSceneSunLight.w * (bent - towards), 0.2, 2.5);
						if (demasterSceneDraw.z > 0.5 && demasterSceneCentre.z > 0.0 && bent > 0.0)
						{
							vec3 orm = texture2D(demasterSceneORM, gl_TexCoord[0].st).rgb;
							float roughness = clamp(orm.g, 0.08, 1.0);
							float alpha2 = roughness * roughness * roughness * roughness;
							vec3 halfway = normalize(demasterSceneSun.xyz - normalize(p));
							float nh = max(dot(n, halfway), 0.0);
							float d = nh * nh * (alpha2 - 1.0) + 1.0;
							added += demasterSceneSunLight.rgb * mix(vec3(0.04), base, orm.b)
								* (alpha2 / (3.14159265 * d * d) * 0.25 * bent * demasterSceneCentre.z);
						}
					}
					for (int i = 0; i < 4; i++)
					{
						if (demasterSceneLightAt[i].w <= 0.0)
							continue;
						vec3 to = demasterSceneLightAt[i].xyz - p;
						float away = length(to);
						float reach = clamp(1.0 - away / demasterSceneLightAt[i].w, 0.0, 1.0);
						added += base * demasterSceneLightColour[i].rgb
							* (reach * reach * (0.25 + 0.75 * max(dot(n, to / max(away, 1.0)), 0.0)));
					}
				}
			}
		}
		lit = lit * demasterScene.rgb + added;
		if (demasterSceneMaps.x > 0.5)
		{
			// a lit window (coverage 1) takes the darkened picture's place, a halo (coverage 0) only adds its light
			vec4 glows = texture2D(demasterSceneEC, gl_TexCoord[0].st);
			float showing = clamp(demasterSceneMaps.y, 0.0, 1.0);
			lit = lit * (1.0 - glows.a * showing) + demasterSceneToLinear(glows.rgb) * (showing * demasterSceneMaps.z);
		}
		color.rgb = demasterSceneToSrgb(lit);
	}

	color.rgb = demasterGrade(color.rgb);
	gl_FragColor = color;
}
