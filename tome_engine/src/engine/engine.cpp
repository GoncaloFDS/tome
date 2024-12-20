#include "engine.h"

#include "VkBootstrap.h"
#include "flecs.h"
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
#define GLFW_INCLUDE_VULKAN
#include <fstream>

#include "GLFW/glfw3.h"
#include "rendering/vulkan/vk_images.h"
#include "rendering/vulkan/vk_initializers.h"
#include "rendering/vulkan/vk_pipelines.h"
#include "spdlog/spdlog.h"
#include "slang/slang.h"
#include "slang/slang-com-ptr.h"

constexpr bool USE_VALIDATION_LAYERS = true;

static Engine *LOADED_ENGINE = nullptr;

Engine &Engine::Get() { return *LOADED_ENGINE; }

void Engine::Init() {
    assert(!LOADED_ENGINE);
    LOADED_ENGINE = this;

    if (!glfwInit()) return;

    if (!glfwVulkanSupported()) {
        spdlog::critical("GLFW Vulkan not supported!");
        return;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    _window = glfwCreateWindow(
        static_cast<int>(_windowExtent.width),
        static_cast<int>(_windowExtent.height),
        "Tome Engine",
        nullptr,
        nullptr);
    if (!_window) {
        glfwTerminate();
        return;
    }
    glfwMakeContextCurrent(_window);

    InitVulkan();
    InitSwapchain();
    InitCommands();
    InitSyncStructures();

    InitDescriptors();

    InitShaderCompiler();
    InitPipelines();

    _isInitialized = true;
}

void Engine::Cleanup() {
    if (!_isInitialized) {
        return;
    }

    vkDeviceWaitIdle(_device);

    for (FrameData &frame : _frames) {
        vkDestroyCommandPool(_device, frame.commandPool, nullptr);
        vkDestroyFence(_device, frame.renderFence, nullptr);
        vkDestroySemaphore(_device, frame.renderSemaphore, nullptr);
        vkDestroySemaphore(_device, frame.swapchainSemaphore, nullptr);
        frame.deletionQueue.Flush();
    }

    _deletionQueue.Flush();

    DestroySwapchain();
    vkDestroySurfaceKHR(_instance, _surface, nullptr);
    vkDestroyDevice(_device, nullptr);

    vkb::destroy_debug_utils_messenger(_instance, _debugMessenger);
    vkDestroyInstance(_instance, nullptr);

    glfwDestroyWindow(_window);
    glfwTerminate();

    LOADED_ENGINE = nullptr;
}

void Engine::Draw() {
    auto &currentFrame = GetCurrentFrame();

    // wait until gpu has finished rendering last frame
    VK_CHECK(vkWaitForFences(_device, 1, &currentFrame.renderFence, true, 1000000000));
    currentFrame.deletionQueue.Flush();
    VK_CHECK(vkResetFences(_device, 1, &currentFrame.renderFence));

    uint32_t swapchainImageIndex;
    VK_CHECK(
        vkAcquireNextImageKHR(_device,_swapchain,1000000000, currentFrame.swapchainSemaphore, nullptr, &
            swapchainImageIndex));

    const VkCommandBuffer cmd = currentFrame.mainCommandBuffer;

    VK_CHECK(vkResetCommandBuffer(cmd, 0));

    const VkCommandBufferBeginInfo cmdBeginInfo = vk::CommandBufferBeginInfo(
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    _drawExtent.width = _drawImage.imageExtent.width;
    _drawExtent.height = _drawImage.imageExtent.height;

    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    vk::TransitionImage(cmd, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    DrawBackground(cmd);

    vk::TransitionImage(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk::TransitionImage(cmd,
        _swapchainImages[swapchainImageIndex],
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    vk::CopyImageToImage(cmd, _drawImage.image, _swapchainImages[swapchainImageIndex], _drawExtent, _swapchainExtent);

    vk::TransitionImage(cmd,
        _swapchainImages[swapchainImageIndex],
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmdSubmitInfo = vk::CommandBufferSubmitInfo(cmd);
    VkSemaphoreSubmitInfo WaitSemaphoreInfo = vk::SemaphoreSubmitInfo(
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR,
        currentFrame.swapchainSemaphore);
    VkSemaphoreSubmitInfo SignalSemaphoreInfo = vk::SemaphoreSubmitInfo(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
        currentFrame.renderSemaphore);

    VkSubmitInfo2 submit = vk::SubmitInfo(&cmdSubmitInfo, &SignalSemaphoreInfo, &WaitSemaphoreInfo);

    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit,currentFrame.renderFence));

    //present
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.swapchainCount = 1;
    presentInfo.pWaitSemaphores = &currentFrame.renderSemaphore;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pImageIndices = &swapchainImageIndex;
    VK_CHECK(vkQueuePresentKHR(_graphicsQueue, &presentInfo));

    _frameNumber++;

}

void Engine::Run() {
    while (!glfwWindowShouldClose(_window)) {
        glfwSwapBuffers(_window);
        glfwPollEvents();

        Draw();
    }
}

void Engine::InitVulkan() {
    vkb::InstanceBuilder instanceBuilder;
    auto instanceResult = instanceBuilder.set_app_name("Tome App")
                                         .request_validation_layers(USE_VALIDATION_LAYERS)
                                         .use_default_debug_messenger()
                                         .require_api_version(1, 3, 0)
                                         .build();

    vkb::Instance vkbInstance = instanceResult.value();

    _instance = vkbInstance.instance;
    _debugMessenger = vkbInstance.debug_messenger;

    VK_CHECK(glfwCreateWindowSurface(_instance, _window, nullptr, &_surface));

    VkPhysicalDeviceVulkan13Features features13 = {};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.pNext = nullptr;
    features13.dynamicRendering = true;
    features13.synchronization2 = true;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = nullptr;
    features12.bufferDeviceAddress = true;
    features12.descriptorIndexing = true;

    vkb::PhysicalDeviceSelector physicalDeviceSelector{ vkbInstance };
    vkb::PhysicalDevice physicalDevice = physicalDeviceSelector.set_minimum_version(1, 3)
                                                               .set_required_features_13(features13)
                                                               .set_required_features_12(features12)
                                                               .set_surface(_surface)
                                                               .select()
                                                               .value();

    vkb::DeviceBuilder deviceBuilder{ physicalDevice };
    vkb::Device vkbDevice = deviceBuilder.build().value();
    _device = vkbDevice.device;
    _chosenGpu = physicalDevice.physical_device;

    _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    VmaAllocatorCreateInfo vmaAllocatorCreateInfo = {};
    vmaAllocatorCreateInfo.physicalDevice = _chosenGpu;
    vmaAllocatorCreateInfo.device = _device;
    vmaAllocatorCreateInfo.instance = _instance;
    vmaAllocatorCreateInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&vmaAllocatorCreateInfo, &_allocator);

    _deletionQueue.PushFunction([&]() {
        spdlog::info("Deleting allocator");
        vmaDestroyAllocator(_allocator);
    });

}

void Engine::InitSwapchain() {
    CreateSwapchain(_windowExtent.width, _windowExtent.height);

    VkExtent3D drawImageExtent = {
        _windowExtent.width,
        _windowExtent.height,
        1
    };

    _drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    _drawImage.imageExtent = drawImageExtent;

    VkImageUsageFlags drawImageUsages{};
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImageCreateInfo renderImageInfo = vk::ImageCreateInfo(_drawImage.imageFormat, drawImageUsages, drawImageExtent);

    VmaAllocationCreateInfo renderImageAllocinfo = {};
    renderImageAllocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    renderImageAllocinfo.requiredFlags = static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vmaCreateImage(_allocator,
        &renderImageInfo,
        &renderImageAllocinfo,
        &_drawImage.image,
        &_drawImage.allocation,
        nullptr);

    VkImageViewCreateInfo renderImageViewCreateInfo = vk::ImageviewCreateInfo(_drawImage.imageFormat,
        _drawImage.image,
        VK_IMAGE_ASPECT_COLOR_BIT);

    VK_CHECK(vkCreateImageView(_device,&renderImageViewCreateInfo, nullptr, &_drawImage.imageView));

    _deletionQueue.PushFunction([&]() {
        spdlog::info("Deleting image");
        vkDestroyImageView(_device, _drawImage.imageView, nullptr);
        vmaDestroyImage(_allocator, _drawImage.image, _drawImage.allocation);
    });
}

void Engine::InitCommands() {
    VkCommandPoolCreateInfo commandPoolCreateInfo = vk::CommanPollCreateInfo(_graphicsQueueFamily,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    for (auto &frame : _frames) {
        VK_CHECK(vkCreateCommandPool(_device,&commandPoolCreateInfo, nullptr, &frame.commandPool));

        VkCommandBufferAllocateInfo commandBufferAllocateInfo = vk::CommandBufferAllocateInfo(frame.commandPool);

        VK_CHECK(vkAllocateCommandBuffers(_device, &commandBufferAllocateInfo, &frame.mainCommandBuffer));
    }
}

void Engine::InitSyncStructures() {
    VkFenceCreateInfo fenceCreateInfo = vk::FenceCreateInfo(VK_FENCE_CREATE_SIGNALED_BIT);
    VkSemaphoreCreateInfo semaphoreCreateInfo = vk::SemaphoreCreateInfo();

    for (auto &frame : _frames) {
        VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &frame.renderFence));
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &frame.swapchainSemaphore));
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &frame.renderSemaphore));
    }

}

