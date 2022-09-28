// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#undef WINRT_LEAN_AND_MEAN

#include "WinRTHttpResource.h"

#include <CppRuntimeOptions.h>
#include <ReactPropertyBag.h>
#include <Utils/CppWinrtLessExceptions.h>
#include <Utils/WinRTConversions.h>
#include <utilities.h>
#include "IRedirectEventSource.h"
#include "OriginPolicyHttpFilter.h"
#include "RedirectHttpFilter.h"

// Boost Libraries
#include <boost/algorithm/string.hpp>

// Windows API
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>

using folly::dynamic;

using std::function;
using std::scoped_lock;
using std::shared_ptr;
using std::string;
using std::vector;
using std::weak_ptr;

using winrt::fire_and_forget;
using winrt::hresult_error;
using winrt::to_hstring;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Security::Cryptography::CryptographicBuffer;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::Streams::DataReader;
using winrt::Windows::Storage::Streams::UnicodeEncoding;
using winrt::Windows::Web::Http::HttpBufferContent;
using winrt::Windows::Web::Http::HttpMethod;
using winrt::Windows::Web::Http::HttpRequestMessage;
using winrt::Windows::Web::Http::HttpStreamContent;
using winrt::Windows::Web::Http::HttpStringContent;
using winrt::Windows::Web::Http::IHttpClient;
using winrt::Windows::Web::Http::IHttpContent;
using winrt::Windows::Web::Http::Headers::HttpMediaTypeHeaderValue;

namespace Microsoft::React::Networking {

#pragma region WinRTHttpResource

WinRTHttpResource::WinRTHttpResource(IHttpClient &&client) noexcept : m_client{std::move(client)} {}

WinRTHttpResource::WinRTHttpResource() noexcept : WinRTHttpResource(winrt::Windows::Web::Http::HttpClient{}) {}

#pragma region IWinRTHttpRequestFactory

IAsyncOperation<HttpRequestMessage> WinRTHttpResource::CreateRequest(
    HttpMethod &&method,
    Uri &&uri,
    winrt::Windows::Foundation::Collections::IMap<winrt::hstring, IInspectable> props) noexcept /*override*/ {
  auto request = HttpRequestMessage{std::move(method), std::move(uri)};
  for (auto prop : props) {
    request.Properties().Insert(prop.Key(), prop.Value());
  }

  auto iReqArgs = request.Properties().Lookup(L"RequestArgs");
  auto reqArgs = iReqArgs.as<RequestArgs>();
  auto self = shared_from_this();

  HttpMediaTypeHeaderValue contentType{nullptr};
  string contentEncoding;
  string contentLength;

  // Headers are generally case-insensitive
  // https://www.ietf.org/rfc/rfc2616.txt section 4.2
  for (auto &header : reqArgs->Headers) {
    if (boost::iequals(header.first.c_str(), "Content-Type")) {
      bool success = HttpMediaTypeHeaderValue::TryParse(to_hstring(header.second), contentType);
      if (!success) {
        if (self->m_onError) {
          self->m_onError(reqArgs->RequestId, "Failed to parse Content-Type", false);
        }
        co_return nullptr;
      }
    } else if (boost::iequals(header.first.c_str(), "Content-Encoding")) {
      contentEncoding = header.second;
    } else if (boost::iequals(header.first.c_str(), "Content-Length")) {
      contentLength = header.second;
    } else if (boost::iequals(header.first.c_str(), "Authorization")) {
      bool success = request.Headers().TryAppendWithoutValidation(to_hstring(header.first), to_hstring(header.second));
      if (!success) {
        if (self->m_onError) {
          self->m_onError(reqArgs->RequestId, "Failed to append Authorization", false);
        }
        co_return nullptr;
      }
    } else {
      try {
        request.Headers().Append(to_hstring(header.first), to_hstring(header.second));
      } catch (hresult_error const &e) {
        if (self->m_onError) {
          self->m_onError(reqArgs->RequestId, Utilities::HResultToString(e), false);
        }
        co_return nullptr;
      }
    }
  }

  // Initialize content
  IHttpContent content{nullptr};
  auto &data = reqArgs->Data;
  if (!data.isNull()) {
    auto bodyHandler = self->m_requestBodyHandler.lock();
    if (bodyHandler && bodyHandler->Supports(data)) {
      auto contentTypeString = contentType ? winrt::to_string(contentType.ToString()) : "";
      dynamic blob;
      try {
        blob = bodyHandler->ToRequestBody(data, contentTypeString);
      } catch (const std::invalid_argument &e) {
        if (self->m_onError) {
          self->m_onError(reqArgs->RequestId, e.what(), false);
        }
        co_return nullptr;
      }
      auto bytes = blob["bytes"];
      auto byteVector = vector<uint8_t>(bytes.size());
      for (auto &byte : bytes) {
        byteVector.push_back(static_cast<uint8_t>(byte.asInt()));
      }
      auto view = winrt::array_view<uint8_t const>{byteVector};
      auto buffer = CryptographicBuffer::CreateFromByteArray(view);
      content = HttpBufferContent{std::move(buffer)};
    } else if (!data["string"].isNull()) {
      content = HttpStringContent{to_hstring(data["string"].asString())};
    } else if (!data["base64"].empty()) {
      auto buffer = CryptographicBuffer::DecodeFromBase64String(to_hstring(data["base64"].asString()));
      content = HttpBufferContent{std::move(buffer)};
    } else if (!data["uri"].empty()) {
      auto file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{to_hstring(data["uri"].asString())});
      auto stream = co_await file.OpenReadAsync();
      content = HttpStreamContent{std::move(stream)};
    } else if (!data["form"].empty()) {
      // #9535 - HTTP form data support
      // winrt::Windows::Web::Http::HttpMultipartFormDataContent()
    }
  }

