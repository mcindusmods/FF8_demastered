#version 120

// DEMASTER (FIELD_PBR_CHARACTERS): the field's characters and 3D models lit by the room. See demaster_chara.vert.
// Without the room's light (FIELD_PBR_CHARACTERS=0, or a field without its light grid) this draws exactly what the game's
// main_char.frag draws.
//
// The model's own maps, in their own atlases laid out like the game's texture:
//   demasterModelN     normal (x along the image's right, y up the image, z out of it; alpha kept for later)
//   demasterModelORME  AO, roughness, metallic, emissive (the albedo glows where it is white)
// The normal map is bent onto each triangle the way the texture lies on it there: the directions of the texture's x and y
// across the surface come from how the texture coordinates change between neighbouring pixels (a cotangent frame), so
// mirrored and rotated parts of the texture need nothing of their own.

uniform sampler2D tex;
uniform sampler2D demasterModelN;
uniform sampler2D demasterModelORME;
uniform sampler3D demasterProbes;
uniform sampler2D demasterRoom;

uniform vec4 demasterModel;      // x: normal map; y: ORME; z: +1 if the image's up runs along +v, -1 along -v; w: lit by the room
uniform vec4 demasterGame;       // rgb: the game's ambientColor for this draw; w: debug view (FIELD_PBR_VIEW)
uniform vec4 demasterShade;      // rgb: added after the lighting (the game's darkening, or what changed of it); w: 1 = light grid
uniform vec4 demasterProbeBox;   // canvas width, height; the light grid's nearest and farthest slice
uniform vec4 demasterProbeInfo;  // x: slices; y: brightness (exposure); z: flicker change of the nearest flickering light (unused here)
uniform vec4 demasterSpace;      // zoom, canvas x and y of the camera axis, depth model (0 linear, 1 inverse)
uniform vec4 demasterDepth;      // a, b, nearest distance
uniform vec4 demasterRoomSize;   // canvas width, height; z: 1 = the room's depth is there
uniform vec4 demasterFeatures;   // w: room shadows for added lights
uniform int demasterLightCount;
uniform vec4 demasterLightPos[8];
uniform vec4 demasterLightColor[8];
uniform vec4 demasterLightFlicker[8];   // x: painted light's change now; y: owns the steady mask; z: shadow strength; w: 1 = a lamp on at night
uniform vec4 demasterNight;      // DAY_NIGHT: x how night it is (0 day, 1 night); y the light left away from the night's lamps; z how much the lamps keep
uniform vec4 demasterNightTint;  // rgb: what's left times this
uniform vec4 demasterCharMode;   // x: 0 the game's screen, 1 a picture; z: 1 = instanced (see demaster_chara.vert)
uniform vec4 demasterQuality;    // x: wall-check steps for added lights (0: none) (FIELD_PBR_QUALITY)

varying vec3 demasterView;
varying vec3 demasterNormal;
varying vec4 demasterLook;       // who looks: xyz a position (w 1) or the direction it looks along (w 0), view space
varying float demasterMirror;    // 1: the picture is mirrored (seen from behind)
varying vec3 demasterTileClip;   // instanced: x, y, w in the copy's own clip space

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

const float PI = 3.14159265;

