#include <cstdio>
#include <cstdlib>

void runWindowUiTreeTests();
void runStackLayoutTests();
void runContainerTests();
void runTextTests();
void runImageTests();
void runButtonTests();
void runTextFieldTests();
void runTextAreaTests();
void runListViewTests();
void runScrollViewTests();
void runDropdownTests();
void runProgressBarTests();

namespace dsbox_ui_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox UI tests\n\n");

    runWindowUiTreeTests();
    runStackLayoutTests();
    runContainerTests();
    runTextTests();
    runImageTests();
    runButtonTests();
    runTextFieldTests();
    runTextAreaTests();
    runListViewTests();
    runScrollViewTests();
    runDropdownTests();
    runProgressBarTests();

    std::printf("\n");

    if (dsbox_ui_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_ui_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
