#include "font.h"
#include "gen_font.cpp"

namespace ImGui {

void UseCustomFont(ImGuiIO& io, float size)
{
    ImFont* font = io.Fonts->AddFontFromMemoryCompressedTTF(
        gen_font_compressed_data, gen_font_compressed_size, size);
    io.FontDefault = font;
}

}