  // Attach content headers
  if (content != nullptr) {
    if (contentType) {
      content.Headers().ContentType(contentType);
    }
    if (!contentEncoding.empty()) {
      if (!content.Headers().ContentEncoding().TryParseAdd(to_hstring(contentEncoding))) {
        if (self->m_onError)
          self->m_onError(reqArgs->RequestId, "Failed to parse Content-Encoding", false);

        co_return nullptr;
      }
    }

    if (!contentLength.empty()) {
      try {
        const auto contentLengthHeader = std::stol(contentLength);
        content.Headers().ContentLength(contentLengthHeader);
      } catch (const std::invalid_argument &e) {
        if (self->m_onError)
          self->m_onError(reqArgs->RequestId, e.what() + string{" ["} + contentLength + "]", false);

        co_return nullptr;
      } catch (const std::out_of_range &e) {
        if (self->m_onError)
          self->m_onError(reqArgs->RequestId, e.what() + string{" ["} + contentLength + "]", false);

        co_return nullptr;
      }
    }

    request.Content(content);
  }

  co_return request;
}

#pragma endregion IWinRTHttpRequestFactory

#pragma region IHttpResource

void WinRTHttpResource::SendRequest(
    string &&method,
    string &&url,
    int64_t requestId,
    Headers &&headers,
    dynamic &&data,
    string &&responseType,
    bool useIncrementalUpdates,
    int64_t timeout,
    bool withCredentials,
    std::function<void(int64_t)> &&callback) noexcept /*override*/ {
  // Enforce supported args
  assert(responseType == "text" || responseType == "base64" || responseType == "blob");

  if (callback) {
    callback(requestId);
  }

  try {
    HttpMethod httpMethod{to_hstring(std::move(method))};
    Uri uri{to_hstring(std::move(url))};

    auto iReqArgs = winrt::make<RequestArgs>();
    auto reqArgs = iReqArgs.as<RequestArgs>();
    reqArgs->RequestId = requestId;
    reqArgs->Headers = std::move(headers);
    reqArgs->Data = std::move(data);
    reqArgs->IncrementalUpdates = useIncrementalUpdates;
    reqArgs->WithCredentials = withCredentials;
    reqArgs->ResponseType = std::move(responseType);
    reqArgs->Timeout = timeout;

    PerformSendRequest(std::move(httpMethod), std::move(uri), iReqArgs);
  } catch (std::exception const &e) {
    if (m_onError) {
      m_onError(requestId, e.what(), false);
    }
  } catch (hresult_error const &e) {
    if (m_onError) {
      m_onError(requestId, Utilities::HResultToString(e), false);
    }
  } catch (...) {
    m_onError(requestId, "Unidentified error sending HTTP request", false);
  }
}

