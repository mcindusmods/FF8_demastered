#version 120

// DEMASTER (FIELD_PBR): the fragment shader DEMASTER draws the game's PBR field pages with (instead of main.frag, only for
// those draws). Cells of the page that are not PBR are drawn as main.frag draws them.
//
// tex holds the layers' albedo (sRGB, coverage in alpha); the other maps have the same cells:
//   demasterLF     lightmap (sRGB curve, times the cell's scale) and steady mask (alpha: 1 steady, lower = flickers)
//   demasterND     normal (x right, y up, z towards the camera) and depth map value (alpha)
//   demasterORME   AO, roughness, metallic, emissive (an older _ORMH has the height there: not used)
//   demasterEC     the glows as a picture: colour (sRGB, times its coverage) and coverage (alpha: how much of the painting
//                  the glow takes the place of). A cell with it doesn't read the ORM map's alpha
//   demasterCells  per 16x16 cell, two texels: (canvas x, y of its tile, flags 1 PBR +2 normal +4 ORM map +8 it is an
//                  _ORME +16 it has a picture of its glows, lightmap scale)
//                  and, cellsY rows further down, (the tile's .map Z, layer id, the view distance of the nearest thing the
//                  tile shows: a tenth of its pixels are nearer (0: none), 0)
// Lighting happens in view space: x right, y down, z away from the camera (see field_pbr.h).
//
// The game's characters are drawn before the background, so everything about them (where they stand, their shadows, the
// pictures of them reflections use, their depth) is ready when this runs.
//
// Depth: every tile keeps the game's own depth (nothing here writes the depth buffer differently from the game, so tiles
// can't change places among themselves). A pixel only steps aside (is not drawn) where a character stands in front of what
// the pixel really shows but behind the tile's own depth: the floor inside a pillar's tile, with someone in front of the
// floor and behind the pillar. "What it really shows" is the depth map, measured from the tile's nearest point, which is
// where the game's depth for the tile is.

uniform sampler2D tex;
uniform vec4 demasterTileClamp;        // FIELD_SMOOTH (as main.frag): x, y one tile in texture units; z, w one texel
uniform vec4 demasterPbr;              // x: on; y: debug view (FIELD_PBR_VIEW); z, w: cells across, down
uniform vec4 demasterSpace;            // zoom, canvas x and y of the camera axis, depth model (0 linear, 1 inverse)
uniform vec4 demasterDepth;            // a, b (distance = a + b * value, or 1 / (a + b * value)), nearest distance
uniform vec4 demasterAmbient;          // rgb: light added everywhere
uniform sampler2D demasterEC;
uniform vec4 demasterEmissive;         // the glows (_lights.txt [emissive]): x hue turn (turns), y strength; w how much shows (how night it is, or the file's "day" if that is more: 0 = none, the painting has them)
uniform vec4 demasterEmissiveTint;     // rgb: the glows times this (linear)
uniform vec4 demasterNight;            // DAY_NIGHT: x how night it is (0 day, 1 night); y the painted light left away from the night's lamps; z how much the lamps keep ([night] level, lamps)
uniform vec4 demasterNightTint;        // rgb: what's left times this ([night] tint, linear)
uniform int demasterLightCount;
uniform vec4 demasterLightPos[8];      // view x, y, z; w: range
uniform vec4 demasterLightColor[8];    // rgb: light added now (colour x intensity x flicker x its group); a: specular strength
uniform vec4 demasterLightFlicker[8];  // x: how much its painted light changes now; y: 1 when it owns the steady mask; z: shadow strength; w: 1 = a lamp on at night (the painting keeps its light around it)
uniform vec4 demasterLightPaint[8];    // the look: rgb the lamp's own colour now, for the highlights dial; a: its weight in the relight dial (0: none; negative: a moving light, whose share is also kept out by the room)
uniform vec4 demasterLook;             // the look (FIELD_PBR_LOOK): x relight, y highlights, z AO on the painting (all 0: the painting as it is); w: the relight's scale, which keeps the field's average brightness (fieldpbr::RelightScale)
uniform sampler2D demasterLF;
uniform sampler2D demasterND;
uniform sampler2D demasterORME;
uniform sampler2D demasterCells;