vec3 demasterToLinear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 demasterToSrgb(vec3 c)
{
	c = clamp(c, 0.0, 1.0);
	return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

float demasterDistance(float value)
{
	float d = demasterDepth.x + demasterDepth.y * value;
	float z = demasterSpace.w > 0.5 ? 1.0 / max(d, 0.000001) : d;
	return max(z, demasterDepth.z);
}

vec2 demasterCanvasOf(vec3 p)
{
	return demasterSpace.yz + p.xy * demasterSpace.x / max(p.z, 0.000001);
}

float demasterRoomClear(vec3 from, vec3 to)
{
	if (demasterRoomSize.z < 0.5 || demasterQuality.x < 0.5)
		return 1.0;
	float steps = floor(demasterQuality.x + 0.5);
	for (int i = 1; i <= 16; i++)
	{
		if (float(i) > steps)
			break;
		vec3 s = mix(from, to, float(i) / (steps + 1.0));
		if (s.z <= demasterDepth.z)
			continue;
		vec2 uv = demasterCanvasOf(s) / demasterRoomSize.xy;
		if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
			continue;
		float room = demasterDistance(texture2D(demasterRoom, uv).a);
		if (s.z > room * 1.02 && s.z < room * 1.35)
			return 0.0;
	}
	return 1.0;
}

// DAY_NIGHT: how much of the room's light is left here (as demaster_field.frag dims the painting): away from the lamps on
// at night [night] level x tint, within a lamp's range [night] lamps of it
vec3 demasterNightLeft(vec3 position)
{
	if (demasterNight.x <= 0.0)
		return vec3(1.0);
	float keep = 0.0;
	for (int i = 0; i < 8; i++)
	{
		if (i >= demasterLightCount)
			break;
		if (demasterLightFlicker[i].w <= 0.0)
			continue;
		vec3 d = demasterLightPos[i].xyz - position;
		float x2 = dot(d, d) / (demasterLightPos[i].w * demasterLightPos[i].w);
		float window = clamp(1.0 - x2, 0.0, 1.0);
		keep = max(keep, window * window);
	}
	vec3 dark = demasterNight.y * demasterNightTint.rgb;
	return mix(vec3(1.0), mix(dark, vec3(1.0), keep * demasterNight.z), demasterNight.x);
}

// the room's light arriving at p on a surface facing n (irradiance); share: how much of it flickers
vec3 demasterProbeLight(vec3 p, vec3 n, out float share)
{
	share = 0.0;
	if (demasterShade.w < 0.5)
		return vec3(PI); // no light grid: as bright as the game draws it
	vec2 canvas = demasterCanvasOf(p) / demasterProbeBox.xy;
	float slices = demasterProbeInfo.x;
	float f = clamp((p.z - demasterProbeBox.z) / max(demasterProbeBox.w - demasterProbeBox.z, 0.000001), 0.0, 1.0);
	float slice = clamp(f * slices, 0.5, slices - 0.5);
	vec2 st = clamp(canvas, 0.0, 1.0);
	float depth = 4.0 * slices;
	vec4 b0 = texture3D(demasterProbes, vec3(st, slice / depth));
	vec4 bx = texture3D(demasterProbes, vec3(st, (slices + slice) / depth));
	vec4 by = texture3D(demasterProbes, vec3(st, (2.0 * slices + slice) / depth));
	vec4 bz = texture3D(demasterProbes, vec3(st, (3.0 * slices + slice) / depth));
	share = b0.a;
	return max(b0.rgb + bx.rgb * n.x + by.rgb * n.y + bz.rgb * n.z, vec3(0.0)) * demasterProbeInfo.y;
}

// Light added on top of the room's (the lights, the model's own glow), rolled off towards white instead of cut off at it,
// the same way as the backgrounds' (demaster_field.frag). With nothing added the room's light comes back exactly.
vec3 demasterRollOff(vec3 lit, vec3 extra)
{
	vec3 base = max(lit + min(extra, 0.0), 0.0);
	vec3 room = max(1.0 - base, 0.0001);
	return base + room * (1.0 - exp(-max(extra, 0.0) / room));
}

void main()
{
	// instanced: outside this copy's tile is another copy's
	if (demasterCharMode.z > 0.5 && (abs(demasterTileClip.x) > demasterTileClip.z || abs(demasterTileClip.y) > demasterTileClip.z))
		discard;
	vec4 color = gl_Color;
	vec4 texel = texture2D(tex, gl_TexCoord[0].st);
	if (texel.a <= 0.0)
		discard;
	int viewMode = int(demasterGame.w + 0.5);
	if (demasterModel.w < 0.5 && viewMode != 15)
	{
		// the game's own look (main_char.frag)
		color.rgb *= texel.rgb;
		color.rgb += clamp(demasterGame.rgb, vec3(-1.0), vec3(1.0));
		color.rgb = clamp(color.rgb, vec3(0.0), vec3(1.0));
		color.a = texel.a;
		color.rgb = demasterGrade(color.rgb);
		gl_FragColor = color;
		return;
	}

	vec3 p = demasterView;
	vec3 n = normalize(demasterNormal);
	vec3 view = demasterLook.w > 0.5 ? normalize(demasterLook.xyz - p) : -normalize(demasterLook.xyz);
	if (gl_FrontFacing == (demasterMirror > 0.5))
		n = -n; // the inside of an open part (a cape, a sleeve) seen in a picture of the character
	// the model's normal map, bent onto this triangle
	if (demasterModel.x > 0.5)
	{
		vec3 mapped = texture2D(demasterModelN, gl_TexCoord[0].st).rgb * 2.0 - 1.0;
		vec3 dp1 = dFdx(p), dp2 = dFdy(p);
		vec2 duv1 = dFdx(gl_TexCoord[0].st), duv2 = dFdy(gl_TexCoord[0].st);
		vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
		vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
		vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
		float size = max(dot(t, t), dot(b, b));
		if (size > 1e-24)
		{
			float invmax = inversesqrt(size);
			// b runs along +v; the image's up runs along demasterModel.z * v
			vec3 bent = t * invmax * mapped.x + b * invmax * (mapped.y * demasterModel.z) + n * mapped.z;
			n = normalize(bent);
		}
	}
	vec4 orme = demasterModel.y > 0.5 ? texture2D(demasterModelORME, gl_TexCoord[0].st) : vec4(1.0, 0.7, 0.0, 0.0);
	float ao = orme.r;
	float roughness = clamp(orme.g, 0.045, 1.0);
	float metallic = clamp(orme.b, 0.0, 1.0);
	float emissive = orme.a;

	vec3 base = demasterToLinear(texel.rgb) * demasterToLinear(color.rgb);
	if (viewMode == 11)
		base = vec3(0.5);
	vec3 diffuseColour = base * (1.0 - metallic);
	vec3 f0 = mix(vec3(0.04), base, metallic);
	float nv = max(dot(n, view), 0.0001);

	// the room's light around it, flickering with the nearest flickering light
	float share;
	vec3 room = demasterProbeLight(p, n, share);
	float nearest = 1e30;
	float change = 0.0;
	for (int i = 0; i < 8; i++)
	{
		if (i >= demasterLightCount)
			break;
		vec3 d = demasterLightPos[i].xyz - p;
		float dd = dot(d, d);
		if (demasterLightFlicker[i].y > 0.5 && dd < nearest)
		{
			nearest = dd;
			change = demasterLightFlicker[i].x;
		}
	}
	vec3 nightLeft = demasterNightLeft(p);
	room *= max(1.0 + share * change, 0.0) * nightLeft;
	vec3 lit = diffuseColour * room / PI * ao;
	// a little of the room in shiny parts
	vec3 r = reflect(-view, n);
	float rshare;
	vec3 env = demasterProbeLight(p, r, rshare) / PI * nightLeft;
	vec3 fresnelV = f0 + (1.0 - f0) * pow(1.0 - nv, 5.0);
	lit += env * fresnelV * (1.0 - roughness) * (1.0 - roughness) * ao;

	// the added lights, kept out by the room (walk behind a pillar: into its shadow)
	float alpha2 = roughness * roughness * roughness * roughness;
	float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
	vec3 added = vec3(0.0);
	for (int i = 0; i < 8; i++)
	{
		if (i >= demasterLightCount)
			break;
		vec3 colour = demasterLightColor[i].rgb;
		if (colour.r + colour.g + colour.b <= 0.0)
			continue;
		vec3 toLight = demasterLightPos[i].xyz - p;
		float dist = length(toLight);
		float range = demasterLightPos[i].w;
		if (dist >= range)
			continue;
		vec3 l = toLight / max(dist, 0.000001);
		float nl = max(dot(n, l), 0.0);
		if (nl <= 0.0)
			continue;
		float x = dist / range;
		float window = clamp(1.0 - x * x * x * x, 0.0, 1.0);
		float falloff = window * window / (1.0 + 8.0 * x * x);
		float clear = demasterFeatures.w > 0.5 ? demasterRoomClear(p, demasterLightPos[i].xyz) : 1.0;
		if (clear <= 0.0)
			continue;
		vec3 h = normalize(l + view);
		float nh = max(dot(n, h), 0.0);
		float vh = max(dot(view, h), 0.0);
		float dd = nh * nh * (alpha2 - 1.0) + 1.0;
		float distribution = alpha2 / (PI * dd * dd);
		float geometry = (nv / (nv * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k));
		vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
		vec3 specular = distribution * geometry * fresnel / max(4.0 * nv * nl, 0.0001);
		added += colour * falloff * nl * clear * (diffuseColour + specular * PI * demasterLightColor[i].a);
	}
	added += base * emissive;

	vec3 rgb = demasterToSrgb(demasterRollOff(lit, added)) + demasterShade.rgb;
	if (viewMode == 15)
		rgb = vec3(n.x, -n.y, -n.z) * 0.5 + 0.5;
	color.rgb = clamp(rgb, vec3(0.0), vec3(1.0));
	color.a = texel.a;
	if (viewMode != 15)
		color.rgb = demasterGrade(color.rgb);
	gl_FragColor = color;
}