void Engine::InitDescriptors() {
    std::vector<DescriptorAllocator::PoolSizeRatio> poolSizeRatios = {
        { .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .ratio = 1 }
    };

    _globalDescriptorAllocator.InitPool(_device, 10, poolSizeRatios);

    {
        DescriptorLayoutBuilder descriptorLayoutBuilder;
        descriptorLayoutBuilder.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _drawImageDescriptorSetLayout = descriptorLayoutBuilder.Build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    _drawImageDescriptorSet = _globalDescriptorAllocator.Allocate(_device, _drawImageDescriptorSetLayout);

    VkDescriptorImageInfo descriptorImageInfo = {};
    descriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    descriptorImageInfo.imageView = _drawImage.imageView;

    VkWriteDescriptorSet drawImageWrite = {};
    drawImageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    drawImageWrite.pNext = nullptr;
    drawImageWrite.dstBinding = 0;
    drawImageWrite.dstSet = _drawImageDescriptorSet;
    drawImageWrite.descriptorCount = 1;
    drawImageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    drawImageWrite.pImageInfo = &descriptorImageInfo;

    vkUpdateDescriptorSets(_device, 1, &drawImageWrite, 0, nullptr);

    _deletionQueue.PushFunction([&]() {
        _globalDescriptorAllocator.DestroyPool(_device);
        vkDestroyDescriptorSetLayout(_device, _drawImageDescriptorSetLayout, nullptr);
    });

}

void Engine::InitShaderCompiler() {
    using namespace slang;

    createGlobalSession(_slangGlobalSession.writeRef());

    SessionDesc sessionDesc = {};

    TargetDesc targetDesc = {};
    targetDesc.format = SLANG_SPIRV;
    targetDesc.profile = _slangGlobalSession->findProfile("spirv_1_5");
    targetDesc.flags = 0;

    std::vector searchPaths = { "shaders/", "../tome_engine/shaders/" };
    sessionDesc.searchPaths = searchPaths.data();
    sessionDesc.searchPathCount = static_cast<uint32_t>(searchPaths.size());

    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;

    std::vector<CompilerOptionEntry> compilerOptions;
    compilerOptions.push_back({ .name = CompilerOptionName::EmitSpirvDirectly,
                                .value = {
                                    .kind = CompilerOptionValueKind::Int, .intValue0 = 1, .intValue1 = 0,
                                    .stringValue0 = nullptr,
                                    .stringValue1 = nullptr } });
    sessionDesc.compilerOptionEntries = compilerOptions.data();
    sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(compilerOptions.size());

    _slangGlobalSession->createSession(sessionDesc, _slangSession.writeRef());
}

void Engine::InitPipelines() {
    InitBackgroundPipelines();
}

void Engine::InitBackgroundPipelines() {
    VkPipelineLayoutCreateInfo computeLayout{};
    computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayout.pNext = nullptr;
    computeLayout.pSetLayouts = &_drawImageDescriptorSetLayout;
    computeLayout.setLayoutCount =1;

    VK_CHECK(vkCreatePipelineLayout(_device, &computeLayout, nullptr, &_gradientPipelineLayout));

    auto computeDrawShader = vk::LoadShaderModule("gradient.slang", _device, _slangSession);
    if (!computeDrawShader.has_value()) {
        return;
    }

    VkPipelineShaderStageCreateInfo stageCreateInfo = {};
    stageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageCreateInfo.pNext = nullptr;
    stageCreateInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageCreateInfo.module = computeDrawShader.value();
    stageCreateInfo.pName = "main";

    VkComputePipelineCreateInfo computePipelineCreateInfo{};
    computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineCreateInfo.pNext = nullptr;
    computePipelineCreateInfo.layout = _gradientPipelineLayout;
    computePipelineCreateInfo.stage = stageCreateInfo;

    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &_gradientPipeline));
    vkDestroyShaderModule(_device, computeDrawShader.value(), nullptr);

    _deletionQueue.PushFunction([&]() {
        vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
        vkDestroyPipeline(_device, _gradientPipeline, nullptr);
    });
}