uniform vec4 demasterStepAside;        // x: on (FIELD_PBR_DEPTH, the characters' depth is there); y: depth buffer per game depth unit; z, w: 1 / its size
uniform sampler2D demasterCharDepth;   // the depth buffer with only the characters in it (copied right after DEMASTER drew them)
uniform vec4 demasterQuality;          // x: wall-check steps for added lights (0: none); y: wall-check steps for reflections; z: shadow map taps (1 or 4); w: how far a wall check towards a light looks (view units)
uniform vec4 demasterSky;              // x: sky from this distance on (0: no sky); y: parallax layers (bit per layer id); z: 1 = sky
uniform sampler2D demasterRoom;        // the whole field's _ND on the canvas
uniform vec4 demasterRoomSize;         // canvas width, height (native pixels); z: 1 = there
uniform vec4 demasterFeatures;         // x: character shadows, y: reflections, z: contact darkening, w: room shadows for added lights
uniform int demasterCharCount;
uniform vec4 demasterCardCenter[8];    // a character's box centre (view); w: half width
uniform vec4 demasterCardUp[8];        // its card's up; w: half height
uniform vec4 demasterCardRight[8];     // its card's right; w: capture tiles (front 2i, back 2i + 1)
uniform vec4 demasterCardDir[8];       // from the camera to it (unit); w: half depth
uniform vec4 demasterContact[8];       // its body for contact darkening: radii along right, up, forward; w: darkness
uniform sampler2D demasterCapture;     // the pictures of the characters: 4 x 4 tiles
uniform int demasterShadowCount;
uniform mat4 demasterShadowMatrix[8];  // view to the light's clip space
uniform vec4 demasterShadowInfo[8];    // x: light; y, z: tile corner in the atlas (0-1); w: tile size
uniform sampler2D demasterShadowMap;

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

