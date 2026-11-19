#version 450
// Draws one axis-aligned rectangle as a 4-vertex triangle strip.
// rect = (x0, y0, x1, y1) in NDC.
layout(push_constant) uniform PC { vec4 rect; vec4 color; } pc;
void main() {
	int i = gl_VertexIndex;
	float x = ((i & 1) != 0) ? pc.rect.z : pc.rect.x;
	float y = ((i & 2) != 0) ? pc.rect.w : pc.rect.y;
	gl_Position = vec4(x, y, 0.0, 1.0);
}