void Engine::CreateSwapchain(uint32_t width, uint32_t height) {
    vkb::SwapchainBuilder swapchainBuilder{ _chosenGpu, _device, _surface };
    _swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

    VkSurfaceFormatKHR SurfaceFormat{ .format = _swapchainImageFormat,
                                      .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    vkb::Swapchain vkbSwapchain = swapchainBuilder.set_desired_format(SurfaceFormat)
                                                  .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                                                  .set_desired_extent(width, height)
                                                  .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                                                  .build()
                                                  .value();

    _swapchainExtent = vkbSwapchain.extent;
    _swapchain = vkbSwapchain.swapchain;
    _swapchainImages = vkbSwapchain.get_images().value();
    _swapchainImageViews = vkbSwapchain.get_image_views().value();
}

void Engine::DestroySwapchain() {
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);

    for (auto imageView : _swapchainImageViews) {
        vkDestroyImageView(_device, imageView, nullptr);
    }
}

void Engine::DrawBackground(VkCommandBuffer cmd) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _gradientPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _gradientPipelineLayout, 0, 1, &_drawImageDescriptorSet, 0, nullptr);
    vkCmdDispatch(cmd, std::ceil(_drawExtent.width/16.0), std::ceil(_drawExtent.height / 16.0) , 1);
}