// a colour (sRGB) turned around the colour wheel, its saturation and brightness kept (through HSV)
vec3 demasterHueTurn(vec3 c, float turns)
{
	vec4 k = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	vec4 p = mix(vec4(c.bg, k.wz), vec4(c.gb, k.xy), step(c.b, c.g));
	vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
	float d = q.x - min(q.w, q.y);
	float hue = fract(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)) + turns);
	float saturation = d / (q.x + 1e-10);
	vec3 rgb = clamp(abs(mod(hue * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
	return q.x * mix(vec3(1.0), rgb, saturation);
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

// albedo and lightmap of a PBR cell, filtered as demasterTileTexture does when FIELD_SMOOTH is on
void demasterPbrFetch(vec2 uv, out vec4 albedo, out vec4 light)
{
	if (demasterTileClamp.x > 0.0)
	{
		vec2 texel = demasterTileClamp.zw;
		vec2 cell = floor(uv / demasterTileClamp.xy);
		vec2 lo = cell * demasterTileClamp.xy + 0.5 * texel;
		vec2 hi = (cell + 1.0) * demasterTileClamp.xy - 0.5 * texel;
		vec2 p = uv / texel - 0.5;
		vec2 f = fract(p);
		vec2 c0 = clamp((floor(p) + 0.5) * texel, lo, hi);
		vec2 c1 = clamp((floor(p) + 1.5) * texel, lo, hi);
		vec2 c10 = vec2(c1.x, c0.y);
		vec2 c01 = vec2(c0.x, c1.y);
		vec4 a00 = texture2D(tex, c0);
		vec4 a10 = texture2D(tex, c10);
		vec4 a01 = texture2D(tex, c01);
		vec4 a11 = texture2D(tex, c1);
		float w00 = (1.0 - f.x) * (1.0 - f.y) * a00.a;
		float w10 = f.x * (1.0 - f.y) * a10.a;
		float w01 = (1.0 - f.x) * f.y * a01.a;
		float w11 = f.x * f.y * a11.a;
		float w = w00 + w10 + w01 + w11;
		float inv = 1.0 / max(w, 0.00001);
		albedo = vec4((a00.rgb * w00 + a10.rgb * w10 + a01.rgb * w01 + a11.rgb * w11) * inv, w);
		light = (texture2D(demasterLF, c0) * w00 + texture2D(demasterLF, c10) * w10 + texture2D(demasterLF, c01) * w01
			+ texture2D(demasterLF, c1) * w11) * inv;
	}
	else
	{
		albedo = texture2D(tex, uv);
		light = texture2D(demasterLF, uv);
	}
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

// the room's distance at a canvas spot (from the whole field's depth); a big number off the picture
float demasterRoomAt(vec2 canvas)
{
	vec2 uv = canvas / demasterRoomSize.xy;
	if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
		return 1e9;
	return demasterDistance(texture2D(demasterRoom, uv).a);
}

// 1: nothing of the room between from and to; 0: blocked (the depth map as a height field, a few steps)
float demasterRoomClear(vec3 from, vec3 to, int steps)
{
	if (demasterRoomSize.z < 0.5)
		return 1.0;
	for (int i = 1; i <= 16; i++)
	{
		if (i > steps)
			break;
		float t = float(i) / float(steps + 1);
		vec3 s = mix(from, to, t);
		if (s.z <= demasterDepth.z)
			continue;
		float room = demasterRoomAt(demasterCanvasOf(s));
		// behind the surface there (but not so far behind that it is only something in front of a far wall)
		if (s.z > room * 1.02 && s.z < room * 1.35)
			return 0.0;
	}
	return 1.0;
}

// 1: lit; 0: in a character's shadow from shadow map k
float demasterShadowAt(int k, vec3 p)
{
	vec4 clip = demasterShadowMatrix[k] * vec4(p, 1.0);
	if (clip.w <= 0.0)
		return 1.0;
	vec3 ndc = clip.xyz / clip.w;
	if (abs(ndc.x) >= 1.0 || abs(ndc.y) >= 1.0 || ndc.z >= 1.0)
		return 1.0;
	vec4 info = demasterShadowInfo[k];
	vec2 uv = info.yz + (ndc.xy * 0.5 + 0.5) * info.w;
	float depth = ndc.z * 0.5 + 0.5 - 0.0015;
	vec2 texel = vec2(1.0 / 2048.0);
	if (demasterQuality.z < 2.0)
		return depth <= texture2D(demasterShadowMap, uv).r ? 1.0 : 0.0;
	float lit = 0.0;
	lit += depth <= texture2D(demasterShadowMap, uv + texel * vec2(-0.5, -0.5)).r ? 1.0 : 0.0;
	lit += depth <= texture2D(demasterShadowMap, uv + texel * vec2(0.5, -0.5)).r ? 1.0 : 0.0;
	lit += depth <= texture2D(demasterShadowMap, uv + texel * vec2(-0.5, 0.5)).r ? 1.0 : 0.0;
	lit += depth <= texture2D(demasterShadowMap, uv + texel * vec2(0.5, 0.5)).r ? 1.0 : 0.0;
	return lit * 0.25;
}

// how much the characters' bodies shut this point in (0-1, darkness included)
float demasterContactAt(vec3 p, vec3 n)
{
	float occlusion = 0.0;
	for (int i = 0; i < 8; i++)
	{
		if (i >= demasterCharCount)
			break;
		vec4 radii = demasterContact[i];
		if (radii.w <= 0.0)
			continue;
		vec3 right = demasterCardRight[i].xyz, up = demasterCardUp[i].xyz;
		vec3 forward = normalize(cross(right, up));
		vec3 d = p - demasterCardCenter[i].xyz;
		vec3 q = vec3(dot(d, right) / radii.x, dot(d, up) / radii.y, dot(d, forward) / radii.z);
		float dist = length(q);
		if (dist > 4.0)
			continue;
		vec3 nq = normalize(vec3(dot(n, right) * radii.x, dot(n, up) * radii.y, dot(n, forward) * radii.z));
		float o = dist <= 1.0 ? 1.0 : clamp(dot(nq, -q / dist), 0.0, 1.0) / (dist * dist);
		occlusion = max(occlusion, clamp(o, 0.0, 1.0) * radii.w);
	}
	return occlusion;
}

// the characters in this surface, as the reflected ray sees them: rgb (linear, premultiplied), alpha
vec4 demasterReflectionAt(vec3 p, vec3 r, float roughness)
{
	vec4 best = vec4(0.0);
	float bestT = 1e9;
	for (int i = 0; i < 8; i++)
	{
		if (i >= demasterCharCount)
			break;
		vec3 c = demasterCardCenter[i].xyz;
		vec3 dir = demasterCardDir[i].xyz;
		float facing = dot(r, dir);
		if (abs(facing) < 0.0001)
			continue;
		float t = dot(c - p, dir) / facing;
		if (t <= 0.0 || t >= bestT)
			continue;
		vec3 hit = p + r * t;
		vec3 local = hit - c;
		float u = 0.5 + 0.5 * dot(local, demasterCardRight[i].xyz) / demasterCardCenter[i].w;
		float v = 0.5 + 0.5 * dot(local, demasterCardUp[i].xyz) / demasterCardUp[i].w;
		if (u <= 0.0 || u >= 1.0 || v <= 0.0 || v >= 1.0)
			continue;
		// a ray going away from the camera meets the side the camera sees; one coming back meets the back
		float tileIndex = demasterCardRight[i].w + (facing > 0.0 ? 0.0 : 1.0);
		vec2 tile = vec2(mod(tileIndex, 4.0), floor(tileIndex / 4.0)) * 0.25;
		vec4 picture = texture2D(demasterCapture, tile + vec2(u, v) * 0.25, roughness * 5.0);
		if (picture.a <= 0.01)
			continue;
		// a wall in between
		if (demasterQuality.y > 0.5 && demasterRoomClear(p, hit, int(demasterQuality.y + 0.5)) < 0.5)
			continue;
		bestT = t;
		best = vec4(demasterToLinear(clamp(picture.rgb / max(picture.a, 0.001), 0.0, 1.0)) * picture.a, picture.a);
	}
	return best;
}

// DAY_NIGHT: how much of the painted light is left here. Away from the lamps on at night it goes down to [night] level x
// tint; within a lamp's range it keeps [night] lamps of it, most at the lamp and fading out towards the edge of its range.
// keep: the most any night lamp keeps here (0-1), from the loop over the lights
vec3 demasterNightLeft(float keep)
{
	vec3 dark = demasterNight.y * demasterNightTint.rgb;
	return mix(vec3(1.0), mix(dark, vec3(1.0), keep * demasterNight.z), demasterNight.x);
}

// Light added on top of the painting (the lights, reflections), rolled off towards white instead of cut off at it: the
// first of it counts in full, and the more there is the less each bit adds, so strong lights keep their shape and colour.
// With nothing added the painting comes back exactly.
vec3 demasterRollOff(vec3 painted, vec3 extra)
{
	vec3 base = max(painted + min(extra, 0.0), 0.0);
	vec3 room = max(1.0 - base, 0.0001);
	return base + room * (1.0 - exp(-max(extra, 0.0) / room));
}

// false: not a PBR cell (drawn as it is)
bool demasterPbrShade(vec2 uv, out vec4 result)
{
	result = vec4(0.0);
	vec2 cells = demasterPbr.zw;
	vec2 cellPos = uv * cells;
	vec2 cellIndex = floor(cellPos);
	vec4 cell = texture2D(demasterCells, (cellIndex + 0.5) / vec2(cells.x, cells.y * 2.0));
	if (cell.z < 0.5)
		return false;
	vec4 cellMore = texture2D(demasterCells, (cellIndex + vec2(0.5, 0.5 + cells.y)) / vec2(cells.x, cells.y * 2.0));
	float flags = cell.z;
	bool hasGlows = flags > 15.5;
	if (hasGlows)
		flags -= 16.0;
	bool hasEmissive = flags > 7.5;
	if (hasEmissive)
		flags -= 8.0;
	bool hasOrme = flags > 3.5;
	if (hasOrme)
		flags -= 4.0;
	bool hasNormal = flags > 1.5;

	vec4 albedo;
	vec4 light;
	demasterPbrFetch(uv, albedo, light);
	if (albedo.a <= 0.0)
		discard;
	vec4 nd = hasNormal ? texture2D(demasterND, uv) : vec4(0.5, 0.5, 1.0, 0.5);
	vec4 orme = hasOrme ? texture2D(demasterORME, uv) : vec4(1.0, 0.8, 0.0, 0.0);
	// the glows: a picture of them (its alpha: how much of the painting it covers), else the old mask in the ORM map's alpha
	vec4 glows = hasGlows ? texture2D(demasterEC, uv) : vec4(0.0);
	float emissive = hasGlows ? glows.a : hasEmissive ? orme.a : 0.0;

	// where this pixel is: its tile's place on the canvas, the depth map, the camera
	vec2 canvas = cell.xy + (cellPos - cellIndex) * 16.0;
	float z = demasterDistance(nd.a);
	vec3 position = vec3((canvas - demasterSpace.yz) * z / demasterSpace.x, z);
	vec3 normal = hasNormal ? normalize(vec3(nd.r * 2.0 - 1.0, 1.0 - nd.g * 2.0, 1.0 - nd.b * 2.0)) : vec3(0.0, 0.0, -1.0);
	vec3 view = normalize(-position);

	// sky: a layer that moves on its own (parallax, scrolled by script), or the farthest depth with sky = 1
	float layerBit = exp2(floor(cellMore.y + 0.5));
	bool parallax = mod(floor(demasterSky.y / layerBit), 2.0) > 0.5;
	bool sky = parallax || (demasterSky.x > 0.0 && z >= demasterSky.x);

	// a character behind the tile's own depth but in front of what this pixel really shows: the pixel steps aside. The
	// game's depth for the tile is where its nearest part is; this pixel is z / nearest times as far (15% kept as a margin,
	// so a flat or gently sloping tile never lets anything through)
	float behind = cellMore.z > 0.0 ? z / cellMore.z : 1.0;
	if (demasterStepAside.x > 0.5 && hasNormal && !sky && behind > 1.15)
	{
		float character = texture2D(demasterCharDepth, gl_FragCoord.xy * demasterStepAside.zw).r;
		if (character < 1.0 && character >= gl_FragCoord.z)
		{
			float tileDepth = 1.0 / gl_FragCoord.w;
			float shown = gl_FragCoord.z + demasterStepAside.y * tileDepth * (behind / 1.15 - 1.0);
			if (character < shown)
				discard;
		}
	}

	vec3 base = demasterToLinear(albedo.rgb);
	vec3 baked = demasterToLinear(light.rgb) * cell.w;
	float mask = 1.0 - light.a;
	float ao = orme.r;
	float roughness = clamp(orme.g, 0.045, 1.0);
	float metallic = clamp(orme.b, 0.0, 1.0);

	// the glow: the picture's colour, else (the old mask) the painting's own colour there; turned by the lights file's hue,
	// tinted, times its strength. Only worked out when it shows: at night, or in FIELD_PBR_VIEW 16
	vec3 glow = vec3(0.0);
	if ((emissive > 0.0 || hasGlows) && (demasterEmissive.w > 0.0 || demasterPbr.y > 15.5))
	{
		vec3 colour = hasGlows ? glows.rgb : demasterToSrgb(base * baked);
		if (demasterEmissive.x != 0.0)
			colour = demasterHueTurn(colour, demasterEmissive.x);
		glow = demasterToLinear(colour) * demasterEmissiveTint.rgb * demasterEmissive.y;
	}

	// the characters' shadows, per light
	float lightShadow[8];
	for (int i = 0; i < 8; i++)
		lightShadow[i] = 1.0;
	bool effects = !sky;
	if (effects && demasterFeatures.x > 0.5)
		for (int k = 0; k < 8; k++)
		{
			if (k >= demasterShadowCount)
				break;
			int li = int(clamp(demasterShadowInfo[k].x + 0.5, 0.0, 7.0)); // (GLSL 1.20 has no clamp for whole numbers)
			lightShadow[li] = min(lightShadow[li], demasterShadowAt(k, position + normal * position.z * 0.004));
		}

	float contact = effects && demasterFeatures.z > 0.5 ? demasterContactAt(position, normal) : 0.0;

	// One pass over the lights for everything they do here:
	//  - painted light: the part under the steady mask follows the nearest flickering light; characters standing in a lamp's
	//    light take away part of its painted light; at night the painting keeps its light around the night's lamps
	//  - the look's relight dial: how this pixel's normal faces the lamps that are on, against a surface half turned to them
	//  - added light: Lambert + GGX (Smith-Schlick, Schlick Fresnel), kept out of crevices by AO; and the look's highlights
	//    dial: the painted lamps' highlights without their diffuse light a second time
	vec3 diffuseColour = base * (1.0 - metallic);
	vec3 f0 = mix(vec3(0.04), base, metallic);
	float alpha2 = roughness * roughness * roughness * roughness;
	float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
	float nv = max(dot(normal, view), 0.0001);
	vec3 added = demasterAmbient.rgb * base * (1.0 - contact);
	float shadowsSeen = 1.0;
	float nearest = 1e30;
	float change = 0.0;
	float paintedShadow = 1.0;
	float nightKeep = 0.0;
	float shadeSum = 0.0;
	float shadeWeight = 0.0;
	bool relight = demasterLook.x > 0.0 && hasNormal && !sky;
	bool wallChecks = demasterFeatures.w > 0.5 && demasterQuality.x > 0.5;
	for (int i = 0; i < 8; i++)
	{
		if (i >= demasterLightCount)
			break;
		vec4 lightPos = demasterLightPos[i];
		vec4 flicker = demasterLightFlicker[i];
		vec3 toLight = lightPos.xyz - position;
		float dd = dot(toLight, toLight);
		float x2 = dd / (lightPos.w * lightPos.w); // (distance / range) squared
		if (flicker.y > 0.5 && dd < nearest)
		{
			nearest = dd;
			change = flicker.x;
		}
		float window = clamp(1.0 - x2, 0.0, 1.0);
		if (lightShadow[i] < 1.0)
			paintedShadow *= 1.0 - (1.0 - lightShadow[i]) * flicker.z * window;
		if (flicker.w > 0.0)
			nightKeep = max(nightKeep, window * window);
		if (sky || x2 >= 1.0)
			continue;
		vec4 colour = demasterLightColor[i];
		vec4 paint = demasterLightPaint[i];
		vec3 gloss = paint.rgb * demasterLook.y;
		bool adds = colour.r + colour.g + colour.b > 0.0;
		bool shines = gloss.r + gloss.g + gloss.b > 0.0;
		float shadeShare = relight ? abs(paint.a) : 0.0;
		if (!adds && !shines && shadeShare <= 0.0)
			continue;
		float inverse = inversesqrt(max(dd, 0.000000000001));
		vec3 l = toLight * inverse;
		float edge = 1.0 - x2 * x2;
		float falloff = edge * edge / (1.0 + 8.0 * x2);
		float nl = dot(normal, l);
		float lit = lightShadow[i];
		// a wall of the room in between: for light that is added (not for highlights alone), and for a moving light's share
		// of the painted light. It looks demasterQuality.w far towards the light (a far light: the part near this pixel)
		bool moving = paint.a < 0.0;
		float clear = 1.0;
		if (wallChecks && ((adds && nl > 0.0 && lit > 0.0) || (moving && shadeShare > 0.0)))
			clear = demasterRoomClear(position, position + toLight * min(1.0, demasterQuality.w * inverse), int(demasterQuality.x + 0.5));
		if (shadeShare > 0.0)
		{
			float weight = shadeShare * falloff;
			shadeSum += weight * clamp((nl + 0.5) / 1.5, 0.0, 1.0) * (moving ? clear : 1.0);
			shadeWeight += weight;
		}
		if (nl <= 0.0 || !(adds || shines))
			continue;
		if (adds)
		{
			lit *= clear;
			shadowsSeen = min(shadowsSeen, lit);
		}
		if (lit <= 0.0)
			continue;
		vec3 h = normalize(l + view);
		float nh = max(dot(normal, h), 0.0);
		float vh = max(dot(view, h), 0.0);
		float ggx = nh * nh * (alpha2 - 1.0) + 1.0;
		float distribution = alpha2 / (PI * ggx * ggx);
		float geometry = (nv / (nv * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k));
		vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
		vec3 specular = distribution * geometry * fresnel * (PI / max(4.0 * nv * nl, 0.0001));
		added += (falloff * nl * lit) * (colour.rgb * (diffuseColour + specular * colour.a) + gloss * specular);
	}
	added *= ao;

	vec3 painted = base * baked * max(1.0 + mask * change, 0.0) * paintedShadow * (1.0 - contact) * demasterNightLeft(nightKeep);
	// the look: the painted light follows the normal map and the AO map. shadeSum / shadeWeight is how this pixel faces the
	// lamps that reach it (0 away, 1 towards them); demasterLook.w scales it so the field as a whole keeps its brightness
	// (1.5: a surface half turned to its lamps stays as it is); 0.25 is the share no lamp moves, so where no lamp reaches,
	// nothing changes. A glowing pixel is a light itself: left alone
	float look = 1.0;
	if (shadeWeight > 0.0)
		look = max(1.0 + demasterLook.x * ((0.25 + shadeSum * demasterLook.w) / (0.25 + shadeWeight) - 1.0), 0.05);
	look *= max(1.0 - demasterLook.z * (1.0 - ao), 0.0);
	painted *= mix(look, 1.0, emissive);
	// at night a glowing pixel keeps its own light: the glow takes the dimmed painting's place there (it isn't added on top:
	// the painting has the glow in it already, and adding it again burnt lamps out to white)
	// A picture of the glows: its colour already holds its coverage, so a lit window (coverage 1) takes the painting's place
	// and a soft halo (coverage 0) only adds its light
	float showing = clamp(demasterEmissive.w, 0.0, 1.0);
	if (hasGlows)
		painted = painted * (1.0 - emissive * showing) + glow * showing;
	else
		painted = mix(painted, glow, clamp(emissive * demasterEmissive.w, 0.0, 1.0));

	// the characters reflected in glossy and metal surfaces
	vec3 reflected = vec3(0.0);
	if (effects && demasterFeatures.y > 0.5 && demasterCharCount > 0 && roughness < 0.75)
	{
		vec3 r = reflect(-view, normal);
		vec4 seen = demasterReflectionAt(position, r, roughness);
		if (seen.a > 0.0)
		{
			vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - nv, 5.0);
			float sharp = (1.0 - roughness) * (1.0 - roughness);
			// what the reflection hides: the room the painting reflects there
			vec3 under = (painted + added) * fresnel * seen.a * sharp;
			reflected = (seen.rgb * fresnel * sharp - under) * ao;
		}
	}

	// nothing added (no light reaches this pixel, no reflection): the painting as it is, without the roll-off's arithmetic
	vec3 extra = added + reflected;
	vec3 rgb = demasterToSrgb(extra == vec3(0.0) ? painted : demasterRollOff(painted, extra));
	int viewMode = int(demasterPbr.y + 0.5);
	if (viewMode == 1)
		rgb = albedo.rgb;
	else if (viewMode == 2)
		rgb = light.rgb;
	else if (viewMode == 3)
		rgb = nd.rgb;
	else if (viewMode == 4)
		rgb = vec3(nd.a);
	else if (viewMode == 5)
		rgb = vec3(orme.r);
	else if (viewMode == 6)
		rgb = vec3(orme.g);
	else if (viewMode == 7)
		rgb = vec3(orme.b);
	else if (viewMode == 8)
		rgb = vec3(mask);
	else if (viewMode == 9)
		rgb = demasterToSrgb(added);
	else if (viewMode == 10)
		rgb = demasterToSrgb(base * baked);
	else if (viewMode == 12)
		rgb = sky ? vec3(0.2, 0.4, 1.0) : vec3(paintedShadow * shadowsSeen * (1.0 - contact));
	else if (viewMode == 13)
		rgb = demasterToSrgb(max(reflected, 0.0) * 4.0);
	else if (viewMode == 14)
	{
		// how far behind its tile's nearest point each pixel is (where a character can stand in between and be seen):
		// black = at the tile's own depth, white = twice as far or more; orange: tiles without a nearest point (the game's
		// depth only)
		rgb = sky ? vec3(0.2, 0.4, 1.0) : cellMore.z > 0.0 ? vec3(clamp(behind - 1.0, 0.0, 1.0)) : vec3(1.0, 0.5, 0.0);
	}
	else if (viewMode == 16)
		rgb = demasterToSrgb(hasGlows ? glow : glow * emissive); // the glows as they'll show at night (black: none)
	else if (viewMode == 17)
		rgb = vec3(emissive); // the emissive map (with a picture of the glows: its coverage)
	result = vec4(rgb, albedo.a);
	return true;
}

void main()
{
	vec4 color = gl_Color;
	vec4 pbr;
	if (demasterPbr.x > 0.0 && demasterPbrShade(gl_TexCoord[0].st, pbr))
	{
		color.rgb *= pbr.rgb;
		color.a = pbr.a;
		gl_FragColor = color;
		return;
	}
	vec4 texture_color = demasterTileClamp.x > 0.0 ? demasterTileTexture(gl_TexCoord[0].st) : texture2D(tex, gl_TexCoord[0].st);
	if (texture_color.a <= 0.0)
		discard;
	color.rgb *= texture_color.rgb;
	color.a = texture_color.a;
	gl_FragColor = color;
}
