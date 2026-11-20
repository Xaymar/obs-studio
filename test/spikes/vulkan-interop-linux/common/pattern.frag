#version 450
// mode 0: 8-bit unorm pattern, mode 1: 10-bit values in the top bits of 16-bit unorm (P010 layout)
layout(push_constant) uniform PC { int mode; } pc;
layout(location = 0) out vec4 o;
void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	int a = p.x * 7 + p.y * 13;
	int b = p.x * 3 + p.y * 5 + 100;
	if (pc.mode == 0)
		o = vec4(float(a & 255) / 255.0, float(b & 255) / 255.0, 0.0, 1.0);
	else
		o = vec4(float((a & 1023) << 6) / 65535.0, float((b & 1023) << 6) / 65535.0, 0.0, 1.0);
}
