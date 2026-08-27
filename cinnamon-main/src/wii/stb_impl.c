#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
// The Wii only ever decodes the PNG texture pages out of data.win, so every other
// decoder is compiled out. This matters: each one costs MEM1 we do not have.
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"
