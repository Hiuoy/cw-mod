// The look of a brush in the editor: a plain surface with a grid in game units (minor lines every
// 16 units, major every 128), so sizes read the way they will in the game.
shader_type spatial;

uniform vec3 base_color : source_color = vec3(0.62, 0.62, 0.6);
uniform float minor_units = 16.0;
uniform float major_units = 128.0;

varying vec3 world_pos;
varying vec3 world_normal;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

float grid_line(vec2 p, float spacing) {
	vec2 q = p / spacing;
	vec2 g = abs(fract(q - 0.5) - 0.5) / max(fwidth(q), vec2(1e-5));
	return 1.0 - clamp(min(g.x, g.y), 0.0, 1.0);
}

void fragment() {
	vec3 n = abs(world_normal);
	vec2 p = n.y > 0.5 ? world_pos.xz : (n.x > 0.5 ? world_pos.zy : world_pos.xy);
	p *= 39.37008; // meters to game units
	float line = max(grid_line(p, minor_units) * 0.25, grid_line(p, major_units) * 0.6);
	ALBEDO = mix(base_color, base_color * 0.45, line);
	ROUGHNESS = 0.9;
}
