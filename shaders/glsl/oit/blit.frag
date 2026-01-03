#version 450

layout (location = 0) out vec4 outFragColor;

layout (set = 0, binding = 0, rgba8) uniform image2D inputImage;

void main()
{
    outFragColor = imageLoad(inputImage, ivec2(gl_FragCoord.xy));
}