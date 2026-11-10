#include "hop_windows/image_codec.h"
#include "hop_windows/clipboard.h"
#include "hop_windows/dib.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <limits>
#include <utility>

namespace hop::windows {
namespace {
template<class T> struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T** out() { return &value; }
    T* operator->() const { return value; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};
struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool ready() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};
bool factory(ComPtr<IWICImagingFactory>& out) {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_IWICImagingFactory, reinterpret_cast<void**>(out.out())));
}
bool memory_stream(ComPtr<IStream>& out) { return SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, out.out())); }
bool dimensions(UINT w, UINT h) { return w && h && w<=16384 && h<=16384 && std::uint64_t(w)*h<=max_png_pixels; }
}
bool png_to_dibv5(const std::vector<std::uint8_t>& png, std::vector<std::uint8_t>& dib) {
    if (!valid_png_bytes(png)) return false;
    Apartment apartment; if (!apartment.ready()) return false;
    ComPtr<IWICImagingFactory> wic; ComPtr<IStream> stream;
    ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
    if (!factory(wic) || !memory_stream(stream)) return false;
    ULONG written = 0;
    if (FAILED(stream->Write(png.data(), static_cast<ULONG>(png.size()), &written)) || written != png.size()) return false;
    LARGE_INTEGER begin{}; if (FAILED(stream->Seek(begin, STREAM_SEEK_SET, nullptr))) return false;
    if (FAILED(wic->CreateDecoderFromStream(stream.value, nullptr, WICDecodeMetadataCacheOnDemand, decoder.out()))) return false;
    UINT count=0,w=0,h=0;
    if (FAILED(decoder->GetFrameCount(&count)) || count!=1 || FAILED(decoder->GetFrame(0,frame.out())) ||
        FAILED(frame->GetSize(&w,&h)) || !dimensions(w,h)) return false;
    if (FAILED(wic->CreateFormatConverter(converter.out())) ||
        FAILED(converter->Initialize(frame.value, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    BgraImage result; result.width=w; result.height=h;
    try { result.pixels.resize(std::size_t(w)*h*4); } catch (...) { return false; }
    if (result.pixels.size()>std::numeric_limits<UINT>::max()) return false;
    if (FAILED(converter->CopyPixels(nullptr, w*4, static_cast<UINT>(result.pixels.size()), result.pixels.data()))) return false;
    return encode_dibv5(result, dib);
}
bool dib_to_png(const std::uint8_t* dib, std::size_t size, std::vector<std::uint8_t>& png) {
    BgraImage image;
    if (!decode_dib(dib,size,image)) return false;
    Apartment apartment; if (!apartment.ready()) return false;
    ComPtr<IWICImagingFactory> wic; ComPtr<IStream> stream; ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties; ComPtr<IWICFormatConverter> converter;
    if (!factory(wic) || !memory_stream(stream)) return false;
    if (FAILED(wic->CreateBitmapFromMemory(image.width,image.height,GUID_WICPixelFormat32bppBGRA,image.width*4,
                                           static_cast<UINT>(image.pixels.size()),image.pixels.data(),bitmap.out()))) return false;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.out())) ||
        FAILED(encoder->Initialize(stream.value,WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.out(),properties.out())) ||
        FAILED(frame->Initialize(properties.value)) || FAILED(frame->SetSize(image.width,image.height))) return false;
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    // Never accept a codec-selected opaque/premultiplied format: either could
    // silently change transparency while turning a DIB into a PNG.
    if (!IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA) && !IsEqualGUID(format,GUID_WICPixelFormat32bppRGBA)) return false;
    if (FAILED(wic->CreateFormatConverter(converter.out())) ||
        FAILED(converter->Initialize(bitmap.value,format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)) ||
        FAILED(frame->WriteSource(converter.value,nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
    STATSTG info{};
    if (FAILED(stream->Stat(&info,STATFLAG_NONAME)) || info.cbSize.HighPart || info.cbSize.LowPart>max_png_bytes) return false;
    std::vector<std::uint8_t> result;
    try { result.resize(info.cbSize.LowPart); } catch (...) { return false; }
    LARGE_INTEGER begin{}; ULONG got=0;
    if (FAILED(stream->Seek(begin,STREAM_SEEK_SET,nullptr)) ||
        FAILED(stream->Read(result.data(),static_cast<ULONG>(result.size()),&got)) || got!=result.size() ||
        !valid_png_bytes(result)) return false;
    png=std::move(result); return true;
}
} // namespace hop::windows
