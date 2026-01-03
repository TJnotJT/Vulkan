/*
* Vulkan Example - Order Independent Transparency rendering using linked lists
*
* Copyright by Sascha Willems - www.saschawillems.de
* Copyright by Daemyung Jang  - dm86.jang@gmail.com
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include "vulkanexamplebase.h"
#include "VulkanglTFModel.h"

#define NODE_COUNT 20

class VulkanExample : public VulkanExampleBase
{
public:
	VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT extInterlock {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
		.pNext = nullptr,
		.fragmentShaderSampleInterlock = VK_FALSE,
		.fragmentShaderPixelInterlock = VK_TRUE,
		.fragmentShaderShadingRateInterlock = VK_FALSE
	};

	struct {
		vkglTF::Model sphere;
		vkglTF::Model cube;
	} models;

	struct Node {
		glm::vec4 color;
		float depth{ 0.0f };
		uint32_t next{ 0 };
	};

	static constexpr int NUM_SPHERES = 125;

	struct BlendPass {
		VkRenderPass renderPass{ VK_NULL_HANDLE };
		VkFramebuffer framebuffer{ VK_NULL_HANDLE };
		vks::Buffer objectDataBuffer;
		vks::Buffer outputBuffer;
		vks::Texture outputTex;
	} blendPass;

	struct BlitPass {
		VkRenderPass renderPass{ VK_NULL_HANDLE };
		VkFramebuffer framebuffer{ VK_NULL_HANDLE };
		vks::Texture inputTex;
	} blitPass;

	struct RenderPassUniformData {
		glm::mat4 projection;
		glm::mat4 view;
	} renderPassUniformData;
	std::array<vks::Buffer, maxConcurrentFrames> renderPassUniformBuffer;

	struct ObjectData {
		glm::mat4 model;
		glm::vec4 color;
	};

	struct {
		VkDescriptorSetLayout blend{ VK_NULL_HANDLE };
		VkDescriptorSetLayout blit{ VK_NULL_HANDLE };
	} descriptorSetLayouts;

	struct {
		VkPipelineLayout blend{ VK_NULL_HANDLE };
		VkPipelineLayout blit{ VK_NULL_HANDLE };
	} pipelineLayouts;

	struct {
		VkPipeline blend{ VK_NULL_HANDLE };
		VkPipeline blit{ VK_NULL_HANDLE };
	} pipelines;

	struct DescriptorSets {
		VkDescriptorSet blend{ VK_NULL_HANDLE };
		VkDescriptorSet blit{ VK_NULL_HANDLE };
	};
	std::array<DescriptorSets, maxConcurrentFrames> descriptorSets{};

	VkDeviceSize objectUniformBufferSize{ 0 };

	VulkanExample() : VulkanExampleBase()
	{
		title = "Order independent transparency rendering";
		camera.type = Camera::CameraType::lookat;
		camera.setPosition(glm::vec3(0.0f, 0.0f, -6.0f));
		camera.setRotation(glm::vec3(0.0f, 0.0f, 0.0f));
		camera.setPerspective(60.0f, (float) width / (float) height, 0.1f, 256.0f);
	}

	~VulkanExample()
	{
		if (device) {
			vkDestroyPipeline(device, pipelines.blend, nullptr);
			vkDestroyPipelineLayout(device, pipelineLayouts.blend, nullptr);
			vkDestroyPipelineLayout(device, pipelineLayouts.blit, nullptr);
			vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.blend, nullptr);
			vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.blit, nullptr);
			destroyBlendPass();
			for (auto& buffer : renderPassUniformBuffer) {
				buffer.destroy();
			}
		}
	}

	void getEnabledFeatures() override
	{
		// The linked lists are built in a fragment shader using atomic stores, so the sample won't work without that feature available
		if (deviceFeatures.fragmentStoresAndAtomics) {
			enabledFeatures.fragmentStoresAndAtomics = VK_TRUE;
		} else {
			vks::tools::exitFatal("Selected GPU does not support stores and atomic operations in the fragment stage", VK_ERROR_FEATURE_NOT_PRESENT);
		}
	};

	void loadAssets()
	{
		const uint32_t glTFLoadingFlags = vkglTF::FileLoadingFlags::PreTransformVertices | vkglTF::FileLoadingFlags::FlipY;
		models.sphere.loadFromFile(getAssetPath() + "models/sphere.gltf", vulkanDevice, queue, glTFLoadingFlags);
		models.cube.loadFromFile(getAssetPath() + "models/cube.gltf", vulkanDevice, queue, glTFLoadingFlags);
	}

	void prepareUniformBuffers()
	{
		for (auto& buffer : renderPassUniformBuffer) {
			// Create an uniform buffer for a render pass.
			VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buffer, sizeof(RenderPassUniformData)));
			VK_CHECK_RESULT(buffer.map());
		}
	}

	void prepareBlendPass()
	{
		VkSubpassDescription subpassDescription = {};
		subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

		// Geometry render pass doesn't need any output attachment.
		VkRenderPassCreateInfo renderPassInfo = vks::initializers::renderPassCreateInfo();
		renderPassInfo.attachmentCount = 0;
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpassDescription;

		VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassInfo, nullptr, &blendPass.renderPass));

		// Frame buffer doesn't need any output attachment since we render to image in fragment shader
		VkFramebufferCreateInfo fbufCreateInfo = vks::initializers::framebufferCreateInfo();
		fbufCreateInfo.renderPass = blendPass.renderPass;
		fbufCreateInfo.attachmentCount = 0;
		fbufCreateInfo.width = width;
		fbufCreateInfo.height = height;
		fbufCreateInfo.layers = 1;

		VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &blendPass.framebuffer));

		// Create a buffer for object data
		vks::Buffer stagingBuffer;
		constexpr int bufferSize = NUM_SPHERES * sizeof(ObjectData);
	
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&stagingBuffer,
			bufferSize));
		VK_CHECK_RESULT(stagingBuffer.map());

		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			&blendPass.objectDataBuffer,
			bufferSize));

		// Set up ModelMatrixSSBO data.
		//objectData.color = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f);
		std::vector<ObjectData> objectData;
		for (int32_t x = 0; x < 5; x++)
		{
			for (int32_t y = 0; y < 5; y++)
			{
				for (int32_t z = 0; z < 5; z++)
				{
					glm::mat4 T = glm::translate(glm::mat4(1.0f), glm::vec3(x - 2, y - 2, z - 2));
					glm::mat4 S = glm::scale(glm::mat4(1.0f), glm::vec3(0.3f));
					objectData.push_back({ T * S, { 1.0f, 0.0f, 0.0f, 0.5f } });
				}
			}
		}
		assert(objectData.size() == static_cast<size_t>(NUM_SPHERES));

		memcpy(stagingBuffer.mapped, objectData.data(), bufferSize);

		// Copy data to device
		VkCommandBuffer copyCmd = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
		VkBufferCopy copyRegion = {};
		copyRegion.size = bufferSize;
		vkCmdCopyBuffer(copyCmd, stagingBuffer.buffer, blendPass.objectDataBuffer.buffer, 1, &copyRegion);
		vulkanDevice->flushCommandBuffer(copyCmd, queue, true);

		stagingBuffer.destroy();
		
		// Create a texture for output
		blendPass.outputTex.device = vulkanDevice;

		VkImageCreateInfo imageInfo = vks::initializers::imageCreateInfo();
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.extent.width = width;
		imageInfo.extent.height = height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
#if (defined(VK_USE_PLATFORM_IOS_MVK) || defined(VK_USE_PLATFORM_MACOS_MVK) || defined(VK_USE_PLATFORM_METAL_EXT))
		// SRS - On macOS/iOS use linear tiling for atomic image access, see https://github.com/KhronosGroup/MoltenVK/issues/1027
		imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
#else
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
#endif
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

		VK_CHECK_RESULT(vkCreateImage(device, &imageInfo, nullptr, &blendPass.outputTex.image));

		blendPass.outputTex.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(device, blendPass.outputTex.image, &memReqs);

		VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &blendPass.outputTex.deviceMemory));
		VK_CHECK_RESULT(vkBindImageMemory(device, blendPass.outputTex.image, blendPass.outputTex.deviceMemory, 0));

		VkImageViewCreateInfo imageViewInfo = vks::initializers::imageViewCreateInfo();
		imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageViewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageViewInfo.flags = 0;
		imageViewInfo.image = blendPass.outputTex.image;
		imageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageViewInfo.subresourceRange.baseMipLevel = 0;
		imageViewInfo.subresourceRange.levelCount = 1;
		imageViewInfo.subresourceRange.baseArrayLayer = 0;
		imageViewInfo.subresourceRange.layerCount = 1;

		VK_CHECK_RESULT(vkCreateImageView(device, &imageViewInfo, nullptr, &blendPass.outputTex.view));

		blendPass.outputTex.width = width;
		blendPass.outputTex.height = height;
		blendPass.outputTex.mipLevels = 1;
		blendPass.outputTex.layerCount = 1;
		blendPass.outputTex.descriptor.imageView = blendPass.outputTex.view;
		blendPass.outputTex.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		blendPass.outputTex.sampler = VK_NULL_HANDLE;

		blitPass.inputTex = blendPass.outputTex;

		// Change output image's layout from UNDEFINED to GENERAL
		VkCommandBufferAllocateInfo cmdBufAllocInfo = vks::initializers::commandBufferAllocateInfo(cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1);

		VkCommandBuffer cmdBuf;
		VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocInfo, &cmdBuf));

		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
		VK_CHECK_RESULT(vkBeginCommandBuffer(cmdBuf, &cmdBufInfo));

		VkImageMemoryBarrier barrier = vks::initializers::imageMemoryBarrier();
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.image = blendPass.outputTex.image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		VK_CHECK_RESULT(vkEndCommandBuffer(cmdBuf));

		VkSubmitInfo submitInfo = vks::initializers::submitInfo();
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmdBuf;

		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
		VK_CHECK_RESULT(vkQueueWaitIdle(queue));
	}

	void setupDescriptors()
	{
		// Pool
		std::vector<VkDescriptorPoolSize> poolSizes = {
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxConcurrentFrames),
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxConcurrentFrames * 2),
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxConcurrentFrames),
		};
		VkDescriptorPoolCreateInfo descriptorPoolInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes, 2 * maxConcurrentFrames);
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));

		// Layouts

		// Create a geometry descriptor set layout
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
			// renderPassUniformData
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0),
			// model matrices
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 1),
			// output images
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT, 2),
		};
		VkDescriptorSetLayoutCreateInfo descriptorLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayoutCI, nullptr, &descriptorSetLayouts.blend));

		// Create a blit descriptor set layout
		setLayoutBindings = {
			// output image
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT, 0),
		};
		descriptorLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayoutCI, nullptr, &descriptorSetLayouts.blit));

		updateDescriptors();
	}

	void updateDescriptors()
	{
		// Sets per frame, just like the buffers themselves
		// Images and GPU-only SSBO do not need to be duplicated per frame, we reuse the same one for each frame
		for (auto i = 0; i < renderPassUniformBuffer.size(); i++) {
			// Images and linked buffers are recreated on resize and part of the descriptors, so we need to update those at runtime
			VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.blend, 1);
			VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSets[i].blend));
			std::vector<VkWriteDescriptorSet> writeDescriptorSets = {
				// Binding 0: renderPassUniformData
				vks::initializers::writeDescriptorSet(descriptorSets[i].blend, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &renderPassUniformBuffer[i].descriptor),
				// Binding 1: model matrix SSBO
				vks::initializers::writeDescriptorSet(descriptorSets[i].blend, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, &blendPass.objectDataBuffer.descriptor),
				// Binding 2: output image
				vks::initializers::writeDescriptorSet(descriptorSets[i].blend, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2, &blendPass.outputTex.descriptor),
			};
			vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, nullptr);

			// Update a blit descriptor set
			allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.blit, 1);
			VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSets[i].blit));
			writeDescriptorSets = {
				// Binding 0: headIndexImage
				vks::initializers::writeDescriptorSet(descriptorSets[i].blit, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0, &blitPass.inputTex.descriptor),
			};
			vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, nullptr);
		}
	}

	void preparePipelines()
	{
		// Layouts

		// Create a blend pipeline layout
		VkPipelineLayoutCreateInfo pipelineLayoutCI = vks::initializers::pipelineLayoutCreateInfo(&descriptorSetLayouts.blend, 1);
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCI, nullptr, &pipelineLayouts.blend));

		// Create a blit pipeline layout
		pipelineLayoutCI = vks::initializers::pipelineLayoutCreateInfo(&descriptorSetLayouts.blit, 1);
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCI, nullptr, &pipelineLayouts.blit));

		// Pipelines
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineRasterizationStateCreateInfo rasterizationState = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0);
		VkPipelineColorBlendStateCreateInfo colorBlendState = vks::initializers::pipelineColorBlendStateCreateInfo(0, nullptr);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_FALSE, VK_FALSE, VK_COMPARE_OP_ALWAYS);
		VkPipelineViewportStateCreateInfo viewportState = vks::initializers::pipelineViewportStateCreateInfo(1, 1, 0);
		VkPipelineMultisampleStateCreateInfo multisampleState = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
		std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};

		VkGraphicsPipelineCreateInfo pipelineCI = vks::initializers::pipelineCreateInfo(pipelineLayouts.blend, blendPass.renderPass);
		pipelineCI.pInputAssemblyState = &inputAssemblyState;
		pipelineCI.pRasterizationState = &rasterizationState;
		pipelineCI.pColorBlendState = &colorBlendState;
		pipelineCI.pMultisampleState = &multisampleState;
		pipelineCI.pViewportState = &viewportState;
		pipelineCI.pDepthStencilState = &depthStencilState;
		pipelineCI.pDynamicState = &dynamicState;
		pipelineCI.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCI.pStages = shaderStages.data();
		pipelineCI.pVertexInputState = vkglTF::Vertex::getPipelineVertexInputState({ vkglTF::VertexComponent::Position });

		// Create a blending pipeline
		shaderStages[0] = loadShader(getShadersPath() + "oit/colorInterlock.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "oit/colorInterlock.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.blend));

		VkPipelineColorBlendAttachmentState blendAttachmentState = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		colorBlendState = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);

		VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
		vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

		pipelineCI = vks::initializers::pipelineCreateInfo(pipelineLayouts.blit, renderPass);
		pipelineCI.pInputAssemblyState = &inputAssemblyState;
		pipelineCI.pRasterizationState = &rasterizationState;
		pipelineCI.pColorBlendState = &colorBlendState;
		pipelineCI.pMultisampleState = &multisampleState;
		pipelineCI.pViewportState = &viewportState;
		pipelineCI.pDepthStencilState = &depthStencilState;
		pipelineCI.pDynamicState = &dynamicState;
		pipelineCI.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCI.pStages = shaderStages.data();
		pipelineCI.pVertexInputState = &vertexInputInfo;

		shaderStages[0] = loadShader(getShadersPath() + "oit/blit.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "oit/blit.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		rasterizationState.cullMode = VK_CULL_MODE_FRONT_BIT;
		rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.blit));
	}

	void updateUniformBuffers()
	{
		renderPassUniformData.projection = camera.matrices.perspective;
		renderPassUniformData.view = camera.matrices.view;
		memcpy(renderPassUniformBuffer[currentBuffer].mapped, &renderPassUniformData, sizeof(RenderPassUniformData));
	}

	void getEnabledExtensions() override
	{
		// Make sure fragment shader interlock is enabled
		enabledDeviceExtensions.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);

		// Make sure the features is visible
		extInterlock.pNext = deviceCreatepNextChain;
		deviceCreatepNextChain = &extInterlock;
	}

	void prepare() override
	{
		VulkanExampleBase::prepare();
		loadAssets();
		prepareUniformBuffers();
		prepareBlendPass();
		setupDescriptors();
		preparePipelines();
		prepared = true;
	}

	void buildCommandBuffer()
	{
		VkCommandBuffer cmdBuffer = drawCmdBuffers[currentBuffer];
		
		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

		VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderArea.offset.x = 0;
		renderPassBeginInfo.renderArea.offset.y = 0;
		renderPassBeginInfo.renderArea.extent.width = width;
		renderPassBeginInfo.renderArea.extent.height = height;

		VkViewport viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
		VkRect2D scissor = vks::initializers::rect2D(width, height, 0, 0);

		VK_CHECK_RESULT(vkBeginCommandBuffer(cmdBuffer, &cmdBufInfo));

		// Update dynamic viewport state
		vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);

		// Update dynamic scissor state
		vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);

		VkClearColorValue clearColor{ 0.0f, 0.0f, 0.0f, 1.0f };

		VkImageSubresourceRange subresRange = {};

		subresRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresRange.levelCount = 1;
		subresRange.layerCount = 1;

		vkCmdClearColorImage(cmdBuffer, blendPass.outputTex.image, VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &subresRange);

		// We need a barrier to make sure all writes are finished before we draw
		VkMemoryBarrier memoryBarrier = vks::initializers::memoryBarrier();
		memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
		
		// Begin the geometry render pass
		renderPassBeginInfo.renderPass = blendPass.renderPass;
		renderPassBeginInfo.framebuffer = blendPass.framebuffer;
		renderPassBeginInfo.clearValueCount = 0;
		renderPassBeginInfo.pClearValues = nullptr;

		vkCmdBeginRenderPass(cmdBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.blend);
		vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.blend, 0, 1, &descriptorSets[currentBuffer].blend, 0, nullptr);
		
		models.sphere.bindBuffers(cmdBuffer);
		vkCmdDrawIndexed(cmdBuffer, models.sphere.indices.count, NUM_SPHERES, 0, 0, 0);

		vkCmdEndRenderPass(cmdBuffer);

		// Make a pipeline barrier to guarantee the output is written and able to be blitted
		memoryBarrier = vks::initializers::memoryBarrier();
		memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);

		// Begin the blit render pass
		VkClearValue clearValues[2] = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 0 } };
		renderPassBeginInfo.renderPass = renderPass;
		renderPassBeginInfo.framebuffer = frameBuffers[currentImageIndex];
		renderPassBeginInfo.clearValueCount = 2;
		renderPassBeginInfo.pClearValues = clearValues;

		vkCmdBeginRenderPass(cmdBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.blit);
		vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.blit, 0, 1, &descriptorSets[currentBuffer].blit, 0, nullptr);
		vkCmdDraw(cmdBuffer, 3, 1, 0, 0);
		drawUI(cmdBuffer);
		vkCmdEndRenderPass(cmdBuffer);

		VK_CHECK_RESULT(vkEndCommandBuffer(cmdBuffer));
	}

	void render() override
	{
		if (!prepared)
			return;
		VulkanExampleBase::prepareFrame();
		updateUniformBuffers();
		buildCommandBuffer();
		VulkanExampleBase::submitFrame();
	}

	void windowResized() override
	{
		destroyBlendPass();
		prepareBlendPass();
		vkResetDescriptorPool(device, descriptorPool, 0);
		updateDescriptors();
		resized = false;
	}

	void destroyBlendPass()
	{
		vkDestroyRenderPass(device, blendPass.renderPass, nullptr);
		vkDestroyFramebuffer(device, blendPass.framebuffer, nullptr);
		blendPass.outputTex.destroy();
		blendPass.outputBuffer.destroy();
		blendPass.objectDataBuffer.destroy();
	}
};

VULKAN_EXAMPLE_MAIN()
