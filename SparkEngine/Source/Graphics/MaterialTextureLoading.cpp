#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file MaterialTextureLoading.cpp
 * @brief WIC-based texture loading for the MaterialSystem (Windows only)
 *
 * Extracted from MaterialSystem.cpp to keep texture I/O separate from
 * material lifecycle and binding logic.
 */

#include "MaterialSystem.h"
#include "../Utils/SparkConsole.h"

#include <filesystem>
#include <string>
#include <vector>

#include <wincodec.h>
#include <wincodecsdk.h>

ComPtr<ID3D11ShaderResourceView> MaterialSystem::LoadTextureFromFile(const std::string& filePath)
{
    ComPtr<ID3D11ShaderResourceView> texture;

    if (!m_device || filePath.empty())
    {
        Spark::SimpleConsole::GetInstance().LogError("Invalid device or empty file path in LoadTextureFromFile");
        return texture;
    }

    if (!std::filesystem::exists(filePath))
    {
        Spark::SimpleConsole::GetInstance().LogError("Texture file not found: " + filePath);
        return texture;
    }

    try
    {
        // Initialize WIC factory
        ComPtr<IWICImagingFactory> wicFactory;
        HRESULT hr =
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC Imaging Factory for: " + filePath);
            return texture;
        }

        // Create decoder
        ComPtr<IWICBitmapDecoder> decoder;
        std::wstring wideFilePath(filePath.begin(), filePath.end());
        hr = wicFactory->CreateDecoderFromFilename(wideFilePath.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC decoder for: " + filePath);
            return texture;
        }

        // Get first frame
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to get frame from decoder for: " + filePath);
            return texture;
        }

        // Get original size for mipmap calculation
        UINT originalWidth, originalHeight;
        hr = frame->GetSize(&originalWidth, &originalHeight);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to get frame size for: " + filePath);
            return texture;
        }

        // Calculate mip levels (power of 2 textures get full mip chain)
        UINT mipLevels = 1;
        if ((originalWidth & (originalWidth - 1)) == 0 && (originalHeight & (originalHeight - 1)) == 0)
        {
            UINT maxDimension = std::max(originalWidth, originalHeight);
            while (maxDimension > 1)
            {
                maxDimension >>= 1;
                mipLevels++;
            }
        }

        // Create format converter
        ComPtr<IWICFormatConverter> converter;
        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create format converter for: " + filePath);
            return texture;
        }

        // Convert to RGBA format
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to initialize format converter for: " + filePath);
            return texture;
        }

        // Create texture description
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = originalWidth;
        texDesc.Height = originalHeight;
        texDesc.MipLevels = mipLevels;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        texDesc.CPUAccessFlags = 0;
        texDesc.MiscFlags = (mipLevels > 1) ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

        // Prepare initial data for base mip level
        std::vector<BYTE> imageData(originalWidth * originalHeight * 4);
        hr = converter->CopyPixels(nullptr, originalWidth * 4, static_cast<UINT>(imageData.size()), imageData.data());
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to copy pixels for: " + filePath);
            return texture;
        }

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = imageData.data();
        initData.SysMemPitch = originalWidth * 4;
        initData.SysMemSlicePitch = 0;

        // Create texture
        ComPtr<ID3D11Texture2D> tex2D;
        hr = m_device->CreateTexture2D(&texDesc, &initData, &tex2D);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create Direct3D texture for: " + filePath);
            return texture;
        }

        // Create shader resource view
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = mipLevels;

        hr = m_device->CreateShaderResourceView(tex2D.Get(), &srvDesc, &texture);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create shader resource view for: " + filePath);
            return texture;
        }

        // Generate mipmaps if enabled
        if (mipLevels > 1 && m_context)
        {
            m_context->GenerateMips(texture.Get());
        }

        Spark::SimpleConsole::GetInstance().LogInfo(
            "Successfully loaded texture: " + filePath + " (" + std::to_string(originalWidth) + "x" +
            std::to_string(originalHeight) + ", " + std::to_string(mipLevels) + " mips)");
    }
    catch (const std::exception& e)
    {
        Spark::SimpleConsole::GetInstance().LogError("Exception loading texture " + filePath + ": " +
                                                     std::string(e.what()));
        texture.Reset();
    }

    return texture;
}

#endif // SPARK_PLATFORM_WINDOWS
