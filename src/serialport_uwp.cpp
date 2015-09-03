#include "serialport.h"
#include <nan.h>
#include <ppltasks.h>

#define MAX_BUFFER_SIZE 1000

using namespace concurrency;
using namespace Platform;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Devices::SerialCommunication;
using namespace Windows::Foundation;
using namespace Windows::Storage::Streams;

struct WindowsPlatformOptions : OpenBatonPlatformOptions
{
};

OpenBatonPlatformOptions* ParsePlatformOptions(const v8::Local<v8::Object>& options) {
  // currently none
  return new WindowsPlatformOptions();
}

int bufferSize;
SerialDevice^ g_device;

void ParseUSBSerialDeviceId(String^ deviceId, UINT16* vid, UINT16* pid, std::wstring& serialStr) {
  if (0 == deviceId->Length()) {
    return;
  }
  std::wstring wStr(deviceId->Data());

  std::wstring usbStr = wStr.substr(0, 3);
  if (0 != usbStr.compare(L"USB")) {
    return;
  }
  *vid = wcstol(wStr.substr(8, 4).c_str(), NULL, 16);
  *pid = wcstol(wStr.substr(17, 4).c_str(), NULL, 16);
  serialStr = wStr.substr(22);
  int index = -1;
  index = serialStr.find_first_of(L"\\", 1);
  if (-1 == index) {
    index = serialStr.find_first_of(L"#", 1);
  }
  if (-1 != index) {
    serialStr = serialStr.substr(0, index);
  }
}

void PlatformStringToChar(String^ platStr, char* charStr) {
  if (0 == platStr->Length()) {
      return;
  }
  std::wstring wStr(platStr->Data());
  auto wideData = wStr.c_str();
  int bufferSize = WideCharToMultiByte(CP_UTF8, 0, wideData, -1, nullptr, 0, NULL, NULL);
  auto utf8 = std::make_unique<char[]>(bufferSize);
  if (0 == WideCharToMultiByte(CP_UTF8, 0, wideData, -1, utf8.get(), bufferSize, NULL, NULL)) {
    throw std::exception("Can't convert string to UTF8");
  }
  strcpy(charStr, utf8.get());
}

void EIO_Open(uv_work_t* req) {
  OpenBaton* data = static_cast<OpenBaton*>(req->data);
  UINT16 vid = 0;
  UINT16 pid = 0;
  std::wstring serialStr;
  SerialDevice^ device;

  bufferSize = data->bufferSize;
  if (bufferSize > MAX_BUFFER_SIZE || bufferSize == 0) {
    bufferSize = MAX_BUFFER_SIZE;
  }
  
  // data->path (name of the device passed in to the 'open' function of serialport)
  // can either be a regular port name like "COM1" or "UART2" or it can be in a 
  // format describing a USB-Serial device that has not been assigned a port name.
  // The format is:
  // USB[#|\]VID_<Vendor ID>&PID_<Product ID>[#|\]<Serial String>
  // Example:
  // USB#VID_2341&PID_0043#85436323631351311141
  // This string can be found by either:
  // 1. Calling the serialport list function.
  // 2. (On Windows 10 IoT Core) Running "devcon status usb*"
  std::string s_str = std::string(data->path);
  std::wstring wid_str = std::wstring(s_str.begin(), s_str.end());
  const wchar_t* w_char = wid_str.c_str();
  String^ deviceId = ref new String(w_char);
  String ^aqs = SerialDevice::GetDeviceSelector();

  auto deviceInfoCollection = concurrency::create_task(DeviceInformation::FindAllAsync(aqs)).get();
  if (deviceInfoCollection->Size < 1) {
    return;
  }

  for (int i = 0; i < deviceInfoCollection->Size; i++) {
    device = concurrency::create_task(SerialDevice::FromIdAsync(deviceInfoCollection->GetAt(i)->Id)).get();
    if (device) {
      if (0 == String::CompareOrdinal(device->PortName, deviceId)) {
        break;
      }
      else {
        // Check USB-Serial
        ParseUSBSerialDeviceId(deviceId, &vid, &pid, serialStr);
        if (device->UsbVendorId == vid && device->UsbProductId == pid) {
          std::wstring wStr(deviceId->Data());
          int index = -1;
          index = wStr.find(serialStr, 0);
          if (-1 != index) {
              break;
          }
        }
      }
    }
  }

  if (!device) {
    return;
  }

  device->BaudRate = data->baudRate;
  device->DataBits = data->dataBits;
  switch (data->parity) {
    case SERIALPORT_PARITY_NONE:
      device->Parity = SerialParity::None;
      break;
    case SERIALPORT_PARITY_MARK:
      device->Parity = SerialParity::Mark;
      break;
    case SERIALPORT_PARITY_EVEN:
      device->Parity = SerialParity::Even;
      break;
    case SERIALPORT_PARITY_ODD:
      device->Parity = SerialParity::Odd;
      break;
    case SERIALPORT_PARITY_SPACE:
      device->Parity = SerialParity::Space;
      break;
  }
  switch (data->stopBits) {
    case SERIALPORT_STOPBITS_ONE:
      device->StopBits = SerialStopBitCount::One;
      break;
    case SERIALPORT_STOPBITS_ONE_FIVE:
      device->StopBits = SerialStopBitCount::OnePointFive;
      break;
    case SERIALPORT_STOPBITS_TWO:
      device->StopBits = SerialStopBitCount::Two;
      break;
  }

  // Set the com port read/write timeouts
  TimeSpan commTimeout;
  commTimeout.Duration = 10000000; // 1 second. Duration is expressed in 100 nanosecond units.
  device->ReadTimeout = commTimeout;
  device->WriteTimeout = commTimeout;

  data->device = device;
  g_device = device;
  data->result = 0;
}

