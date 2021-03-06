/*
	版权：CY001 WDF USB开发板项目  2009/9/1
	
	你所拥有的此份代码拷贝，仅可用于个人学习，任何商业或与利益相关途径的使用，都不被允许。
	如果你未经特许，请不要将代码在网络上传播。项目开发者已将代码发布，并长期维护。

	作者：麦盒数码		张佩 
		  驱动开发网	马勇
		  AMD			夏浩翔

	日期：2009/9/1

	文件：Util.c
	说明：实用程序
	历史：
	创建：
*/

#include "CY001Drv.h"

#define CY001_LOAD_REQUEST    0xA0
#define ANCHOR_LOAD_EXTERNAL  0xA3
#define MAX_INTERNAL_ADDRESS  0x4000
#define INTERNAL_RAM(address) ((address <= MAX_INTERNAL_ADDRESS) ? 1 : 0)

#define MCU_Ctl_REG    0x7F92
#define MCU_RESET_REG  0xE600

// 开启中断读操作。为此我们开启了一个连续读操作。
// 读者应该有兴趣参看相关的WDM代码，为了实现这个功能，WDM的实现相当繁琐，而且容易出错。
NTSTATUS CY001Drv::InterruptReadStart()
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_USB_CONTINUOUS_READER_CONFIG interruptConfig;
	ASSERT(m_hUsbIntInPipe);

	KDBG(DPFLTR_INFO_LEVEL, "[InterruptReadStart]");

	//WDF_IO_TARGET_STATE state;
	WDF_USB_PIPE_INFORMATION pipeInfo;
	WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
	WdfUsbTargetPipeGetInformation(m_hUsbIntInPipe, &pipeInfo);

	// 获取pipe IOTarget的当前状态
	//WdfIoTargetGetState(WdfUsbTargetPipeGetIoTarget(m_hUsbIntInPipe), &state);

	// 要判断标志位。
	// 中断Pipe只需要进行一次continue配置就可以了。后来如果管道中止再重启，不必二次配置。
	if(m_bIntPipeConfigured == FALSE)
	{
		WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&interruptConfig, 
			InterruptRead_sta,			// 回调函数注册。当收到一次读完成消息后，此函数被调用。
			this,	 					// 回调函数参数
			pipeInfo.MaximumPacketSize	// 从设备读取数据的长度
			);

		status = WdfUsbTargetPipeConfigContinuousReader(m_hUsbIntInPipe, &interruptConfig);

		if(NT_SUCCESS(status))
			m_bIntPipeConfigured = TRUE;
		else
			KDBG(DPFLTR_INFO_LEVEL, "Error! Status: %08x", status);
	}

	// 启动Pipe。可能是第一次启动，也可能是后续重启。
	if(NT_SUCCESS(status))
		status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(m_hUsbIntInPipe));
	else
		KDBG(DPFLTR_INFO_LEVEL, "WdfUsbTargetPipeConfigContinuousReader failed with status 0x%08x", status);

	return status;
}

// 停止中断读操作
NTSTATUS CY001Drv::InterruptReadStop()
{
	NTSTATUS status = STATUS_SUCCESS;
	WDFREQUEST Request = NULL;

	if(NULL == m_hUsbIntInPipe)
		return STATUS_SUCCESS;

	KDBG(DPFLTR_INFO_LEVEL, "[InterruptReadStop]");

	if(m_hUsbIntInPipe)
		// 如果还有未完成的IO操作，都Cancel掉。
		WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(m_hUsbIntInPipe), WdfIoTargetCancelSentIo);

	// 完成在手动队列中的所有未完成Request。
	// 如果Queue处于未启动状态，会返回STATUS_WDF_PAUSED；
	// 如果已启动，则会挨个取得其Entry，直到返回STATUS_NO_MORE_ENTRIES。	
	do{
		status = WdfIoQueueRetrieveNextRequest(m_hInterruptManualQueue, &Request);

		if(NT_SUCCESS(status))
		{
			WdfRequestComplete(Request, STATUS_SUCCESS);
		}
	}while(status != STATUS_NO_MORE_ENTRIES && status != STATUS_WDF_PAUSED);

	return STATUS_SUCCESS;
}

