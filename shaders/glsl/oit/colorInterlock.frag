#version 450

#extension GL_ARB_fragment_shader_interlock : require

layout (early_fragment_tests) in;

layout(pixel_interlock_ordered) in;

layout (location = 0) in vec4 inColor;

layout (set = 0, binding = 2, rgba8) uniform coherent image2D outputImage;

void main()
{
    beginInvocationInterlockARB();

    vec3 currColor = imageLoad(outputImage, ivec2(gl_FragCoord.xy)).rgb;
    vec3 newColor = mix(currColor, inColor.rgb, inColor.a);
    imageStore(outputImage, ivec2(gl_FragCoord.xy), vec4(newColor, 1.0));

    endInvocationInterlockARB();
}