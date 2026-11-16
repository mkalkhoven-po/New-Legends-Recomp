// Release version, shown in the window title and the exe's version resource.
#pragma once
#define NL_VERSION_MAJOR 0
#define NL_VERSION_MINOR 9
#define NL_VERSION_PATCH 0
#define NL_STR2(x) #x
#define NL_STR(x) NL_STR2(x)
#define NL_VERSION_STRING NL_STR(NL_VERSION_MAJOR) "." NL_STR(NL_VERSION_MINOR) "." NL_STR(NL_VERSION_PATCH)
#define NL_PRODUCT_NAME "New Legends Recomp"
