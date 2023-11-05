#include "PathHider.h"

#include "FileNameInformation.h"

bool IsControlledProcess(const PEPROCESS Process)
{
    bool currentProcess = PsGetCurrentProcess() == Process;
    HANDLE hProcess;
    if (currentProcess)
        hProcess = NtCurrentProcess();
    else
    {
        auto status = ObOpenObjectByPointer(Process, OBJ_KERNEL_HANDLE, nullptr,
                                            0, nullptr, KernelMode, &hProcess);
        if (!NT_SUCCESS(status))
            return true;
    }

    auto size = 300;
    bool controlledProcess = false;
    auto processName =
        (UNICODE_STRING*)ExAllocatePoolWithTag(PagedPool, size, DRIVER_TAG);

    if (processName)
    {
        RtlZeroMemory(processName,
                      size); // ensure string will be NULL-terminated
        auto status = ZwQueryInformationProcess(hProcess, ProcessImageFileName,
                                                processName,
                                                size - sizeof(WCHAR), nullptr);

        if (NT_SUCCESS(status))
        {
            KdPrint(("The operation from %wZ\n", processName));

            if (processName->Length > 0 &&
                wcsstr(processName->Buffer, L"\\Windows\\explorer.exe") !=
                    nullptr)
            {
                controlledProcess = true;
            }
        }
        ExFreePool(processName);
    }
    if (!currentProcess)
        ZwClose(hProcess);

    return controlledProcess;
}

bool IsRelevantFileInfoQuery(PFLT_CALLBACK_DATA Data)
{
    bool relevantQuery = false;

    switch (Data->Iopb->Parameters.DirectoryControl.QueryDirectory
                .FileInformationClass)
    {
    case FileIdFullDirectoryInformation:
    case FileIdBothDirectoryInformation:
    case FileBothDirectoryInformation:
    case FileDirectoryInformation:
    case FileFullDirectoryInformation:
    case FileNamesInformation:
        relevantQuery = true;
    }

    return relevantQuery;
}

bool FileObjectToHide(PFLT_CALLBACK_DATA Data,
                      PUNICODE_STRING Name, KUtils::intrusive_ptr<FileList>& FolderDataHead)
{
    KUtils::FilterFileNameInformation info(Data);
    auto status = info.Parse();
    if (!NT_SUCCESS(status))
    {
        KdPrint(("Failed to parse file name info (0x%08X)\n", status));
        return false;
    }
    KUtils::intrusive_ptr<FileList> temp = KUtils::intrusive_ptr<FileList>(FolderDataHead);
    while (temp != nullptr)
    {
        if (RtlEqualUnicodeString(&temp->m_name.GetUnicodeString(), Name, TRUE))
        {
            return true;
        }
        temp = KUtils::intrusive_ptr<FileList>(temp->m_next);
    }
    return false;
}

bool ContextIsPresent(PCFLT_RELATED_OBJECTS FltObjects)
{
    FolderContext* context = nullptr;
    auto status =
        FltGetFileContext(FltObjects->Instance, FltObjects->FileObject,
                          reinterpret_cast<PFLT_CONTEXT*>(&context));
    if (status == STATUS_NOT_FOUND || status == STATUS_NOT_SUPPORTED)
    {
        return false;
    }
    if (!NT_SUCCESS(status))
    {
        KdPrint(("Failed to get file context (0x%08X)\n", status));
        return false;
    }
    FltReleaseContext(context);
    return true;
}
bool ShouldHandleRequest(PFLT_CALLBACK_DATA Data,
                         PCFLT_RELATED_OBJECTS FltObjects)
{
    
    bool retValue = (Data->Iopb->MinorFunction == IRP_MN_QUERY_DIRECTORY) &&
                    IsRelevantFileInfoQuery(Data) && ContextIsPresent(FltObjects);
    return retValue;
}

template <class T> 
bool AddressIsValid(const void* ValidBufferStart, const void* ValidBufferEnd, T* AddressToCheck) 
{    
    return (reinterpret_cast<const void*>(AddressToCheck) >= ValidBufferStart &&
                   reinterpret_cast<const void*>(AddressToCheck) <= reinterpret_cast<const char*>(ValidBufferEnd) - sizeof(T));
}

