// This file is part of the QuantumGate project. For copyright and
// licensing information refer to the license file(s) in the project root.

#include "stdafx.h"
#include "SourceReader.h"

#include <Common\Util.h>

namespace QuantumGate::AVExtender
{
	GUID GetCaptureGUID(const CaptureDevice::Type type)
	{
		switch (type)
		{
			case CaptureDevice::Type::Video:
				return MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID;
			case CaptureDevice::Type::Audio:
				return MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID;
			default:
				assert(false);
				break;
		}

		return GUID_NULL;
	}

	DWORD GetStreamIndex(const CaptureDevice::Type type)
	{
		switch (type)
		{
			case CaptureDevice::Type::Video:
				return static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
			case CaptureDevice::Type::Audio:
				return static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
			default:
				assert(false);
				break;
		}

		return 0;
	}

	SourceReader::SourceReader(const CaptureDevice::Type type, const GUID supported_format) noexcept :
		m_Type(type), m_SupportedFormat(supported_format), m_CaptureGUID(GetCaptureGUID(type)),
		m_StreamIndex(GetStreamIndex(type))
	{
		DiscardReturnValue(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
	}

	SourceReader::~SourceReader()
	{
		Close();

		CoUninitialize();
	}

	Result<CaptureDeviceVector> SourceReader::EnumCaptureDevices() const noexcept
	{
		switch (m_Type)
		{
			case CaptureDevice::Type::Video:
				return CaptureDevices::Enum(CaptureDevice::Type::Video);
			case CaptureDevice::Type::Audio:
				return CaptureDevices::Enum(CaptureDevice::Type::Audio);
			default:
				assert(false);
				break;
		}

		return AVResultCode::Failed;
	}

	Result<> SourceReader::Open(const CaptureDevice& device, SampleEventCallback&& event_callback) noexcept
	{
		IMFAttributes* attributes{ nullptr };

		// Create an attribute store to specify symbolic link
		auto hr = MFCreateAttributes(&attributes, 2);
		if (SUCCEEDED(hr))
		{
			// Release attributes when we exit this scope
			const auto sg = MakeScopeGuard([&]() noexcept { SafeRelease(&attributes); });

			// Set source type attribute
			hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, m_CaptureGUID);
			if (SUCCEEDED(hr))
			{
				hr = E_UNEXPECTED;

				switch (m_Type)
				{
					case CaptureDevice::Type::Video:
						// Set symbolic link attribute
						hr = attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, device.SymbolicLink);
						break;
					case CaptureDevice::Type::Audio:
						// Set endpoint ID attribute
						hr = attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID, device.EndpointID);
						break;
					default:
						assert(false);
						break;
				}

				if (SUCCEEDED(hr))
				{
					auto source_reader_data = m_SourceReaderData.WithUniqueLock();

					hr = MFCreateDeviceSource(attributes, &source_reader_data->Source);
					if (SUCCEEDED(hr))
					{
						auto result = CreateSourceReader(*source_reader_data);
						if (result.Failed())
						{
							source_reader_data->Release();
						}
						else
						{
							source_reader_data->SampleEvent = std::move(event_callback);
						}

						return result;
					}
					else
					{
						if (m_Type == CaptureDevice::Type::Audio) return AVResultCode::FailedCreateAudioDeviceSource;
						else if (m_Type == CaptureDevice::Type::Video) return AVResultCode::FailedCreateVideoDeviceSource;
					}
				}
			}
		}

		return AVResultCode::Failed;
	}

	bool SourceReader::IsOpen() noexcept
	{
		auto source_reader_data = m_SourceReaderData.WithSharedLock();
		return (source_reader_data->SourceReader != nullptr);
	}

	void SourceReader::Close() noexcept
	{
		m_SourceReaderData.WithUniqueLock()->Release();
	}

	Result<> SourceReader::CreateSourceReader(SourceReaderData& source_reader_data) noexcept
	{
		IMFAttributes* attributes{ nullptr };

		// Allocate attributes
		auto hr = MFCreateAttributes(&attributes, 2);
		if (SUCCEEDED(hr))
		{
			// Release attributes when we exit this scope
			const auto sg = MakeScopeGuard([&]() noexcept { SafeRelease(&attributes); });

			hr = attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
			if (SUCCEEDED(hr))
			{
				// Set the callback pointer
				hr = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, this);
				if (SUCCEEDED(hr))
				{
					// Create the source reader
					hr = MFCreateSourceReaderFromMediaSource(source_reader_data.Source, attributes,
															 &source_reader_data.SourceReader);
					if (SUCCEEDED(hr))
					{
						auto result = GetSupportedMediaType(source_reader_data.SourceReader);
						if (result.Succeeded())
						{
							auto& media_type = result->first;
							auto& subtype = result->second;

							// Release media type when we exit this scope
							const auto sg2 = MakeScopeGuard([&]() noexcept { SafeRelease(&result->first); });

							hr = source_reader_data.SourceReader->SetCurrentMediaType(m_StreamIndex,
																					  nullptr, media_type);
							if (SUCCEEDED(hr))
							{
								source_reader_data.Format = subtype;

								if (OnMediaTypeChanged(media_type).Succeeded())
								{
									const auto result2 = CreateReaderBuffer(source_reader_data, media_type);
									if (result2.Succeeded())
									{
										// Ask for the first sample
										hr = source_reader_data.SourceReader->ReadSample(m_StreamIndex,
																						 0, nullptr, nullptr, nullptr, nullptr);

										if (SUCCEEDED(hr)) return AVResultCode::Succeeded;
									}
									else return result.GetErrorCode();
								}
							}
						}
						else return result.GetErrorCode();
					}
				}
			}
		}

		return AVResultCode::Failed;
	}

	Result<> SourceReader::CreateReaderBuffer(SourceReaderData& source_reader_data, IMFMediaType* media_type) noexcept
	{
		const auto result = GetBufferSize(media_type);
		if (result.Succeeded())
		{
			try
			{
				auto result2 = CaptureDevices::CreateMediaSample(*result);
				if (result2.Succeeded())
				{
					source_reader_data.Sample = result2->first;
					source_reader_data.Buffer = result2->second;

					return AVResultCode::Succeeded;
				}
			}
			catch (...)
			{
				return AVResultCode::FailedOutOfMemory;
			}
		}

		return result.GetErrorCode();
	}

	Result<std::pair<IMFMediaType*, GUID>> SourceReader::GetSupportedMediaType(IMFSourceReader* source_reader) noexcept
	{
		assert(source_reader != nullptr);

		// Try to find a suitable output type
		for (DWORD i = 0; ; ++i)
		{
			IMFMediaType* media_type{ nullptr };

			auto hr = source_reader->GetNativeMediaType(m_StreamIndex, i, &media_type);
			if (SUCCEEDED(hr))
			{
				// Release media type when we exit this scope
				auto sg = MakeScopeGuard([&]() noexcept { SafeRelease(&media_type); });

				GUID subtype{ GUID_NULL };

				hr = media_type->GetGUID(MF_MT_SUBTYPE, &subtype);
				if (SUCCEEDED(hr))
				{
					if (subtype == m_SupportedFormat)
					{
						// We'll return the media type so
						// the caller should release it
						sg.Deactivate();

						return std::make_pair(media_type, subtype);
					}
				}
			}
			else break;
		}

		switch (m_Type)
		{
			case CaptureDevice::Type::Video:
				return AVResultCode::FailedNoSupportedVideoMediaType;
			case CaptureDevice::Type::Audio:
				return AVResultCode::FailedNoSupportedAudioMediaType;
			default:
				assert(false);
				break;
		}

		return AVResultCode::Failed;
	}

	HRESULT SourceReader::OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex, DWORD dwStreamFlags,
									   LONGLONG llTimestamp, IMFSample* pSample)
	{
		HRESULT hr{ S_OK };

		auto source_reader_data = m_SourceReaderData.WithUniqueLock();

		if (FAILED(hrStatus)) hr = hrStatus;

		if (SUCCEEDED(hr))
		{
			if (pSample)
			{
				source_reader_data->SampleEvent(llTimestamp, pSample);
			}
		}

		// Request the next sample
		if (SUCCEEDED(hr))
		{
			hr = source_reader_data->SourceReader->ReadSample(m_StreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
		}

		return hr;
	}

	Result<> SourceReader::OnMediaTypeChanged(IMFMediaType* media_type) noexcept
	{
		return AVResultCode::Failed;
	}

	Result<Size> SourceReader::GetBufferSize(IMFMediaType* media_type) noexcept
	{
		return AVResultCode::Failed;
	}
}