void CY001Drv::ClearSyncQueue()
{
	NTSTATUS status;
	WDFREQUEST Request = NULL;

	KDBG(DPFLTR_INFO_LEVEL, "[ClearSyncQueue]");

	// 清空同步队列中的所有同步Request。此部分逻辑与上面函数相同。
	do{
		status = WdfIoQueueRetrieveNextRequest(m_hAppSyncManualQueue, &Request);

		if(NT_SUCCESS(status))
			WdfRequestComplete(Request, STATUS_SUCCESS);

	}while(status != STATUS_NO_MORE_ENTRIES && status != STATUS_WDF_PAUSED);
}

// 从同步队列中取得一个有效Request。
NTSTATUS CY001Drv::GetOneSyncRequest(WDFREQUEST* pRequest)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	ASSERT(pRequest);
	*pRequest = NULL;

	if(m_hAppSyncManualQueue)
		status = WdfIoQueueRetrieveNextRequest(m_hAppSyncManualQueue, pRequest);

	return status;
}

// 完成一个同步Request，并用相关信息填充这个Request。
void CY001Drv::CompleteSyncRequest(DRIVER_SYNC_ORDER_TYPE type, int info)
{
	NTSTATUS status;
	WDFREQUEST Request;
	if(NT_SUCCESS(GetOneSyncRequest(&Request)))
	{
		PDriverSyncPackt pData = NULL;

		if(!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, sizeof(DriverSyncPackt), (void**)&pData, NULL)))
			WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
		else{

			// 填充Output结构内容
			pData->type = type;
			pData->info = info;
			WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(DriverSyncPackt));
		}
	}
}

NTSTATUS CY001Drv::SetDigitron(IN UCHAR chSet)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_USB_CONTROL_SETUP_PACKET controlPacket;
	WDF_MEMORY_DESCRIPTOR hMemDes;

	KDBG(DPFLTR_INFO_LEVEL, "[SetDigitron] %d", chSet);
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&hMemDes, &chSet, sizeof(UCHAR));

	WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
		&controlPacket,
		BmRequestHostToDevice,
		BmRequestToDevice,
		0xD2, // Vendor命令
		0,
		0);

	status = WdfUsbTargetDeviceSendControlTransferSynchronously(
		m_hUsbDevice,
		NULL, NULL,
		&controlPacket,
		&hMemDes,
		NULL);

	return status;
}

NTSTATUS CY001Drv::GetDigitron(OUT UCHAR* pchGet)
{
	NTSTATUS ntStatus = STATUS_UNSUCCESSFUL;
	WDF_USB_CONTROL_SETUP_PACKET controlPacket;
	WDFMEMORY hMem = NULL;
	WDFREQUEST newRequest = NULL;

	ASSERT(pchGet);
	KDBG(DPFLTR_INFO_LEVEL, "[GetDigitron]");

	// 构造内存描述符
	ntStatus = WdfMemoryCreatePreallocated(WDF_NO_OBJECT_ATTRIBUTES, pchGet, sizeof(UCHAR), &hMem);
	if(!NT_SUCCESS(ntStatus))
		return ntStatus;

	// 初始化控制命令
	WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
		&controlPacket,
		BmRequestDeviceToHost,// input命令
		BmRequestToDevice,
		0xD4,// Vendor 命令D4
		0, 0);

	// 创建新的WDF REQUEST对象。
	ntStatus = WdfRequestCreate(NULL, NULL, &newRequest);
	if(!NT_SUCCESS(ntStatus))
		return ntStatus;

	WdfUsbTargetDeviceFormatRequestForControlTransfer(m_hUsbDevice, newRequest, &controlPacket, hMem, NULL);	

	if(NT_SUCCESS(ntStatus))
	{
		// 同步发送
		WDF_REQUEST_SEND_OPTIONS opt;
		WDF_REQUEST_SEND_OPTIONS_INIT(&opt, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
		if(WdfRequestSend(newRequest, WdfDeviceGetIoTarget(m_hDevice), &opt))
		{
			WDF_REQUEST_COMPLETION_PARAMS par;
			WDF_REQUEST_COMPLETION_PARAMS_INIT(&par);
			WdfRequestGetCompletionParams(newRequest, &par);

			// 判断读取到的字符长度。
			if(sizeof(UCHAR) != par.Parameters.Usb.Completion->Parameters.DeviceControlTransfer.Length)
				ntStatus = STATUS_UNSUCCESSFUL;
		}else
			ntStatus = STATUS_UNSUCCESSFUL;
	}

	// 通过WdfXxxCreate创建的对象，必须删除
	WdfObjectDelete(newRequest);

	return ntStatus;
}

