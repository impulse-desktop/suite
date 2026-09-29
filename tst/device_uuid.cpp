// The Vulkan device's UUID, the one a compositor names a shared buffer's
// exporting device with, as 32 lowercase hex digits: the first device the
// loader offers, which under VK_DRIVER_FILES is the one the tool gets too.
#include <stdio.h>
#include <vulkan/vulkan.h>

int main() {
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) {
        fprintf(stderr, "device_uuid: no vulkan instance\n");
        return 1;
    }
    uint32_t count = 1;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    const VkResult result = vkEnumeratePhysicalDevices(instance, &count, &device);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
        fprintf(stderr, "device_uuid: no vulkan device\n");
        return 1;
    }
    VkPhysicalDeviceIDProperties ids = {};
    ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 props = {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &ids;
    vkGetPhysicalDeviceProperties2(device, &props);
    for (int i = 0; i < VK_UUID_SIZE; i++) {
        printf("%02x", ids.deviceUUID[i]);
    }
    printf("\n");
    vkDestroyInstance(instance, nullptr);
    return 0;
}