void WinRTHttpResource::AbortRequest(int64_t requestId) noexcept /*override*/ {
  ResponseOperation request{nullptr};

  {
    scoped_lock lock{m_mutex};
    auto iter = m_responses.find(requestId);
    if (iter == std::end(m_responses)) {
      return;
    }
    request = iter->second;
  }

  try {
    request.Cancel();
  } catch (hresult_error const &e) {
    m_onError(requestId, Utilities::HResultToString(e), false);
  }
}

void WinRTHttpResource::ClearCookies() noexcept /*override*/ {
  assert(false);
  // NOT IMPLEMENTED
}

void WinRTHttpResource::SetOnRequestSuccess(function<void(int64_t requestId)> &&handler) noexcept /*override*/ {
  m_onRequestSuccess = std::move(handler);
}

void WinRTHttpResource::SetOnResponse(function<void(int64_t requestId, Response &&response)> &&handler) noexcept
/*override*/ {
  m_onResponse = std::move(handler);
}

void WinRTHttpResource::SetOnData(function<void(int64_t requestId, string &&responseData)> &&handler) noexcept
/*override*/ {
  m_onData = std::move(handler);
}

void WinRTHttpResource::SetOnData(function<void(int64_t requestId, dynamic &&responseData)> &&handler) noexcept
/*override*/
{
  m_onDataDynamic = std::move(handler);
}

void WinRTHttpResource::SetOnError(
    function<void(int64_t requestId, string &&errorMessage, bool isTimeout)> &&handler) noexcept
/*override*/ {
  m_onError = std::move(handler);
}

#pragma endregion IHttpResource

void WinRTHttpResource::TrackResponse(int64_t requestId, ResponseOperation response) noexcept {
  scoped_lock lock{m_mutex};
  m_responses[requestId] = response;
}

void WinRTHttpResource::UntrackResponse(int64_t requestId) noexcept {
  scoped_lock lock{m_mutex};
  m_responses.erase(requestId);
}