NTSTATUS CY001Drv::SetLEDs(IN UCHAR chSet)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_USB_CONTROL_SETUP_PACKET controlPacket;
	WDF_MEMORY_DESCRIPTOR hMemDes;

	KDBG(DPFLTR_INFO_LEVEL, "[SetLEDs] %c", chSet);
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&hMemDes, &chSet, sizeof(UCHAR));

	WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
		&controlPacket,
		BmRequestHostToDevice,
		BmRequestToDevice,
		0xD1, // Vendor命令
		0, 0);

	status = WdfUsbTargetDeviceSendControlTransferSynchronously(
		m_hUsbDevice,
		NULL, NULL,
		&controlPacket,
		&hMemDes,
		NULL);

	return status;
}

NTSTATUS CY001Drv::GetLEDs(OUT UCHAR* pchGet)
{
	NTSTATUS ntStatus = STATUS_UNSUCCESSFUL;
	WDF_USB_CONTROL_SETUP_PACKET controlPacket;
	WDFMEMORY hMem = NULL;
	WDFREQUEST newRequest = NULL;

	KDBG(DPFLTR_INFO_LEVEL, "[GetLEDs]");
	ASSERT(pchGet);

	// 构造内存描述符
	ntStatus = WdfMemoryCreatePreallocated(WDF_NO_OBJECT_ATTRIBUTES, pchGet, sizeof(UCHAR), &hMem);
	if(!NT_SUCCESS(ntStatus))
		return ntStatus;

	// 初始化控制命令
	WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
		&controlPacket,
		BmRequestDeviceToHost,// input命令
		BmRequestToDevice,
		0xD3,// Vendor 命令D3
		0, 0);

	// 创建WDF REQUEST对象。
	ntStatus = WdfRequestCreate(NULL, NULL, &newRequest);
	if(!NT_SUCCESS(ntStatus))
		return ntStatus;

	WdfUsbTargetDeviceFormatRequestForControlTransfer(m_hUsbDevice, newRequest, &controlPacket, hMem, NULL);	

	if(NT_SUCCESS(ntStatus))
	{
		// 同步发送
		WDF_REQUEST_SEND_OPTIONS opt;
		WDF_REQUEST_SEND_OPTIONS_INIT(&opt, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
		if(WdfRequestSend(newRequest, WdfDeviceGetIoTarget(m_hDevice), &opt))
		{
			WDF_REQUEST_COMPLETION_PARAMS par;
			WDF_REQUEST_COMPLETION_PARAMS_INIT(&par);
			WdfRequestGetCompletionParams(newRequest, &par);

			// 判断读取到的字符长度。
			if(sizeof(UCHAR) != par.Parameters.Usb.Completion->Parameters.DeviceControlTransfer.Length)
				ntStatus = STATUS_UNSUCCESSFUL;
		}else
			ntStatus = STATUS_UNSUCCESSFUL;
	}

	// 通过WdfXxxCreate创建的对象，必须删除
	WdfObjectDelete(newRequest);

	return ntStatus;
}

