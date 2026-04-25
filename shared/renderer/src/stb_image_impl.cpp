// The single translation unit that provides the stb_image symbols for the
// entire project. Every exe that links rco_renderer (client, GUE, terrain
// editor, future tools) gets stbi_* from here — do NOT define
// STB_IMAGE_IMPLEMENTATION anywhere else.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
