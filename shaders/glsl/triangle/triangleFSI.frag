#version 450

#ifndef MODE
#define MODE 0 // 0 for base, 1 for blend.
#endif

#extension GL_ARB_fragment_shader_interlock : require

// layout (early_fragment_tests) in;

layout(pixel_interlock_ordered) in;

layout (location = 0) in vec4 inColor;
layout (location = 1) in vec2 inTex;

layout (location = 0) out vec4 outFragColor;

layout (set = 0, binding = 1, rgba8) uniform coherent image2D fsiImageColor;
layout (set = 0, binding = 2, r32f) uniform coherent image2D fsiImageDepth;

layout (set = 0, binding = 3) uniform sampler2D imageBase;
layout (set = 0, binding = 4) uniform sampler2D imageBlend;

void main()
{
	beginInvocationInterlockARB();

	// Load curent color/depth.
	vec4 currColor = imageLoad(fsiImageColor, ivec2(gl_FragCoord.xy));
	float currZ = imageLoad(fsiImageDepth, ivec2(gl_FragCoord.xy)).x;

	// Emulate Z test (less equal).
	float inputZ = gl_FragCoord.z;
	bool failZ = inputZ > currZ;

	vec4 inColor2 = inColor;
#if MODE == 0
	vec4 texColor = texture(imageBase, inTex);
#else
	vec4 texColor = texture(imageBlend, inTex);
#endif

	// Adjust for PS2 scaling.
	inColor2.a *= 255.0f / 128.0f;
	texColor *= 255.0f / 128.0f;

	inColor2 *= texColor; // texture modulate

#if MODE == 0
	outFragColor = inColor2;
#else
	outFragColor = vec4(inColor2.rbg * currColor.a + currColor.rgb, inColor2.a); // Cs * Ad + Cd
#endif

	// Discard if fail Z test.
	outFragColor = failZ ? currColor : outFragColor;
	float outZ = failZ ? currZ : inputZ;

	// Write back.
	imageStore(fsiImageColor, ivec2(gl_FragCoord.xy), outFragColor);
	imageStore(fsiImageDepth, ivec2(gl_FragCoord.xy), vec4(outZ, 0, 0, 0));

	endInvocationInterlockARB();
}