NTSTATUS CY001Drv::GetStringDes(USHORT shIndex, USHORT shLanID, VOID* pBufferOutput, ULONG OutputBufferLength, ULONG* pulRetLen)
{
	NTSTATUS status;

	USHORT  numCharacters;
	PUSHORT  stringBuf;
	WDFMEMORY  memoryHandle;

	KDBG(DPFLTR_INFO_LEVEL, "[GetStringDes] index:%d", shIndex);
	ASSERT(pulRetLen);
	*pulRetLen = 0;

	// 由于String描述符是一个变长字符数组，故首先取得其长度
	status = WdfUsbTargetDeviceQueryString(
		m_hUsbDevice,
		NULL, NULL, NULL, // 传入空字符串
		&numCharacters,
		shIndex,
		shLanID
		);
	if(!NT_SUCCESS(status))
		return status;

	// 判读缓冲区的长度
	if(OutputBufferLength < numCharacters){
		status = STATUS_BUFFER_TOO_SMALL;
		return status;
	}

	// 再次正式地取得String描述符
	status = WdfUsbTargetDeviceQueryString(m_hUsbDevice,
		NULL, NULL,
		(PUSHORT)pBufferOutput,// Unicode字符串
		&numCharacters,
		shIndex,
		shLanID
		);

	// 完成操作
	if(NT_SUCCESS(status)){
		((PUSHORT)pBufferOutput)[numCharacters] = L'\0';// 手动在字符串末尾添加NULL
		*pulRetLen = numCharacters+1;
	}
	return status;
}

NTSTATUS CY001Drv::FirmwareReset(IN UCHAR resetBit)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_USB_CONTROL_SETUP_PACKET controlPacket;
	WDF_MEMORY_DESCRIPTOR memDescriptor;
	PDEVICE_CONTEXT Context = GetDeviceContext(m_hDevice);

	KDBG(DPFLTR_INFO_LEVEL, "[FirmwareReset]");

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memDescriptor, &resetBit, 1);

	// 写地址MCU_RESET_REG
	WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
		&controlPacket,
		BmRequestHostToDevice,
		BmRequestToDevice,
		CY001_LOAD_REQUEST,// Vendor命令
		MCU_RESET_REG,	   // 指定地址
		0);

	status = WdfUsbTargetDeviceSendControlTransferSynchronously(
		m_hUsbDevice, 
		NULL, NULL,	
		&controlPacket,
		&memDescriptor,
		NULL);

	if(!NT_SUCCESS(status))
		KDBG(DPFLTR_ERROR_LEVEL, "FirmwareReset failed: 0x%X!!!", status);

	return status;
}

// 把一段二进制的固件代码写入开发板指定地址处。
//
NTSTATUS CY001Drv::FirmwareUpload(PUCHAR pData, ULONG ulLen, WORD offset)
{
	NTSTATUS ntStatus;
	WDF_USB_CONTROL_SETUP_PACKET controlPacket;
	ULONG chunkCount = 0;
	ULONG ulWritten;
	WDF_MEMORY_DESCRIPTOR memDescriptor;
	WDF_OBJECT_ATTRIBUTES attributes;
	int i;

	chunkCount = ((ulLen + CHUNK_SIZE - 1) / CHUNK_SIZE);

	// 为安全起见，下载过程中，大块数据被分割成以64字节为单位的小块进行发送。
	// 如果以大块进行传递，可能会发生数据丢失的情况。
	//
	for (i = 0; i < chunkCount; i++)
	{
		// 构造内存描述符
		WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memDescriptor, pData, (i < chunkCount-1)?
			CHUNK_SIZE : 
			(ulLen - (chunkCount-1) * CHUNK_SIZE));// 如果不是最后一个块，则CHUNK_SIZE字节；否则要计算尾巴长度。

		// 初始化控制命令
		WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
			&controlPacket,
			BmRequestHostToDevice,
			BmRequestToDevice,
			CY001_LOAD_REQUEST,		// Vendor 命令A3
			offset + i*CHUNK_SIZE,  // 写入起始地址
			0);

		ntStatus = WdfUsbTargetDeviceSendControlTransferSynchronously(
			m_hUsbDevice, 
			NULL, NULL, 
			&controlPacket,
			&memDescriptor, 
			&ulWritten);

		if (!NT_SUCCESS(ntStatus)){
			KDBG( DPFLTR_ERROR_LEVEL, "FirmwareUpload Failed :0x%0.8x!!!", ntStatus);
			break;
		}else			
			KDBG( DPFLTR_INFO_LEVEL, "%d bytes are written.", ulWritten);

		pData += CHUNK_SIZE;
	}

	return ntStatus;
}