template <class T>
PVOID AddDirEntry(_In_ T* LastFileDirInfo, _In_ ULONG EntryOffsetInBufferDir, _In_ ULONG BufferDirSize, _Out_ PBOOLEAN Copied)
{
    PAGED_CODE();

    ULONG entrySize = sizeof(T) + LastFileDirInfo->FileNameLength - 1;

    if (BufferDirSize - (static_cast<unsigned long long>(EntryOffsetInBufferDir) + entrySize) >= entrySize)
    {

        //
        //  There is enough room for this element, so copy it.
        //
        T* last2 = reinterpret_cast<T*>(reinterpret_cast<char*>(LastFileDirInfo) + entrySize);
        RtlCopyMemory(last2, LastFileDirInfo, entrySize);
        LastFileDirInfo->NextEntryOffset = entrySize;

        WCHAR* nameStart = reinterpret_cast<WCHAR*>(last2->FileName);
        *nameStart = 33;
        *(nameStart + 1) = 100;
        *(nameStart + 2) = 105;
        *(nameStart + 3) = 114;
        *(nameStart + 4) = 46;
        *(nameStart + 5) = 116;
        *(nameStart + 6) = 120;
        *(nameStart + 7) = 116;

        last2->FileNameLength = 16;
        last2->FileAttributes = 8224;

        *Copied = TRUE;
    }
    else
    {
        *Copied = FALSE;
    }

    return NULL;
}

