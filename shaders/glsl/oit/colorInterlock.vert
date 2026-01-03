#version 450

layout (location = 0) in vec3 inPos;

layout (location = 0) out vec4 outColor;

layout (set = 0, binding = 0) uniform RenderPassUBO
{
    mat4 projection;
    mat4 view;
} renderPassUBO;

struct ObjectData
{
    mat4 model;
    vec4 color;
};

layout (set = 0, binding = 1) readonly buffer ObjectDataSSBO
{
    ObjectData objectData[];
};

void main()
{
    mat4 PVM = renderPassUBO.projection * renderPassUBO.view * objectData[gl_InstanceIndex].model;
    gl_Position = PVM * vec4(inPos, 1.0);
    outColor = objectData[gl_InstanceIndex].color;
}