// 从开发板内存的的指定地址处读取当前内容
//
NTSTATUS CY001Drv::ReadRAM(WDFREQUEST Request, ULONG* pLen)
{
	NTSTATUS ntStatus;
	WDF_USB_CONTROL_SETUP_PACKET controlPacket;    
	WDFMEMORY   hMem = NULL;
	PFIRMWARE_UPLOAD pUpLoad = NULL;
	WDFREQUEST newRequest;
	void* pData = NULL;
	size_t size;

	KDBG(DPFLTR_INFO_LEVEL, "[ReadRAM]");

	ASSERT(pLen);
	*pLen = 0;

	if(!NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(FIRMWARE_UPLOAD), (void**)&pUpLoad, NULL)) ||
		!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, 1, &pData, &size)))
	{		
		KDBG( DPFLTR_ERROR_LEVEL, "Failed to retrieve memory handle\n");
		return STATUS_INVALID_PARAMETER;
	}

	// 构造内存描述符
	ntStatus = WdfMemoryCreatePreallocated(WDF_NO_OBJECT_ATTRIBUTES, pData, min(size, pUpLoad->len), &hMem);
	if(!NT_SUCCESS(ntStatus))
		return ntStatus;

	// 初始化控制命令
	WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(	
		&controlPacket,
		BmRequestDeviceToHost,// input命令
		BmRequestToDevice,
		CY001_LOAD_REQUEST,// Vendor 命令A0
		pUpLoad->addr,// 地址
		0);

	// 创建被初始化WDF REQUEST对象。
	ntStatus = WdfRequestCreate(NULL, NULL, &newRequest);
	if(!NT_SUCCESS(ntStatus))
		return ntStatus;

	WdfUsbTargetDeviceFormatRequestForControlTransfer(m_hUsbDevice,
		newRequest, &controlPacket, hMem, NULL);	

	if(NT_SUCCESS(ntStatus))
	{
		WDF_REQUEST_SEND_OPTIONS opt;
		WDF_REQUEST_SEND_OPTIONS_INIT(&opt, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
		if(WdfRequestSend(newRequest, WdfDeviceGetIoTarget(m_hDevice), &opt))
		{
			WDF_REQUEST_COMPLETION_PARAMS par;
			WDF_REQUEST_COMPLETION_PARAMS_INIT(&par);
			WdfRequestGetCompletionParams(newRequest, &par);

			// 取得读取到的字符长度。
			*pLen = par.Parameters.Usb.Completion->Parameters.DeviceControlTransfer.Length;
		}
	}

	// 通过WdfXxxCreate创建的对象，必须删除
	WdfObjectDelete(newRequest);

	return ntStatus;
}

// 删除新建命令，并完成原始命令。
void ControlRequestComplete(IN WDFREQUEST  Request,
							IN WDFIOTARGET  Target,
							IN PWDF_REQUEST_COMPLETION_PARAMS  Params,
							IN WDFCONTEXT  Context)
{
	ULONG len = 0;
	WDFREQUEST OriginalReqeust = (WDFREQUEST)Context;// 获得原始命令
	NTSTATUS status = WdfRequestGetStatus(Request);

	KDBG(DPFLTR_INFO_LEVEL, "[ControlRequestComplete] status: %08X", status);

	if(status == STATUS_IO_TIMEOUT)
		KDBG(DPFLTR_ERROR_LEVEL, "the control request is time out, should be checked.");
	else
		len = Params->Parameters.Usb.Completion->Parameters.DeviceControlTransfer.Length;

	WdfObjectDelete(Request); // 删除之，此乃新建也
	WdfRequestCompleteWithInformation(OriginalReqeust, status, len);// 完成之，此乃来自用户程序也
}

