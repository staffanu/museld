// Minimal repro: divergent indexing into a push constant array returns wrong
// elements on RADV/GCN. 64 threads each read pc_data[x / 2] (and, as a control,
// ssbo_data[x / 2] with identical contents) and write the results to an SSBO.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <fstream>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); return 1; } } while (0)

int main(int argc, char **argv) {
    const char *spv_path = argc > 1 ? argv[1] : "repro.spv";
    std::ifstream f(spv_path, std::ios::binary | std::ios::ate);
    if (!f) { fprintf(stderr, "cannot open %s\n", spv_path); return 1; }
    std::vector<uint32_t> spirv(static_cast<size_t>(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char *>(spirv.data()), spirv.size() * 4);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, nullptr, &instance));

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(instance, &ndev, nullptr);
    std::vector<VkPhysicalDevice> devs(ndev);
    vkEnumeratePhysicalDevices(instance, &ndev, devs.data());
    VkPhysicalDevice phys = devs[0];
    for (auto d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
            p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys = d; break; }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("device: %s\n", props.deviceName);

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nqf);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qf.data());
    uint32_t qfi = 0;
    for (uint32_t i = 0; i < nqf; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = i; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfi;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    CHECK(vkCreateDevice(phys, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qfi, 0, &queue);

    auto makeBuffer = [&](VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem) -> int {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        CHECK(vkCreateBuffer(dev, &bci, nullptr, buf));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, *buf, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        uint32_t idx = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { idx = i; break; }
        }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = idx;
        CHECK(vkAllocateMemory(dev, &mai, nullptr, mem));
        CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
        return 0;
    };
    VkBuffer inBuf, outBuf;
    VkDeviceMemory inMem, outMem;
    if (makeBuffer(32 * 4, &inBuf, &inMem)) return 1;
    if (makeBuffer(128 * 4, &outBuf, &outMem)) return 1;

    uint32_t data[32];
    for (int i = 0; i < 32; i++) data[i] = 0x1000 + i;
    void *p;
    CHECK(vkMapMemory(dev, inMem, 0, VK_WHOLE_SIZE, 0, &p));
    memcpy(p, data, sizeof data);
    vkUnmapMemory(dev, inMem);

    VkDescriptorSetLayoutBinding binds[2]{};
    for (int i = 0; i < 2; i++) {
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 2;
    dslci.pBindings = binds;
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

    VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &dps;
    VkDescriptorPool dpool;
    CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    VkDescriptorSet dset;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));

    VkDescriptorBufferInfo dbi[2] = {{inBuf, 0, VK_WHOLE_SIZE}, {outBuf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2]{};
    for (int i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = dset;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &dbi[i];
    }
    vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spirv.size() * 4;
    smci.pCode = spirv.data();
    VkShaderModule sm;
    CHECK(vkCreateShaderModule(dev, &smci, nullptr, &sm));

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = sm;
    cpci.stage.pName = "main";
    cpci.layout = pl;
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));

    VkCommandPoolCreateInfo cpc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpc.queueFamilyIndex = qfi;
    VkCommandPool cpool;
    CHECK(vkCreateCommandPool(dev, &cpc, nullptr, &cpool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, nullptr);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 128, data); // same values as the SSBO
    vkCmdDispatch(cb, 1, 1, 1);
    CHECK(vkEndCommandBuffer(cb));

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, nullptr, &fence));
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    CHECK(vkQueueSubmit(queue, 1, &sub, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

    uint32_t out[128];
    CHECK(vkMapMemory(dev, outMem, 0, VK_WHOLE_SIZE, 0, &p));
    memcpy(out, p, sizeof out);
    vkUnmapMemory(dev, outMem);

    int errors = 0;
    for (int x = 0; x < 64; x++) {
        uint32_t want = 0x1000 + x / 2;
        if (out[x] != want) {
            printf("thread %2d: pc_data[%2d] = 0x%04x, expected 0x%04x   (ssbo control = 0x%04x)\n",
                   x, x / 2, out[x], want, out[64 + x]);
            errors++;
        }
        if (out[64 + x] != want) {
            printf("thread %2d: ssbo_data[%2d] = 0x%04x, expected 0x%04x\n", x, x / 2, out[64 + x], want);
            errors++;
        }
    }
    printf(errors ? "FAIL: %d wrong values\n" : "PASS\n", errors);
    return errors != 0;
}
