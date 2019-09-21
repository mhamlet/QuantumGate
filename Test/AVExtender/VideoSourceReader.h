// This file is part of the QuantumGate project. For copyright and
// licensing information refer to the license file(s) in the project root.

#pragma once

#include "SourceReader.h"
#include <Concurrency\ThreadSafe.h>

namespace QuantumGate::AVExtender
{
	class VideoSourceReader : public SourceReader
	{
		struct SourceReaderData
		{
			IMFMediaSource* Source{ nullptr };
			IMFSourceReader* SourceReader{ nullptr };
			GUID VideoFormat{ GUID_NULL };
			UInt Width{ 0 };
			UInt Height{ 0 };
			UInt BytesPerPixel{ 0 };
			Long Stride{ 0 };
			QuantumGate::Buffer RawData;

			void Release() noexcept
			{
				SafeRelease(&SourceReader);

				if (Source)
				{
					Source->Shutdown();
				}
				SafeRelease(&Source);

				VideoFormat = GUID_NULL;
				Width = 0;
				Height = 0;
				BytesPerPixel = 0;
				Stride = 0;
				RawData.Clear();
				RawData.FreeUnused();
			}
		};

		using SourceReaderData_ThS = Concurrency::ThreadSafe<SourceReaderData, std::shared_mutex>;

	public:
		VideoSourceReader() noexcept;
		VideoSourceReader(const VideoSourceReader&) = delete;
		VideoSourceReader(VideoSourceReader&&) = delete;
		virtual ~VideoSourceReader();
		VideoSourceReader& operator=(const VideoSourceReader&) = delete;
		VideoSourceReader& operator=(VideoSourceReader&&) = delete;

		[[nodiscard]] Result<> Open(const CaptureDevice& device) noexcept;
		[[nodiscard]] bool IsOpen() noexcept;
		void Close() noexcept;

		void GetSample(BGRAPixel* buffer) noexcept;
		[[nodiscard]] std::pair<UInt, UInt> GetSampleDimensions() noexcept;

		// Methods from IUnknown 
		STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override;
		STDMETHODIMP_(ULONG) AddRef() override;
		STDMETHODIMP_(ULONG) Release() override;

		// Methods from IMFSourceReaderCallback 
		STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex, DWORD dwStreamFlags,
								  LONGLONG llTimestamp, IMFSample* pSample) override;
		STDMETHODIMP OnEvent(DWORD dwStreamIndex, IMFMediaEvent* pEvent) override { return S_OK; }
		STDMETHODIMP OnFlush(DWORD dwStreamIndex) override { return S_OK; }

	private:
		[[nodiscard]] Result<> CreateSourceReader(SourceReaderData& source_reader_data) noexcept;

		[[nodiscard]] Result<std::pair<IMFMediaType*, GUID>> GetSupportedMediaType(IMFSourceReader* source_reader) noexcept;
		[[nodiscard]] Result<> SetMediaType(SourceReaderData& source_reader_data,
											IMFMediaType* media_type, const GUID& subtype) noexcept;

		[[nodiscard]] bool GetDefaultStride(IMFMediaType* type, LONG* stride) const noexcept;

	private:
		long m_RefCount{ 1 };
		SourceReaderData_ThS m_SourceReader;
	};
}