// USB控制端口命令。这些命令包括USB预定义的命令、Vendor自定义命令、各种特殊类定义命令等。
// 
NTSTATUS CY001Drv::UsbControlRequest(IN WDFREQUEST Request)
{
	NTSTATUS status;
	WDFREQUEST RequestNew = NULL;
	WDFMEMORY memHandle = NULL;
	WDF_USB_CONTROL_SETUP_PACKET controlPacket;
	WDF_REQUEST_SEND_OPTIONS  opt;

	PUSB_CTL_REQ pRequestControl;
	char* pOutputBuf;
	WDF_USB_BMREQUEST_DIRECTION dir;
	WDF_USB_BMREQUEST_RECIPIENT recipient;

	KDBG(DPFLTR_INFO_LEVEL, "[UsbControlRequest]");
	
	__try
	{
		// 输入参数为一个USB_CTL_REQ类型的结构体
		status = WdfRequestRetrieveInputBuffer(Request, sizeof(USB_CTL_REQ)-1, (void**)&pRequestControl, NULL);
		if(!NT_SUCCESS(status))
			__leave;

		// 输出缓冲区为USB_CTL_REQ中的buf成员变量。最短长度为1.
		status = WdfRequestRetrieveOutputBuffer(Request, max(1, pRequestControl->length), (void**)&pOutputBuf, NULL);
		if(!NT_SUCCESS(status))
			__leave;

		// 判断输入或者输出命令
		if(pRequestControl->type.Request.bDirInput) 
			dir = BmRequestDeviceToHost;
		else
			dir = BmRequestHostToDevice;

		// USB设备中的接受方。可以是设备本身、接口、端点，或三者之外的未知者。
		switch(pRequestControl->type.Request.recepient){
			case 0: 
				recipient = BmRequestToDevice;
				break;
			case 1:
				recipient = BmRequestToInterface;
				break;
			case 2:
				recipient = BmRequestToEndpoint;
				break;
			case 3:
			default:
				recipient = BmRequestToOther;
		}

		// 与命令相关的数据被保存在buf成员指针中。
		if(pRequestControl->length)
		{
			status = WdfMemoryCreatePreallocated(NULL, pOutputBuf, pRequestControl->length, &memHandle);
			if(!NT_SUCCESS(status))
				__leave;
		}

		KDBG(DPFLTR_INFO_LEVEL, "%s: recepient:%d type:%d",
			pRequestControl->type.Request.bDirInput?"In Req":"Out Req", 
			pRequestControl->type.Request.recepient, 
			pRequestControl->type.Request.Type);

		// 初始化Setup结构体。WDF提供了5中初始化宏，在这里分别都有展示。
		if(pRequestControl->type.Request.Type == 0x1) // Class 命令
		{			
			KDBG(DPFLTR_INFO_LEVEL, "Class Request");
			WDF_USB_CONTROL_SETUP_PACKET_INIT_CLASS (
				&controlPacket,
				dir, recipient,
				pRequestControl->req,
				pRequestControl->value,
				pRequestControl->index
				);
		}
		else if(pRequestControl->type.Request.Type == 0x2) // Vendor 命令
		{
			if(pRequestControl->req == 0xA0 || (pRequestControl->req >= 0xD1 && pRequestControl->req <= 0xD5))
			{
				KDBG(DPFLTR_INFO_LEVEL, "Known Vendor Request.");// 可识别的Vendor 命令
			}
			else
			{
				KDBG(DPFLTR_INFO_LEVEL, "Unknown Vendor Request. DANGER!!!");// 不可识别，危险!!!!
			}

			WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR (
				&controlPacket,
				dir,
				recipient,
				pRequestControl->req,
				pRequestControl->value,
				pRequestControl->index
				);
		}
		else if(pRequestControl->type.Request.Type == 0x0) // 标准命令
		{
			KDBG(DPFLTR_INFO_LEVEL, "Standard Request");

			if(pRequestControl->req == 1 || pRequestControl->req == 3) // Feature命令
			{
				KDBG(DPFLTR_INFO_LEVEL, "Feature Request");
				WDF_USB_CONTROL_SETUP_PACKET_INIT_FEATURE(
					&controlPacket,
					recipient,
					pRequestControl->value,
					pRequestControl->index,				
					pRequestControl->req == 1 ? FALSE: // clear
												TRUE   // set
				);
			}
			else if(pRequestControl->req == 0)							// Status命令
			{			
				KDBG(DPFLTR_INFO_LEVEL, "Status Request");
				WDF_USB_CONTROL_SETUP_PACKET_INIT_GET_STATUS(
					&controlPacket,
					recipient,
					pRequestControl->index
				);
			}
			else														// 其他
			{				
				WDF_USB_CONTROL_SETUP_PACKET_INIT (
					&controlPacket,
					dir, recipient,
					pRequestControl->req,
					pRequestControl->value,
					pRequestControl->index
					);
			}
		}

		// 创建一个新的Request对象
		status = WdfRequestCreate(NULL, WdfDeviceGetIoTarget(m_hDevice), &RequestNew);
		if(!NT_SUCCESS(status))
			__leave;

		// 针对Usb设备，用controlPacket结构格式化新创建的Request对象。
		status = WdfUsbTargetDeviceFormatRequestForControlTransfer(
			m_hUsbDevice,
			RequestNew, &controlPacket, 
			memHandle, NULL
			);

		if (!NT_SUCCESS(status)){
			KDBG( DPFLTR_ERROR_LEVEL, "WdfUsbTargetDeviceFormatRequestForControlTransfer failed");
			__leave;
		}

		// 异步发送
		// 设置Timeout标志，以防止用户在发送了不可识别的命令后，驱动无限期等待的现象。
		// 等待超过2秒，请求将被取消。
		WDF_REQUEST_SEND_OPTIONS_INIT(&opt, 0);
		WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opt, WDF_REL_TIMEOUT_IN_SEC(2));
		WdfRequestSetCompletionRoutine(RequestNew, ControlRequestComplete, Request);
		if(WdfRequestSend(RequestNew, WdfDeviceGetIoTarget(m_hDevice), &opt) == FALSE) 
		{
			status = WdfRequestGetStatus(RequestNew);
			KDBG(DPFLTR_ERROR_LEVEL, "WdfRequestSend failed");
			__leave;
		}		
	}
	__finally{
	}
	
	if(!NT_SUCCESS(status))
		KDBG(DPFLTR_ERROR_LEVEL, "status: %08X", status);
	
	return status;
}