fire_and_forget
WinRTHttpResource::PerformSendRequest(HttpMethod &&method, Uri &&rtUri, IInspectable const &args) noexcept {
  // Keep references after coroutine suspension.
  auto self = shared_from_this();
  auto coArgs = args;
  auto reqArgs = coArgs.as<RequestArgs>();
  auto coMethod = std::move(method);
  auto coUri = std::move(rtUri);

  // Ensure background thread
  co_await winrt::resume_background();

  auto props = winrt::multi_threaded_map<winrt::hstring, IInspectable>();
  props.Insert(L"RequestArgs", coArgs);

  auto coRequest = co_await CreateRequest(std::move(coMethod), std::move(coUri), props);
  if (!coRequest) {
    co_return;
  }

  // If URI handler is available, it takes over request processing.
  if (auto uriHandler = self->m_uriHandler.lock()) {
    auto uri = winrt::to_string(coRequest.RequestUri().ToString());
    try {
      if (uriHandler->Supports(uri, reqArgs->ResponseType)) {
        auto blob = uriHandler->Fetch(uri);
        if (self->m_onDataDynamic && self->m_onRequestSuccess) {
          self->m_onDataDynamic(reqArgs->RequestId, std::move(blob));
          self->m_onRequestSuccess(reqArgs->RequestId);
        }

        co_return;
      }
    } catch (const hresult_error &e) {
      if (self->m_onError)
        co_return self->m_onError(reqArgs->RequestId, Utilities::HResultToString(e), false);
    } catch (const std::exception &e) {
      if (self->m_onError)
        co_return self->m_onError(reqArgs->RequestId, e.what(), false);
    }
  }

  try {
    auto sendRequestOp = self->m_client.SendRequestAsync(coRequest);
    self->TrackResponse(reqArgs->RequestId, sendRequestOp);

    if (reqArgs->Timeout > 0) {
      // See https://devblogs.microsoft.com/oldnewthing/20220415-00/?p=106486
      auto timedOut = std::make_shared<bool>(false);
      auto sendRequestTimeout = [](auto timedOut, auto milliseconds) -> ResponseOperation {
        // Convert milliseconds to "ticks" (10^-7 seconds)
        co_await winrt::resume_after(winrt::Windows::Foundation::TimeSpan{milliseconds * 10000});
        *timedOut = true;
        co_return nullptr;
      }(timedOut, reqArgs->Timeout);

      co_await lessthrow_await_adapter<ResponseOperation>{winrt::when_any(sendRequestOp, sendRequestTimeout)};

      // Cancel either still unfinished coroutine.
      sendRequestTimeout.Cancel();
      sendRequestOp.Cancel();

      if (*timedOut) {
        if (self->m_onError) {
          // TODO: Try to replace with either:
          //       WININET_E_TIMEOUT
          //       ERROR_INTERNET_TIMEOUT
          //       INET_E_CONNECTION_TIMEOUT
          self->m_onError(reqArgs->RequestId, Utilities::HResultToString(HRESULT_FROM_WIN32(ERROR_TIMEOUT)), true);
        }
        co_return self->UntrackResponse(reqArgs->RequestId);
      }
    } else {
      co_await lessthrow_await_adapter<ResponseOperation>{sendRequestOp};
    }

    auto result = sendRequestOp.ErrorCode();
    if (result < 0) {
      if (self->m_onError) {
        self->m_onError(reqArgs->RequestId, Utilities::HResultToString(std::move(result)), false);
      }
      co_return self->UntrackResponse(reqArgs->RequestId);
    }

    auto response = sendRequestOp.GetResults();
    if (response) {
      if (self->m_onResponse) {
        auto url = to_string(response.RequestMessage().RequestUri().AbsoluteUri());

        // Gather headers for both the response content and the response itself
        // See Invoke-WebRequest PowerShell cmdlet or Chromium response handling
        Headers responseHeaders;
        for (auto header : response.Headers()) {
          responseHeaders.emplace(to_string(header.Key()), to_string(header.Value()));
        }
        for (auto header : response.Content().Headers()) {
          responseHeaders.emplace(to_string(header.Key()), to_string(header.Value()));
        }

        self->m_onResponse(
            reqArgs->RequestId,
            {static_cast<int32_t>(response.StatusCode()), std::move(url), std::move(responseHeaders)});
      }
    }

    // #9534 - Support HTTP incremental updates
    if (response && response.Content()) {
      auto inputStream = co_await response.Content().ReadAsInputStreamAsync();
      auto reader = DataReader{inputStream};

      // #9510 - 10mb limit on fetch
      co_await reader.LoadAsync(10 * 1024 * 1024);

      // Let response handler take over, if set
      if (auto responseHandler = self->m_responseHandler.lock()) {
        if (responseHandler->Supports(reqArgs->ResponseType)) {
          auto bytes = vector<uint8_t>(reader.UnconsumedBufferLength());
          reader.ReadBytes(bytes);
          auto blob = responseHandler->ToResponseData(std::move(bytes));

          if (self->m_onDataDynamic && self->m_onRequestSuccess) {
            self->m_onDataDynamic(reqArgs->RequestId, std::move(blob));
            self->m_onRequestSuccess(reqArgs->RequestId);
          }

          co_return;
        }
      }

      auto isText = reqArgs->ResponseType == "text";
      if (isText) {
        reader.UnicodeEncoding(UnicodeEncoding::Utf8);
      }

      // #9510 - We currently accumulate all incoming request data in 10MB chunks.
      uint32_t segmentSize = 10 * 1024 * 1024;
      string responseData;
      winrt::Windows::Storage::Streams::IBuffer buffer;
      uint32_t length;
      do {
        co_await reader.LoadAsync(segmentSize);
        length = reader.UnconsumedBufferLength();

        if (isText) {
          auto data = std::vector<uint8_t>(length);
          reader.ReadBytes(data);

          responseData += string(Common::Utilities::CheckedReinterpretCast<char *>(data.data()), data.size());
        } else {
          buffer = reader.ReadBuffer(length);
          auto data = CryptographicBuffer::EncodeToBase64String(buffer);

          responseData += winrt::to_string(std::wstring_view(data));
        }
      } while (length > 0);

      if (self->m_onData) {
        self->m_onData(reqArgs->RequestId, std::move(responseData));
      }
    } else {
      if (self->m_onError) {
        self->m_onError(reqArgs->RequestId, response == nullptr ? "request failed" : "No response content", false);
      }
    }
  } catch (std::exception const &e) {
    if (self->m_onError) {
      self->m_onError(reqArgs->RequestId, e.what(), false);
    }
  } catch (hresult_error const &e) {
    if (self->m_onError) {
      self->m_onError(reqArgs->RequestId, Utilities::HResultToString(e), false);
    }
  } catch (...) {
    if (self->m_onError) {
      self->m_onError(reqArgs->RequestId, "Unhandled exception during request", false);
    }
  }

  self->UntrackResponse(reqArgs->RequestId);
} // PerformSendRequest

