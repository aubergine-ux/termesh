// Single translation unit that compiles the header-only third-party
// libraries, so their code (and their warnings) stay out of ours.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"
