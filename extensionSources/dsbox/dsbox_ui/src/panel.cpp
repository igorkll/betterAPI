// dsbox_ui is built as a shared library (see CMakeLists.txt's
// DSBOX_UI_EXPORTING define): a DSBOX_UI_API class only gets its
// member functions actually compiled into (and exported from) the DLL
// if SOME translation unit inside this project's own build includes
// its header -- even for members defined inline in the class body,
// the way PanelRegistry's and AtomicPanelIdAllocator's are (compare
// container.hpp's own trivial inline setters, which work only because
// container.cpp includes container.hpp). panel.hpp had no such
// translation unit, which is why dsbox_adb_tool's link against
// DSBox::Ui failed with LNK2019 for every PanelRegistry/
// AtomicPanelIdAllocator member despite the header compiling fine on
// its own. This file's only job is to be that translation unit.
#include <dsbox_ui/panel.hpp>