NTSTATUS CreateOrOpenFile(_In_ PFLT_CALLBACK_DATA Data, _In_ PCFLT_RELATED_OBJECTS FltObjects, _Out_ PHANDLE FileHandle, PFILE_OBJECT* FileObject)
{
    UNREFERENCED_PARAMETER(Data);

    NTSTATUS status;
    OBJECT_ATTRIBUTES objAttributes;
    OBJECT_ATTRIBUTES objAttributes2;
    IO_STATUS_BLOCK ioStatusBlock;
    UNICODE_STRING targetFileName;
    UNICODE_STRING targetFolderName;
    PFILE_OBJECT fileObject;
    PHANDLE fileHandle = NULL;

    const PWSTR basePath = L"\\??\\C:\\dirinfofolder";
    const PWSTR file = L"\\!dir.txt";

    PWSTR createFilePath = (PWSTR)ExAllocatePool(PagedPool, BUFFER_SIZE);
    RtlZeroMemory(createFilePath, BUFFER_SIZE);
    RtlCopyMemory(createFilePath, basePath, 20 * 2);
    RtlCopyMemory(((char*)createFilePath) + 20 * 2, Data->Iopb->TargetFileObject->FileName.Buffer, Data->Iopb->TargetFileObject->FileName.Length);

    RtlInitUnicodeString(&targetFolderName, createFilePath);

    // Initialize object attributes for the target file
    InitializeObjectAttributes(&objAttributes, &targetFolderName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    //status = FltCreateFileEx(FltObjects->Filter,                     // Filter pointer
    //                         FltObjects->Instance,                   // Filter instance
    //                         fileHandle,           // Output file handle
    //                         &fileObject,                            
    //                         FILE_LIST_DIRECTORY | SYNCHRONIZE,
    //                         &objAttributes,                         // Object attributes
    //                         &ioStatusBlock,                         // I/O status block
    //                         NULL,                                   // Allocation size (optional)
    //                         FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL, // File attributes
    //                         0,                                      // Share access
    //                         FILE_OVERWRITE_IF,                      // Create disposition (use FILE_OVERWRITE_IF for overwrite)
    //                         FILE_DIRECTORY_FILE,                // Create options
    //                         NULL,                                   // EA buffer (optional)
    //                         0,                                      // EA buffer size
    //                         IO_IGNORE_SHARE_ACCESS_CHECK            // Flags
    //);

    //if (!NT_SUCCESS(status))
    //{
    //    KdPrint(("FltCreateFileEx failed with status 0x%X\n", status));
    //    return status;
    //}

    status = ZwCreateFile(NULL,                     // File handle (not needed for directory creation)
                          FILE_GENERIC_WRITE,       // Desired access
                          &objAttributes,           // Object attributes
                          &ioStatusBlock,           // I/O status block
                          NULL,                     // Allocation size (optional)
                          FILE_ATTRIBUTE_DIRECTORY, // File attributes (for directory)
                          0,                        // Share access
                          FILE_CREATE,              // Create disposition
                          FILE_DIRECTORY_FILE,      // Create options (treat it as a directory)
                          NULL,                     // EA buffer (optional)
                          0                         // EA buffer size
    );

    if (!NT_SUCCESS(status))
    {
        KdPrint(("ZwCreateFile for directory creation failed with status 0x%X\n", status));
        //return status;
    }

    RtlCopyMemory(((char*)createFilePath) + 20 * 2 + Data->Iopb->TargetFileObject->FileName.Length, file, 10 * 2);
    RtlInitUnicodeString(&targetFileName, createFilePath);

        InitializeObjectAttributes(&objAttributes2, &targetFileName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = FltCreateFileEx(FltObjects->Filter,   // Filter pointer
                             FltObjects->Instance, // Filter instance
                             FileHandle,           // Output file handle
                             FileObject, FILE_GENERIC_WRITE | FILE_GENERIC_READ,
                             &objAttributes2,              // Object attributes
                             &ioStatusBlock,              // I/O status block
                             NULL,                        // Allocation size (optional)
                             FILE_ATTRIBUTE_NORMAL,       // File attributes
                             0,                           // Share access
                             FILE_OVERWRITE_IF,           // Create disposition (use FILE_OVERWRITE_IF for overwrite)
                             FILE_NON_DIRECTORY_FILE,     // Create options
                             NULL,                        // EA buffer (optional)
                             0,                           // EA buffer size
                             IO_IGNORE_SHARE_ACCESS_CHECK // Flags
    );

    if (!NT_SUCCESS(status))
    {
        KdPrint(("FltCreateFileEx failed with status 0x%X\n", status));
        return status;
    }

    return status;
}

template <class T>
void HideFiles(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects)
{
    T* fileDirInfo = nullptr;
    T* nextFileDirInfo = nullptr;
    if (Data->Iopb->Parameters.DirectoryControl.QueryDirectory.Length <= 0)
    {
        KdPrint(("Buffer size is <= 0 \n"));
        return;
    }
    if (Data->Iopb->Parameters.DirectoryControl.QueryDirectory.MdlAddress != NULL)
    {
        fileDirInfo = static_cast<T*>(MmGetSystemAddressForMdlSafe(Data->Iopb->Parameters.DirectoryControl.QueryDirectory.MdlAddress, NormalPagePriority));
    }
    else
    {
        fileDirInfo = static_cast<T*>(Data->Iopb->Parameters.DirectoryControl.QueryDirectory.DirectoryBuffer);
    }
    if (!fileDirInfo)
    {
        KdPrint(("Null buffer \n"));
        return;
    }
    const void* ValidBufferStart = fileDirInfo;
    ULONG ValidBufferSize = Data->Iopb->Parameters.DirectoryControl.QueryDirectory.Length;
    const void* ValidBufferEnd = reinterpret_cast<const char*>(ValidBufferStart) + Data->Iopb->Parameters.DirectoryControl.QueryDirectory.Length;
    
    FolderContext* context = nullptr;
    auto status = FltGetFileContext(FltObjects->Instance, FltObjects->FileObject,
                               reinterpret_cast<PFLT_CONTEXT*>(&context));
    if (!NT_SUCCESS(status))
    {
        KdPrint(("Failed to get file context (0x%08X)\n", status));
        return;
    }
    BOOLEAN singleEntry = BooleanFlagOn(Data->Iopb->OperationFlags, SL_RETURN_SINGLE_ENTRY);
    if (singleEntry)
    {
        // I'm not gonna do anything here for now
    }
    else
    {
        __try
        {
            // Never Trust Buffers
            ProbeForWrite(fileDirInfo, Data->Iopb->Parameters.DirectoryControl.QueryDirectory.Length, 1);

            if (!AddressIsValid<T>(ValidBufferStart, ValidBufferEnd, fileDirInfo))
                __leave;

            if (fileDirInfo->NextEntryOffset != 0) // This is the first
                                                    // entry
            {
                ULONG fileDirInfoBufferOffset = 0;
                nextFileDirInfo = (T*)((PUCHAR)fileDirInfo + fileDirInfo->NextEntryOffset);
                fileDirInfoBufferOffset += fileDirInfo->NextEntryOffset;
                if (!AddressIsValid<T>(ValidBufferStart, ValidBufferEnd, nextFileDirInfo))
                    __leave;

                if (nextFileDirInfo->NextEntryOffset == 0)
                {
                    FltReleaseContext(context);
                    return;
                }

                PVOID myBufferWithDirInfos = ExAllocatePool(NonPagedPool, 512);
                RtlZeroMemory(myBufferWithDirInfos, 512);
                int offset = 0;

                while (nextFileDirInfo->NextEntryOffset != 0)
                {
                    nextFileDirInfo = (T*)((PUCHAR)nextFileDirInfo + nextFileDirInfo->NextEntryOffset);
                    RtlCopyMemory((PVOID)((char*)myBufferWithDirInfos + offset), nextFileDirInfo->FileName,
                                  (ULONG)nextFileDirInfo->FileNameLength);
                    offset += nextFileDirInfo->FileNameLength;

                    unsigned short ch = 10;
                    RtlCopyMemory((PVOID)((char*)myBufferWithDirInfos + offset), &ch,
                                  1);
                    offset += 1;

                    fileDirInfoBufferOffset += fileDirInfo->NextEntryOffset;
                    if (!AddressIsValid<T>(ValidBufferStart, ValidBufferEnd, nextFileDirInfo))
                        __leave;
                }

                BOOLEAN isCopied = false;
                AddDirEntry(nextFileDirInfo, fileDirInfoBufferOffset, ValidBufferSize,
                                          &isCopied);

                if (isCopied)
                {
                    HANDLE fileHandle;
                    PFILE_OBJECT fileObject = NULL;
                    CreateOrOpenFile(Data, FltObjects, &fileHandle, &fileObject);

                    if (fileObject != NULL)
                    {
                        ULONG written;
                        LARGE_INTEGER li{};

                        status = FltWriteFile(FltObjects->Instance,           // Filter instance
                                              fileObject,           // File object
                                              &li,                  // File offset (where to write)
                                              512,         // Length of data to write
                                              myBufferWithDirInfos, // Data buffer
                                              0,              // Flags (0 for synchronous write)
                                              &written,             // I/O status block
                                              NULL,           // Completion routine
                                              NULL            // Context
                        );

                        if (!NT_SUCCESS(status))
                        {
                            KdPrint(("FltWriteFile failed with status 0x%X\n", status));
                        }



                        NTSTATUS st = FltClose(fileHandle);
                        if (!NT_SUCCESS(st))
                        {
                            KdPrint(("FltClose failed with status 0x%X\n", st));
                        }

                        ExFreePool(myBufferWithDirInfos);
                    }
                }
            }
            else // This is the first and last entry, so there's nothing to
                    // return
            {
                FltReleaseContext(context);
                Data->IoStatus.Status = STATUS_NO_MORE_ENTRIES;
                return;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            status = GetExceptionCode();
            KdPrint(("An exception occurred at " __FUNCTION__ " status = (0x%08X)\n", status));
        }

    }
    if (context)
        FltReleaseContext(context);
}

void ProcessFileInfoQuery(PFLT_CALLBACK_DATA Data,
                          PCFLT_RELATED_OBJECTS FltObjects)
{
    switch (Data->Iopb->Parameters.DirectoryControl.QueryDirectory
                .FileInformationClass)
    {
    case FileDirectoryInformation:
        HideFiles<FILE_DIRECTORY_INFORMATION>(Data, FltObjects);
        break;
    case FileFullDirectoryInformation:
        HideFiles<FILE_FULL_DIR_INFORMATION>(Data, FltObjects);
        break;
    case FileBothDirectoryInformation:
        HideFiles<FILE_BOTH_DIR_INFORMATION>(Data, FltObjects);
        break;
    case FileIdBothDirectoryInformation:
        HideFiles<FILE_ID_BOTH_DIR_INFORMATION>(Data, FltObjects);
        break;
    case FileIdFullDirectoryInformation:
        HideFiles<FILE_ID_FULL_DIR_INFORMATION>(Data, FltObjects);
        break;
    case FileNamesInformation:
    default:
        NT_ASSERT(FALSE);
    }
}

/*************************************************************************
        MiniFilter callback routines.
*************************************************************************/

extern "C" FLT_PREOP_CALLBACK_STATUS PathHiderPreDirectoryControl(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext)
{
    UNREFERENCED_PARAMETER(CompletionContext);
    
    if (ShouldHandleRequest(Data, FltObjects))
    {
        return FLT_PREOP_SUCCESS_WITH_CALLBACK;
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}


extern "C" FLT_POSTOP_CALLBACK_STATUS
PathHiderPostDirectoryControl(_Inout_ PFLT_CALLBACK_DATA Data,
                              _In_ PCFLT_RELATED_OBJECTS FltObjects,
                              _In_ PVOID CompletionContext,
                              _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(CompletionContext);

    if (NT_SUCCESS(Data->IoStatus.Status))
    {
        ProcessFileInfoQuery(Data, FltObjects);
    }
    return FLT_POSTOP_FINISHED_PROCESSING;
}