#pragma region IHttpModuleProxy

void WinRTHttpResource::AddUriHandler(shared_ptr<IUriHandler> /*uriHandler*/) noexcept /*override*/
{
  // TODO: Implement custom URI handling.
}

void WinRTHttpResource::AddRequestBodyHandler(shared_ptr<IRequestBodyHandler> requestBodyHandler) noexcept /*override*/
{
  m_requestBodyHandler = weak_ptr<IRequestBodyHandler>(requestBodyHandler);
}

void WinRTHttpResource::AddResponseHandler(shared_ptr<IResponseHandler> responseHandler) noexcept /*override*/
{
  m_responseHandler = weak_ptr<IResponseHandler>(responseHandler);
}

#pragma endregion IHttpModuleProxy

#pragma endregion WinRTHttpResource

#pragma region IHttpResource

/*static*/ shared_ptr<IHttpResource> IHttpResource::Make(
    winrt::Windows::Foundation::IInspectable const &inspectableProperties) noexcept {
  using namespace winrt::Microsoft::ReactNative;
  using winrt::Windows::Web::Http::HttpClient;

  auto redirFilter = winrt::make<RedirectHttpFilter>();
  HttpClient client;

  if (static_cast<OriginPolicy>(GetRuntimeOptionInt("Http.OriginPolicy")) == OriginPolicy::None) {
    client = HttpClient{redirFilter};
  } else {
    auto globalOrigin = GetRuntimeOptionString("Http.GlobalOrigin");
    OriginPolicyHttpFilter::SetStaticOrigin(std::move(globalOrigin));
    auto opFilter = winrt::make<OriginPolicyHttpFilter>(redirFilter);
    redirFilter.as<RedirectHttpFilter>()->SetRedirectSource(opFilter.as<IRedirectEventSource>());

    client = HttpClient{opFilter};
  }

  auto result = std::make_shared<WinRTHttpResource>(std::move(client));

  // Allow redirect filter to create requests based on the resource's state
  redirFilter.as<RedirectHttpFilter>()->SetRequestFactory(weak_ptr<IWinRTHttpRequestFactory>{result});

  // Register resource as HTTP module proxy.
  if (inspectableProperties) {
    auto propId = ReactPropertyId<ReactNonAbiValue<weak_ptr<IHttpModuleProxy>>>{L"HttpModule.Proxy"};
    auto propBag = ReactPropertyBag{inspectableProperties.try_as<IReactPropertyBag>()};
    auto moduleProxy = weak_ptr<IHttpModuleProxy>{result};
    propBag.Set(propId, std::move(moduleProxy));
  }

  return result;
}

/*static*/ shared_ptr<IHttpResource> IHttpResource::Make() noexcept {
  auto inspectableProperties = IInspectable{nullptr};
  return Make(inspectableProperties);
}

#pragma endregion IHttpResource

} // namespace Microsoft::React::Networking