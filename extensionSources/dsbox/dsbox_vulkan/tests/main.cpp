#include <cstdio>
#include <cstdlib>

// Deliberately NOT forcing SDL's "dummy" video driver here (unlike
// dsbox_sdl's own tests): a dummy driver has no real windowing surface,
// so it cannot support Vulkan surface creation at all. This suite needs
// a real display and a real Vulkan-capable device to exercise the
// window-attach/swapchain/acquire/present path -- tests that need those
// check for their absence and print [SKIP] rather than fail, so this
// binary still runs (and still passes) on a headless machine or one
// with no GPU driver installed; it just skips what it structurally
// can't test there.

void runVulkanModuleTests();

namespace dsbox_vulkan_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox Vulkan tests\n\n");

    runVulkanModuleTests();

    std::printf("\n");

    if (dsbox_vulkan_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_vulkan_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed (see [SKIP] notes above for anything this "
                "environment couldn't exercise).\n");
    return EXIT_SUCCESS;
}
