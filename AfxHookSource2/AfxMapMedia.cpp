// Publish FFmpeg's bottom-up BGRA BMP stream as one local DirectX Spout sender.
#include <SpoutDX.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2 || !argv[1][0] || strlen(argv[1]) >= 256) return 1;
    _setmode(_fileno(stdin), _O_BINARY);
    spoutDX sender;
    sender.SetSenderName(argv[1]);
    sender.SetSenderFormat(DXGI_FORMAT_B8G8R8A8_UNORM);
    std::vector<unsigned char> pixels;
    BITMAPFILEHEADER file;
    BITMAPINFOHEADER info;
    while (fread(&file, 1, sizeof(file), stdin) == sizeof(file)) {
        if (fread(&info, 1, sizeof(info), stdin) != sizeof(info)
            || file.bfType != 0x4d42 || file.bfOffBits != sizeof(file) + sizeof(info)
            || info.biSize != sizeof(info) || info.biPlanes != 1 || info.biBitCount != 32
            || info.biCompression != BI_RGB || info.biWidth < 1 || info.biWidth > 1920
            || info.biHeight < 1 || info.biHeight > 1080) return 2;
        pixels.resize(static_cast<size_t>(info.biWidth) * info.biHeight * 4);
        if (file.bfSize != file.bfOffBits + pixels.size()
            || fread(pixels.data(), 1, pixels.size(), stdin) != pixels.size()) return 2;
        // FFmpeg's vflip cancels BMP's bottom-up scan order before the GPU upload.
        if (!sender.SendImage(pixels.data(), info.biWidth, info.biHeight)) return 3;
    }
    return ferror(stdin) ? 2 : 0;
}
