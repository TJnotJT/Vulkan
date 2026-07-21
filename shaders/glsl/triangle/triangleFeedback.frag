#version 450

#ifndef MODE
#define MODE 0 // 0 for base, 1 for blend.
#endif

layout (location = 0) in vec4 inColor;
layout (location = 1) in vec2 inTex;

layout (location = 0) out vec4 outFragColor;

layout (set = 0, binding = 1) uniform sampler2D samplerColor;
layout (set = 0, binding = 2) uniform sampler2D samplerDepth;

layout (set = 0, binding = 3) uniform sampler2D imageBase;
layout (set = 0, binding = 4) uniform sampler2D imageBlend;

layout (set = 0, binding = 0) uniform UBO 
{
	mat4 projectionMatrix;
	mat4 modelMatrix;
	mat4 viewMatrix;
	vec4 rtSize;
} ubo;

void main()
{
	// Load curent color/depth.
	vec4 currColor = texture(samplerColor, gl_FragCoord.xy / ubo.rtSize.xy);
	float currZ = texture(samplerDepth, gl_FragCoord.xy / ubo.rtSize.xy).x;

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

	// Write back depth (color is already written in outFragColor).
	gl_FragDepth = outZ;
}