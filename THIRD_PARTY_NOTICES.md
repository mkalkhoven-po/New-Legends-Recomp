# Third-party notices

`newlegends.exe` statically links the GLSL compiler from the LunarG Vulkan SDK
(version 1.4.363.0), which it uses to build shaders while the game runs. The full
license texts are in the `licenses` folder.

| Component | Use | License | Text |
|---|---|---|---|
| [shaderc](https://github.com/google/shaderc) | GLSL to SPIR-V compiler interface | Apache License 2.0 | `licenses/shaderc.txt` |
| [glslang](https://github.com/KhronosGroup/glslang) | GLSL front end | BSD-3-Clause, BSD-2-Clause, MIT and Apache 2.0; the generated grammar parser is GPL 3 with the Bison exception, which allows distribution under other terms | `licenses/glslang.txt` |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | SPIR-V optimizer | Apache License 2.0 | `licenses/SPIRV-Tools.txt` |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) | SPIR-V definitions | MIT-style Khronos license | `licenses/SPIRV-Headers.txt` |

The Microsoft Visual C++ runtime DLLs in the release package (`msvcp140.dll`,
`vcruntime140.dll`, `vcruntime140_1.dll`) are redistributable files from Visual Studio 2022.

`vulkan-1.dll` is not shipped: it comes with the graphics driver.

New Legends and its data are not included and not covered by any license here. The game is
© its respective owners.
