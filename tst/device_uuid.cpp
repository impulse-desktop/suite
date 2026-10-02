#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static bool sampleable(VkPhysicalDevice device, VkFormat format) {
    VkDrmFormatModifierPropertiesListEXT list = {};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 properties = {};
    properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    properties.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(device, format, &properties);
    VkDrmFormatModifierPropertiesEXT* modifiers = (VkDrmFormatModifierPropertiesEXT*)calloc(list.drmFormatModifierCount + 1, sizeof(VkDrmFormatModifierPropertiesEXT));
    list.pDrmFormatModifierProperties = modifiers;
    vkGetPhysicalDeviceFormatProperties2(device, format, &properties);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    bool found = false;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
        if (modifiers[i].drmFormatModifier == 0 && (modifiers[i].drmFormatModifierTilingFeatures & needed) == needed) {
            found = true;
        }
    }
    free(modifiers);
    VkPhysicalDeviceProperties deviceProperties = {};
    vkGetPhysicalDeviceProperties(device, &deviceProperties);
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    VkExtensionProperties* extensions = (VkExtensionProperties*)calloc(count + 1, sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions);
    bool flags2 = VK_API_VERSION_MINOR(deviceProperties.apiVersion) >= 3;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(extensions[i].extensionName, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME) == 0) {
            flags2 = true;
        }
    }
    free(extensions);
    if (!found || !flags2) {
        return found;
    }
    VkDrmFormatModifierPropertiesList2EXT list2 = {};
    list2.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT;
    properties.pNext = &list2;
    vkGetPhysicalDeviceFormatProperties2(device, format, &properties);
    VkDrmFormatModifierProperties2EXT* modifiers2 = (VkDrmFormatModifierProperties2EXT*)calloc(list2.drmFormatModifierCount + 1, sizeof(VkDrmFormatModifierProperties2EXT));
    list2.pDrmFormatModifierProperties = modifiers2;
    vkGetPhysicalDeviceFormatProperties2(device, format, &properties);
    const VkFormatFeatureFlags2 needed2 = VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
    found = false;
    for (uint32_t i = 0; i < list2.drmFormatModifierCount; i++) {
        if (modifiers2[i].drmFormatModifier == 0 && (modifiers2[i].drmFormatModifierTilingFeatures & needed2) == needed2) {
            found = true;
        }
    }
    free(modifiers2);
    return found;
}

static bool importable(VkPhysicalDevice device, VkFormat format) {
    if (!sampleable(device, format)) {
        return false;
    }
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier = {};
    modifier.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
    modifier.drmFormatModifier = 0;
    modifier.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceExternalImageFormatInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external.pNext = &modifier;
    external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkPhysicalDeviceImageFormatInfo2 query = {};
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    query.pNext = &external;
    query.format = format;
    query.type = VK_IMAGE_TYPE_2D;
    query.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    query.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkExternalImageFormatProperties externalSupport = {};
    externalSupport.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 support = {};
    support.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    support.pNext = &externalSupport;
    if (vkGetPhysicalDeviceImageFormatProperties2(device, &query, &support) != VK_SUCCESS) {
        return false;
    }
    return (externalSupport.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
}

int main(int argc, char** argv) {
    if (argc != 1 && !(argc == 3 && strcmp(argv[1], "--import") == 0)) {
        fprintf(stderr, "usage: device_uuid [--import VKFORMAT]\n");
        return 2;
    }
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
    if (argc == 3) {
        printf("%s\n", importable(device, (VkFormat)atoi(argv[2])) ? "yes" : "no");
        vkDestroyInstance(instance, nullptr);
        return 0;
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
