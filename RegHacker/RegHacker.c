#include "stdafx.h"
#include "debug.h"
#include "RegHacker.h"

#define IOCTL_GETADDR CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)

void RegHackerUnload(IN PDRIVER_OBJECT DriverObject);
NTSTATUS RegHackerCreateClose(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
NTSTATUS RegHackerDefaultHandler(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
NTSTATUS RegHackerAddDevice(IN PDRIVER_OBJECT  DriverObject, IN PDEVICE_OBJECT  PhysicalDeviceObject);
NTSTATUS RegHackerPnP(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
NTSTATUS RegHackerIOControl(IN PDEVICE_OBJECT fdo, IN PIRP Irp);

NTSYSAPI NTSTATUS NTAPI ZwQueryInformationProcess(
        HANDLE ProcessHandle,
        PROCESSINFOCLASS ProcessInformationClass,
        PVOID ProcessInformation,
        ULONG ProcessInformationLength,
        PULONG ReturnLength
);

/* Export SSDT structure */
typedef struct _deviceExtension
{
	PDEVICE_OBJECT DeviceObject;
	PDEVICE_OBJECT TargetDeviceObject;
	PDEVICE_OBJECT PhysicalDeviceObject;
	UNICODE_STRING DeviceInterface;
} RegHacker_DEVICE_EXTENSION, *PRegHacker_DEVICE_EXTENSION;

typedef struct _SERVICE_DESCRIPTOR_TABLE
{
    unsigned int *ServiceTableBase;
    PULONG ServiceCounterTableBase;
    ULONG NumberOfService;
    ULONG ParamTableBase;
} SERVICE_DESCRIPTOR_TABLE,*PSERVICE_DESCRIPTOR_TABLE;
extern PSERVICE_DESCRIPTOR_TABLE KeServiceDescriptorTable;

/* Macro to easily get the SSDT entry */
#define SYSTEMSERVICE(_function) KeServiceDescriptorTable->ServiceTableBase[*(PULONG)((PUCHAR)_function+1)]

/* Export kernel syscalls */
NTSYSAPI NTSTATUS NTAPI ZwOpenProcess(
        PHANDLE ProcessHandle,
        ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes,
        PCLIENT_ID ClientId
);
NTSYSAPI NTSTATUS NTAPI ZwOpenKey(
        PHANDLE KeyHandle,
        ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes
);
NTSYSAPI NTSTATUS NTAPI ZwCreateKey(
        PHANDLE KeyHandle,
        ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes,
        ULONG TitleIndex,
        PUNICODE_STRING Class,
        ULONG CreateOptions,
        PULONG Disposition
);

/* define syscall functiona types to be clear */
typedef NTSTATUS (*fnZwOpenKey)(
        PHANDLE KeyHandle,
        ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes);
fnZwOpenKey RealZwOpenKey = NULL;
typedef NTSTATUS (*fnZwCreateKey)(
        PHANDLE KeyHandle,
        ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes,
        ULONG TitleIndex,
        PUNICODE_STRING Class,
        ULONG CreateOptions,
        PULONG Disposition);
fnZwCreateKey RealZwCreateKey = NULL;
typedef NTSTATUS (*fnZwOpenProcess)(
        PHANDLE ProcessHandle,
        ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes,
        PCLIENT_ID ClientId);
fnZwOpenProcess RealZwOpenProcess = NULL;

/* redefine syscalls */
NTSTATUS NewZwOpenKey(
        PHANDLE KeyHandle,
        ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes)
{
        /* KDBG("Someone opened a key\n"); */
        return RealZwOpenKey(KeyHandle, DesiredAccess, ObjectAttributes);
}

NTSTATUS NewZwCreateKey(
        PHANDLE KeyHandle,
        ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes,
        ULONG TitleIndex,
        PUNICODE_STRING Class,
        ULONG CreateOptions,
        PULONG Disposition)
{
        /* HANDLE current; */
        ULONG retLen, bufLen;
        PVOID buffer = NULL;
        NTSTATUS status;
        ULONG idx;
        ANSI_STRING aStr;

        KDBG("Hijack ZwCreateKey\n");

        /* current = PsGetCurrentProcess(); */
        
        /* 27 is the ProcessImageFileName */
        /* ZwQueryInformationProcess(current, */
        ZwQueryInformationProcess(NtCurrentProcess(),
                                  ProcessImageFileName,
                                  NULL,
                                  0,
                                  &retLen);
        bufLen = retLen - sizeof(UNICODE_STRING);
        KDBG("buflen: %lx %lx\n", retLen, bufLen);
        buffer = ExAllocatePool(NonPagedPool, retLen);
        if (NULL == buffer) {
                goto real;
        }
        KDBG("before query\n");
        status = ZwQueryInformationProcess(NtCurrentProcess(),
                                           ProcessImageFileName,
                                           buffer,
                                           retLen,
                                           &retLen);
        /* if (NT_SUCCESS(status)) { */
        /* aStr.Buffer = (PCHAR)ExAllocatePool(NonPagedPool, ((PUNICODE_STRING)buffer)->Length + 1); */
        aStr.Length = 0;
        /* aStr.MaximumLength = ((PUNICODE_STRING)buffer)->Length + 1; */
        /* XXX buffer is already a pointer to a memory allocated by ExAllocatePool
         * no need to use `&' here ... */
        RtlUnicodeStringToAnsiString(&aStr, (PUNICODE_STRING)buffer, TRUE);
        KDBG("%s\n", aStr.Buffer);
        RtlFreeAnsiString(&aStr);
        /* } */
        ExFreePool(buffer);
real:
        return RealZwCreateKey(KeyHandle, DesiredAccess, ObjectAttributes, TitleIndex, Class, CreateOptions, Disposition);
}

NTSTATUS NewZwOpenProcess(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess,
                          POBJECT_ATTRIBUTES ObjectAttributes, PCLIENT_ID ClientId)
{
        KDBG("Hijack success!\n");
        return RealZwOpenProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
}

// {c2c3d66a-b83a-45fe-a6df-e64a79534b34}
static const GUID GUID_RegHackerInterface = {0xC2C3D66A, 0xb83a, 0x45fe, {0xa6, 0xdf, 0xe6, 0x4a, 0x79, 0x53, 0x4b, 0x34 } };

#ifdef __cplusplus
extern "C" NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING  RegistryPath);
#endif

NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING  RegistryPath)
{
	unsigned i;

        KDBG("%s called\n", __FUNCTION__);
	KDBG("Hello from RegHacker!\n");
	
	for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		DriverObject->MajorFunction[i] = RegHackerDefaultHandler;

	DriverObject->MajorFunction[IRP_MJ_CREATE] = RegHackerCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = RegHackerCreateClose;
	DriverObject->MajorFunction[IRP_MJ_PNP] = RegHackerPnP;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RegHackerIOControl;
	DriverObject->MajorFunction[IRP_MJ_READ] = RegHackerIOControl;
	DriverObject->MajorFunction[IRP_MJ_WRITE] = RegHackerIOControl;

	DriverObject->DriverUnload = RegHackerUnload;
	DriverObject->DriverStartIo = NULL;
	DriverObject->DriverExtension->AddDevice = RegHackerAddDevice;

        KDBG("SSDT Driver has started\n");

        /* replace SSDT entries */
        RealZwOpenProcess = (fnZwOpenProcess)(SYSTEMSERVICE(ZwOpenProcess));
        (fnZwOpenProcess)(SYSTEMSERVICE(ZwOpenProcess)) = NewZwOpenProcess;

        RealZwOpenKey = (fnZwOpenKey)(SYSTEMSERVICE(ZwOpenKey));
        (fnZwOpenKey)(SYSTEMSERVICE(ZwOpenKey)) = NewZwOpenKey;

        RealZwCreateKey = (fnZwCreateKey)(SYSTEMSERVICE(ZwCreateKey));
        (fnZwCreateKey)(SYSTEMSERVICE(ZwCreateKey)) = NewZwCreateKey;

	return STATUS_SUCCESS;
}

void RegHackerUnload(IN PDRIVER_OBJECT DriverObject)
{
        KDBG("%s called\n", __FUNCTION__);
        /* restore original SSDT entries */
        (fnZwOpenProcess)(SYSTEMSERVICE(ZwOpenProcess)) = RealZwOpenProcess;
        (fnZwOpenKey)(SYSTEMSERVICE(ZwOpenKey)) = RealZwOpenKey;
        (fnZwCreateKey)(SYSTEMSERVICE(ZwCreateKey)) = RealZwCreateKey;
	KDBG("Goodbye from RegHacker!\n");
}

NTSTATUS RegHackerCreateClose(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
        KDBG("%s called\n", __FUNCTION__);
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

NTSTATUS RegHackerIOControl(IN PDEVICE_OBJECT pDeviceObject, IN PIRP Irp)
{
        /* Universal verbose preparations */
        NTSTATUS status = STATUS_SUCCESS;
        PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
        ULONG cbin = stack->Parameters.DeviceIoControl.InputBufferLength;
        ULONG cbout = stack->Parameters.DeviceIoControl.OutputBufferLength;
        ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;


        KDBG("%s called\n", __FUNCTION__);
        KDBG("Entering ioctl\n");

        KDBG("address %p %p %p\n", RealZwOpenProcess, RealZwOpenKey, RealZwCreateKey);


        status = STATUS_SUCCESS;
        stack = IoGetCurrentIrpStackLocation(Irp);
        cbin = stack->Parameters.DeviceIoControl.InputBufferLength;
        cbout = stack->Parameters.DeviceIoControl.OutputBufferLength;
        code = stack->Parameters.DeviceIoControl.IoControlCode;

        switch (code) {
        case IOCTL_SET_EVENT:
                /* /\* borrow from CSDN *\/ */

                /* /\* retrieve user event *\/ */
                /* HANDLE hUserEvent = *(HANDLE *)Irp->AssociatedIrp.SystemBuffer; */

                /* /\* convert user handle into event object *\/ */
                /* status = ObReferenceObjectByHandle(hUserEvent, EVENT_MODIFY_STATE, */
                /*                                    *ExEventObjectType, KernelMode, */
                /*                                    (PVOID*)&pDeviceObject->pEvent, */
                /*                                    NULL); */
                /* /\* make sure it is 0 *\/ */
                /* KDBG("reference status = %d\n", status); */
                /* break; */
        default:
                status = STATUS_INVALID_VARIANT;
        }

        /* XXX The following steps are necessary or else
         * the userspace will definitely hang */
        Irp->IoStatus.Status = STATUS_SUCCESS;
        /* Irp->IoStatus.Information */
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
}

NTSTATUS RegHackerDefaultHandler(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
	PRegHacker_DEVICE_EXTENSION deviceExtension = NULL;
	
        KDBG("%s called\n", __FUNCTION__);
	IoSkipCurrentIrpStackLocation(Irp);
	deviceExtension = (PRegHacker_DEVICE_EXTENSION) DeviceObject->DeviceExtension;
	return IoCallDriver(deviceExtension->TargetDeviceObject, Irp);
}

NTSTATUS RegHackerAddDevice(IN PDRIVER_OBJECT  DriverObject, IN PDEVICE_OBJECT  PhysicalDeviceObject)
{
	PDEVICE_OBJECT DeviceObject = NULL;
	PRegHacker_DEVICE_EXTENSION pExtension = NULL;
	NTSTATUS status;

        UNICODE_STRING devName;
        UNICODE_STRING symLinkName;
        KDBG("%s called\n", __FUNCTION__);

        RtlInitUnicodeString(&devName,L"\\Device\\RegHackerDevice");

	status = IoCreateDevice(DriverObject,
                                sizeof(RegHacker_DEVICE_EXTENSION),
                                &devName,
                                FILE_DEVICE_UNKNOWN,
                                0,
                                0,
                                &DeviceObject);

	if (!NT_SUCCESS(status))
		return status;

	pExtension = (PRegHacker_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	pExtension->DeviceObject = DeviceObject;
	pExtension->PhysicalDeviceObject = PhysicalDeviceObject;
	pExtension->TargetDeviceObject = IoAttachDeviceToDeviceStack(DeviceObject, PhysicalDeviceObject);

	status = IoRegisterDeviceInterface(PhysicalDeviceObject, &GUID_RegHackerInterface, NULL, &pExtension->DeviceInterface);
	ASSERT(NT_SUCCESS(status));

        /* NOT creating symbolic link will not export device ioctl interface to userspace */
	RtlInitUnicodeString(&symLinkName,L"\\DosDevices\\RegHacker");
        /* pExtension->ustrDeviceName = devName; */
        /* pExtension->ustrSymLinkName = symLinkName; */
        status = IoCreateSymbolicLink(&symLinkName,&devName);
	if( !NT_SUCCESS(status))
	{
		IoDeleteSymbolicLink(&symLinkName);
		status = IoCreateSymbolicLink(&symLinkName,&devName);
		if( !NT_SUCCESS(status))
		{
			return status;
		}
	}

	DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
	return STATUS_SUCCESS;
}


NTSTATUS RegHackerIrpCompletion(
					  IN PDEVICE_OBJECT DeviceObject,
					  IN PIRP Irp,
					  IN PVOID Context
					  )
{
	PKEVENT Event = (PKEVENT) Context;

        KDBG("%s called\n", __FUNCTION__);
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(Irp);

	KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

	return(STATUS_MORE_PROCESSING_REQUIRED);
}

NTSTATUS RegHackerForwardIrpSynchronous(
							  IN PDEVICE_OBJECT DeviceObject,
							  IN PIRP Irp
							  )
{
	PRegHacker_DEVICE_EXTENSION   deviceExtension;
	KEVENT event;
	NTSTATUS status;

        KDBG("%s called\n", __FUNCTION__);
	KeInitializeEvent(&event, NotificationEvent, FALSE);
	deviceExtension = (PRegHacker_DEVICE_EXTENSION) DeviceObject->DeviceExtension;

	IoCopyCurrentIrpStackLocationToNext(Irp);

	IoSetCompletionRoutine(Irp, RegHackerIrpCompletion, &event, TRUE, TRUE, TRUE);

	status = IoCallDriver(deviceExtension->TargetDeviceObject, Irp);

	if (status == STATUS_PENDING) {
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = Irp->IoStatus.Status;
	}
	return status;
}

NTSTATUS RegHackerPnP(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
	PRegHacker_DEVICE_EXTENSION pExt = ((PRegHacker_DEVICE_EXTENSION)DeviceObject->DeviceExtension);
	NTSTATUS status;

        KDBG("%s called\n", __FUNCTION__);
	ASSERT(pExt);

	switch (irpSp->MinorFunction)
	{
	case IRP_MN_START_DEVICE:
		IoSetDeviceInterfaceState(&pExt->DeviceInterface, TRUE);
		Irp->IoStatus.Status = STATUS_SUCCESS;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return STATUS_SUCCESS;

	case IRP_MN_QUERY_REMOVE_DEVICE:
		Irp->IoStatus.Status = STATUS_SUCCESS;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return STATUS_SUCCESS;

	case IRP_MN_REMOVE_DEVICE:
		IoSetDeviceInterfaceState(&pExt->DeviceInterface, FALSE);
		status = RegHackerForwardIrpSynchronous(DeviceObject, Irp);
		IoDetachDevice(pExt->TargetDeviceObject);
		IoDeleteDevice(pExt->DeviceObject);
		RtlFreeUnicodeString(&pExt->DeviceInterface);
		Irp->IoStatus.Status = STATUS_SUCCESS;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return STATUS_SUCCESS;

	case IRP_MN_QUERY_PNP_DEVICE_STATE:
		status = RegHackerForwardIrpSynchronous(DeviceObject, Irp);
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return status;
	}
	return RegHackerDefaultHandler(DeviceObject, Irp);
}