struct WatchPortBaton {
public:
#ifdef UWP
  SerialDevice^ device;
#endif
  HANDLE fd;
  DWORD bytesRead;
  char buffer[MAX_BUFFER_SIZE];
  char errorString[ERROR_STRING_SIZE];
  DWORD errorCode;
  bool disconnected;
  NanCallback* dataCallback;
  NanCallback* errorCallback;
  NanCallback* disconnectedCallback;
};

void EIO_Update(uv_work_t* req) {
}


void EIO_Set(uv_work_t* req) {
}


void EIO_WatchPort(uv_work_t* req) {
  WatchPortBaton* data = static_cast<WatchPortBaton*>(req->data);
  data->bytesRead = 0;
  data->disconnected = false;

  DataReader^ dataReader = ref new DataReader(data->device->InputStream);
  dataReader->InputStreamOptions = InputStreamOptions::Partial;

  auto bytesToRead = concurrency::create_task(dataReader->LoadAsync((unsigned int)bufferSize)).get();
  data->bytesRead = bytesToRead;
  try {
    memset(data->buffer, 0, MAX_BUFFER_SIZE);
    // Keep reading until we consume the complete stream.
    while (dataReader->UnconsumedBufferLength > 0) {
        char byte = dataReader->ReadByte();
        strncat(data->buffer, &byte, 1);
    }
  }
  catch (Exception^ e) {
      data->errorCode = e->HResult;
      PlatformStringToChar(e->Message, data->errorString);
  }
  dataReader->DetachStream();
}

void DisposeWatchPortCallbacks(WatchPortBaton* data) {
  delete data->dataCallback;
  delete data->errorCallback;
  delete data->disconnectedCallback;
}

void EIO_AfterWatchPort(uv_work_t* req) {
  NanScope();

  WatchPortBaton* data = static_cast<WatchPortBaton*>(req->data);
  if (data->disconnected) {
    data->disconnectedCallback->Call(0, NULL);
    DisposeWatchPortCallbacks(data);
    goto cleanup;
  }

  if (data->bytesRead > 0) {
    v8::Handle<v8::Value> argv[1];
    argv[0] = NanNewBufferHandle(data->buffer, data->bytesRead);
    data->dataCallback->Call(1, argv);
  }
  else if (data->errorCode > 0) {
    if (data->errorCode == E_HANDLE) {
      DisposeWatchPortCallbacks(data);
      goto cleanup;
    }
    else {
      v8::Handle<v8::Value> argv[1];
      argv[0] = NanError(data->errorString);
      data->errorCallback->Call(1, argv);
      Sleep(100); // prevent the errors from occurring too fast
    }
  }
  AfterOpenSuccess(data->device, data->dataCallback, data->disconnectedCallback, data->errorCallback);

cleanup:
  delete data;
  delete req;
}

void AfterOpenSuccess(SerialDevice^ device, NanCallback* dataCallback, NanCallback* disconnectedCallback, NanCallback* errorCallback) {
  WatchPortBaton* baton = new WatchPortBaton();
  memset(baton, 0, sizeof(WatchPortBaton));
  baton->device = device;
  baton->dataCallback = dataCallback;
  baton->errorCallback = errorCallback;
  baton->disconnectedCallback = disconnectedCallback;

  uv_work_t* req = new uv_work_t();
  req->data = baton;

  uv_queue_work(uv_default_loop(), req, EIO_WatchPort, (uv_after_work_cb)EIO_AfterWatchPort);
}

void EIO_Write(uv_work_t* req) {
  QueuedWrite* queuedWrite = static_cast<QueuedWrite*>(req->data);
  WriteBaton* data = static_cast<WriteBaton*>(queuedWrite->baton);
  data->result = 0;

  if (!g_device) {
    return;
  }
  DataWriter^ dataWriter = ref new DataWriter(g_device->OutputStream);
  dataWriter->WriteBytes(ref new Array<unsigned char>((unsigned char*)data->bufferData, data->bufferLength));
  auto bytesStored = concurrency::create_task(dataWriter->StoreAsync()).get();
  data->result = bytesStored;
  if(data->bufferLength == bytesStored) {
    data->offset = data->bufferLength;
  }
  dataWriter->DetachStream();
}

void EIO_Close(uv_work_t* req) {
  if (g_device) {
    delete g_device;
  }
}

void EIO_List(uv_work_t* req) {
  ListBaton* data = static_cast<ListBaton*>(req->data);

  String ^aqs = SerialDevice::GetDeviceSelector();
  
  auto deviceInfoCollection = concurrency::create_task(DeviceInformation::FindAllAsync(aqs)).get();
  if (deviceInfoCollection->Size < 1) {
    return;
  }

  for (size_t i = 0; i < deviceInfoCollection->Size; i++) {
    ListResultItem* resultItem = new ListResultItem();

    auto serialDevice = concurrency::create_task(SerialDevice::FromIdAsync(deviceInfoCollection->GetAt(i)->Id)).get();
    if (!serialDevice) {
      return;
    }

    char* comName = new char[MAX_PATH];
    memset(comName, 0, MAX_PATH);
    PlatformStringToChar(serialDevice->PortName, comName);
    if (0 == strcmp(comName, "")) {
      // Show the device ID if a port name has not been assigned
      PlatformStringToChar(deviceInfoCollection->GetAt(i)->Id, comName);
      resultItem->comName = comName;
    }
    else {
      resultItem->comName = comName;
    }
    resultItem->manufacturer = "";
    resultItem->pnpId = "";
    data->results.push_back(resultItem);
  }
}

void EIO_Flush(uv_work_t* req) {
}

void EIO_Drain(uv_work_t* req) {
}