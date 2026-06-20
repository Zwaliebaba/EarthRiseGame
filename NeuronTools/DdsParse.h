#pragma once

// Pure DDS header parser (no D3D12 / Windows dependency).
//
// Platform-independent core shared by the NeuronRender DdsLoader
// (masterplan §12.1, docs/design/neuronrender-architecture.md §9) and the
// `ddscheck` validator. The Windows loader uses the returned `DxgiFormat`
// (whose enumerators carry the real DXGI_FORMAT numeric values, so the loader
// can cast directly) plus the mip/dimension info to build a TextureGpu.
//
// Supports the two families M2 needs: BC1–BC7 block-compressed textures
// (ship/material art, §12) and uncompressed 32-bit BGRA (the Darwinia
// InterfaceGrey/InterfaceRed chrome + EditorFont atlas, docs/design/
// darwinia-menu-ui.md §2.1 — `B8G8R8A8_UNORM`, mip count 1).

#include <cstddef>
#include <cstdint>
#include <span>

namespace er::format
{
  // Subset of DXGI_FORMAT; enumerator values match the Windows DXGI numbers so
  // a Windows loader can `static_cast<DXGI_FORMAT>(fmt)` without a lookup table.
  enum class DxgiFormat : uint32_t
  {
    Unknown = 0,
    R8G8B8A8_UNORM = 28,
    BC1_UNORM = 71, // DXT1
    BC2_UNORM = 74, // DXT3
    BC3_UNORM = 77, // DXT5
    BC4_UNORM = 80, // ATI1 / BC4U
    BC5_UNORM = 83, // ATI2 / BC5U
    B8G8R8A8_UNORM = 87,
    BC6H_UF16 = 95,
    BC7_UNORM = 98
  };

  enum class DdsStatus : uint8_t
  {
    Ok = 0,
    NotDds,            // missing 'DDS ' magic
    Truncated,         // header (or DXT10 ext) runs past the buffer
    UnsupportedFormat, // pixel format we don't decode
    BadDimensions      // zero width/height
  };

  struct DdsImage
  {
    DxgiFormat format = DxgiFormat::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 1;
    bool blockCompressed = false; // true ⇒ BC1–BC7 (4×4 block layout)
    size_t dataOffset = 0;        // byte offset of mip 0 within the file
  };

  namespace detail
  {
    inline uint32_t ddsU32(const std::byte* p) noexcept
    {
      return std::to_integer<uint32_t>(p[0]) |
             (std::to_integer<uint32_t>(p[1]) << 8) |
             (std::to_integer<uint32_t>(p[2]) << 16) |
             (std::to_integer<uint32_t>(p[3]) << 24);
    }

    inline constexpr uint32_t makeFourCC(char a, char b, char c, char d) noexcept
    {
      return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
             (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
    }

    // DDS_PIXELFORMAT flags.
    inline constexpr uint32_t DDPF_FOURCC = 0x4;
    inline constexpr uint32_t DDPF_RGB = 0x40;
  } // namespace detail

  // Header sizes: 4-byte magic + 124-byte DDS_HEADER, then an optional 20-byte
  // DDS_HEADER_DXT10 when the pixel format fourCC is 'DX10'.
  inline constexpr size_t DDS_MAGIC_SIZE = 4;
  inline constexpr size_t DDS_HEADER_SIZE = 124;
  inline constexpr size_t DDS_DXT10_SIZE = 20;