// 通过管道Item，可以得到管道句柄。调用Abort方法，中止Pipe上的数据流。
NTSTATUS CY001Drv::AbortPipe(IN ULONG nPipeNum)
{
	KDBG(DPFLTR_INFO_LEVEL, "[AbortPipe]");

	WDFUSBPIPE pipe = WdfUsbInterfaceGetConfiguredPipe(m_hUsbInterface, nPipeNum, NULL);

	if(pipe == NULL)
		return STATUS_INVALID_PARAMETER;// 可能nPipeNum太大了

	NTSTATUS status = WdfUsbTargetPipeAbortSynchronously(pipe, NULL, NULL);
	if(!NT_SUCCESS(status))
		KDBG( DPFLTR_ERROR_LEVEL, "AbortPipe Failed: 0x%0.8X", status);

	return status;
}

NTSTATUS CY001Drv::UsbSetOrClearFeature(WDFREQUEST Request)
{
	NTSTATUS status;
	WDFREQUEST Request_New = NULL;
	WDF_USB_CONTROL_SETUP_PACKET controlPacket;
	PSET_FEATURE_CONTROL pFeaturePacket;

	KDBG(DPFLTR_INFO_LEVEL, "[UsbSetOrClearFeature]");

	status = WdfRequestRetrieveInputBuffer(Request, sizeof(SET_FEATURE_CONTROL), (void**)&pFeaturePacket, NULL);
	if(!NT_SUCCESS(status))return status;

	WDF_USB_CONTROL_SETUP_PACKET_INIT_FEATURE(&controlPacket, 
		BmRequestToDevice,
		pFeaturePacket->FeatureSelector,
		pFeaturePacket->Index,
		pFeaturePacket->bSetOrClear
		);

	status = WdfRequestCreate(NULL, NULL, &Request_New);
	if(!NT_SUCCESS(status)){
		KDBG( DPFLTR_ERROR_LEVEL, "WdfRequestCreate Failed: 0x%0.8X", status);
		return status;
	}

	WdfUsbTargetDeviceFormatRequestForControlTransfer(
		m_hUsbDevice,
		Request_New, 
		&controlPacket, 
		NULL, NULL);

	if(FALSE == WdfRequestSend(Request_New, WdfDeviceGetIoTarget(m_hDevice), NULL))
		status = WdfRequestGetStatus(Request_New);
	WdfObjectDelete(Request_New);

	return status;
}

