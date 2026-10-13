// The stb_image (PNG decoding) and stb_image_write implementations, compiled
// once in their own target without SPG's warning flags. Pinned in
// cli/CMakeLists.txt by commit and SHA-256.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