  // Parse the DDS magic + header (+ optional DXT10 extension). Does not require
  // the full pixel payload to be present, but reports its offset so callers can
  // bounds-check before uploading.
  inline DdsStatus parseDds(std::span<const std::byte> file, DdsImage& out) noexcept
  {
    using namespace detail;

    if (file.size() < DDS_MAGIC_SIZE + DDS_HEADER_SIZE)
      return file.size() >= DDS_MAGIC_SIZE && ddsU32(file.data()) == makeFourCC('D', 'D', 'S', ' ')
                 ? DdsStatus::Truncated
                 : DdsStatus::NotDds;

    if (ddsU32(file.data()) != makeFourCC('D', 'D', 'S', ' '))
      return DdsStatus::NotDds;

    // DDS_HEADER fields (offsets relative to the start of the 124-byte header,
    // which begins right after the 4-byte magic):
    //   dwSize 0, dwFlags 4, dwHeight 8, dwWidth 12, dwPitch 16, dwDepth 20,
    //   dwMipMapCount 24, dwReserved1[11] 28..72, DDS_PIXELFORMAT 72..104.
    const std::byte* h = file.data() + DDS_MAGIC_SIZE;
    const uint32_t height = ddsU32(h + 8);
    const uint32_t width = ddsU32(h + 12);
    const uint32_t mipCount = ddsU32(h + 24);

    // DDS_PIXELFORMAT begins at header offset 72 (dwSize at 72, dwFlags at 76).
    const std::byte* pf = h + 72;
    const uint32_t pfFlags = ddsU32(pf + 4);
    const uint32_t fourCC = ddsU32(pf + 8);
    const uint32_t rgbBitCount = ddsU32(pf + 12);
    const uint32_t rMask = ddsU32(pf + 16);
    const uint32_t gMask = ddsU32(pf + 20);
    const uint32_t bMask = ddsU32(pf + 24);
    const uint32_t aMask = ddsU32(pf + 28);

    if (width == 0 || height == 0)
      return DdsStatus::BadDimensions;

    out.width = width;
    out.height = height;
    out.mipCount = mipCount == 0 ? 1 : mipCount;
    size_t dataOffset = DDS_MAGIC_SIZE + DDS_HEADER_SIZE;

    if (pfFlags & DDPF_FOURCC)
    {
      if (fourCC == makeFourCC('D', 'X', '1', '0'))
      {
        // DXT10 extension carries an explicit DXGI format.
        if (file.size() < dataOffset + DDS_DXT10_SIZE)
          return DdsStatus::Truncated;
        const uint32_t dxgi = ddsU32(file.data() + dataOffset);
        dataOffset += DDS_DXT10_SIZE;
        switch (static_cast<DxgiFormat>(dxgi))
        {
        case DxgiFormat::BC1_UNORM:
        case DxgiFormat::BC2_UNORM:
        case DxgiFormat::BC3_UNORM:
        case DxgiFormat::BC4_UNORM:
        case DxgiFormat::BC5_UNORM:
        case DxgiFormat::BC6H_UF16:
        case DxgiFormat::BC7_UNORM:
          out.format = static_cast<DxgiFormat>(dxgi);
          out.blockCompressed = true;
          break;
        case DxgiFormat::B8G8R8A8_UNORM:
        case DxgiFormat::R8G8B8A8_UNORM:
          out.format = static_cast<DxgiFormat>(dxgi);
          out.blockCompressed = false;
          break;
        default:
          return DdsStatus::UnsupportedFormat;
        }
      }
      else if (fourCC == makeFourCC('D', 'X', 'T', '1'))
      {
        out.format = DxgiFormat::BC1_UNORM;
        out.blockCompressed = true;
      }
      else if (fourCC == makeFourCC('D', 'X', 'T', '3'))
      {
        out.format = DxgiFormat::BC2_UNORM;
        out.blockCompressed = true;
      }
      else if (fourCC == makeFourCC('D', 'X', 'T', '5'))
      {
        out.format = DxgiFormat::BC3_UNORM;
        out.blockCompressed = true;
      }
      else if (fourCC == makeFourCC('A', 'T', 'I', '1') || fourCC == makeFourCC('B', 'C', '4', 'U'))
      {
        out.format = DxgiFormat::BC4_UNORM;
        out.blockCompressed = true;
      }
      else if (fourCC == makeFourCC('A', 'T', 'I', '2') || fourCC == makeFourCC('B', 'C', '5', 'U'))
      {
        out.format = DxgiFormat::BC5_UNORM;
        out.blockCompressed = true;
      }
      else
      {
        return DdsStatus::UnsupportedFormat;
      }
    }
    else if (pfFlags & DDPF_RGB)
    {
      // Uncompressed: identify the 32-bit BGRA chrome/font sheet used by Canvas.
      // (B8G8R8A8: R=0x00ff0000 G=0x0000ff00 B=0x000000ff A=0xff000000.)
      if (rgbBitCount == 32 && rMask == 0x00ff0000 && gMask == 0x0000ff00 &&
          bMask == 0x000000ff && aMask == 0xff000000)
      {
        out.format = DxgiFormat::B8G8R8A8_UNORM;
      }
      else if (rgbBitCount == 32 && rMask == 0x000000ff && gMask == 0x0000ff00 &&
               bMask == 0x00ff0000 && aMask == 0xff000000)
      {
        out.format = DxgiFormat::R8G8B8A8_UNORM;
      }
      else
      {
        return DdsStatus::UnsupportedFormat;
      }
      out.blockCompressed = false;
    }
    else
    {
      return DdsStatus::UnsupportedFormat;
    }

    out.dataOffset = dataOffset;
    return DdsStatus::Ok;
  }
} // namespace er::format
