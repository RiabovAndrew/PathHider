#include <fltKernel.h>

#include "PathHider.h"
#include "FileNameInformation.h"

extern LIST_ENTRY gFolderDataHead;
extern LIST_ENTRY FileFilter1;

int wcsrep(wchar_t* str, const wchar_t* match, const wchar_t* rep)
{
    if (!str || !match)
        return -1;
    wchar_t tmpstr[BUFFER_SIZE];
    wchar_t* pos = wcsstr(str, match);
    if (pos)
    {
        memset(tmpstr, 0, sizeof(tmpstr));
        wcsncpy(tmpstr, str, pos - str);
        wcscat(tmpstr, rep);
        wcscat(tmpstr, pos + wcslen(match));
        wcscpy(str, tmpstr);
        return 0;
    }
    return -1;
}

FolderData* GetFolderDataByFolderPath(PUNICODE_STRING FolderPath)
{
    FolderData* retVal = nullptr;
    PLIST_ENTRY temp = &gFolderDataHead;
    while (&gFolderDataHead != temp->Flink)
    {
        temp = temp->Flink;
        auto curFolderData = CONTAINING_RECORD(temp, FolderData, m_listEntry);
        if (RtlEqualUnicodeString(&curFolderData->m_path.GetUnicodeString(), FolderPath, TRUE))
        {
            retVal = curFolderData;
            break;
        }
    }
    return retVal;
}

bool FolderContainsFilesToHide(PFLT_CALLBACK_DATA Data,
                               PCFLT_RELATED_OBJECTS FltObjects)
{
    KUtils::FilterFileNameInformation info(Data);
    if (!info)
    {
        return false;
    }
    auto status = info.Parse();
    if (!NT_SUCCESS(status))
    {
        KdPrint(("Failed to parse file name info (0x%08X)\n", status));
        return false;
    }
    //KdPrint(("Folder path: %wZ\n", &info->Name));
    auto folderData = GetFolderDataByFolderPath(&info->Name);
    FolderContext* context;
    if (folderData)
    {
        status = FltAllocateContext(FltObjects->Filter, FLT_FILE_CONTEXT,
                                    sizeof(FolderContext), PagedPool,
                                    reinterpret_cast<PFLT_CONTEXT*>(&context));
        if (!NT_SUCCESS(status))
        {
            KdPrint(("Failed to allocate context (0x%08X)\n", status));
            return false;
        }
        RtlZeroMemory(context, sizeof(FolderContext));
        
        status =
            FltSetFileContext(FltObjects->Instance, FltObjects->FileObject,
                              FLT_SET_CONTEXT_KEEP_IF_EXISTS, context,
                              nullptr);
        if (status == STATUS_FLT_CONTEXT_ALREADY_DEFINED)
        {
            status = STATUS_SUCCESS;
        }
        else if (!NT_SUCCESS(status))
        {
            FltReleaseContext(context);
            return false;
        }
        else
        {
            context->m_fileListHead = KUtils::intrusive_ptr<FileList>(folderData->m_fileListHead);
        }
        FltReleaseContext(context);
    }
    return folderData;
}

extern "C" FLT_PREOP_CALLBACK_STATUS PathHiderPreCreate(_Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext)
{
    NTSTATUS status;

    FILEFILTERITEM fileFilterItem = { 0 };
    PLIST_ENTRY p = NULL;
    PFILEFILTERITEMINLIST pffil;

    PWCHAR current_path = { 0 };
    UNICODE_STRING uni_current_path = { 0 };
    PFILE_OBJECT FileObject;
    PFLT_FILE_NAME_INFORMATION nameInfo;

    BOOLEAN IsDirectory = FALSE;

    PUNICODE_PREFIX_TABLE_ENTRY PrefixTableEntry = NULL;

    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    status = FltGetFileNameInformation(Data, FLT_FILE_NAME_NORMALIZED, &nameInfo);
    if (NT_SUCCESS(status))
    {

        if (Data->Iopb->Parameters.Create.Options & FILE_DIRECTORY_FILE | FILE_NON_DIRECTORY_FILE)
        {
            // FltParseFileNameInformation(&nameInfo);
            if (nameInfo->Stream.Length == 0)
            {
                current_path = (PWSTR)ExAllocatePool(PagedPool, BUFFER_SIZE);
                RtlZeroMemory(current_path, BUFFER_SIZE);
                RtlCopyMemory(current_path, nameInfo->Name.Buffer, nameInfo->Name.Length);
                for (p = FileFilter1.Blink; p != &FileFilter1; p = p->Blink)
                {
                    pffil = CONTAINING_RECORD(p, FILEFILTERITEMINLIST, listEntry);
                    if (wcsstr(current_path, pffil->filter.sourcePath) != NULL)
                    {

                        wcsrep(current_path, pffil->filter.sourcePath, pffil->filter.redirectPath);
                        RtlInitUnicodeString(&uni_current_path, current_path);
                        FileObject = Data->Iopb->TargetFileObject;
                        FileObject->FileName = uni_current_path;
                        Data->IoStatus.Information = IO_REPARSE;
                        Data->IoStatus.Status = STATUS_REPARSE;

                        FltReleaseFileNameInformation(nameInfo);
                        return FLT_PREOP_COMPLETE;
                    }
                }
            }
        }

        FltReleaseFileNameInformation(nameInfo);
    }

    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}


extern "C" FLT_POSTOP_CALLBACK_STATUS
PathHiderPostCreate(PFLT_CALLBACK_DATA Data,
                    PCFLT_RELATED_OBJECTS FltObjects,
                    PVOID CompletionContext,
                    FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(CompletionContext);

    if (Flags & FLTFL_POST_OPERATION_DRAINING)
        return FLT_POSTOP_FINISHED_PROCESSING;

    // TODO remove this check after testing and bugfix is done - it is just
    // temporary to reduce the number of calls
    /*if ((Data->RequestorMode == KernelMode) ||
    !IsControlledProcess(PsGetCurrentProcess()))
    {
            return FLT_POSTOP_FINISHED_PROCESSING;
    }*/
    if (Data->IoStatus.Information == FILE_DOES_NOT_EXIST || Data->IoStatus.Information == FILE_SUPERSEDED)
    {
        //a new file - skip
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    
    FILE_BASIC_INFORMATION info;
    auto status = FltQueryInformationFile(FltObjects->Instance, FltObjects->FileObject, &info, sizeof(info), FileBasicInformation, nullptr);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("Failed to get file info (0x%08X)\n", status));
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    // is it folder?
    if (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        FolderContainsFilesToHide(Data, FltObjects);
    }
    return FLT_POSTOP_FINISHED_PROCESSING;
}

extern "C" FLT_POSTOP_CALLBACK_STATUS
PathHiderPostCleanup(_Inout_ PFLT_CALLBACK_DATA Data,
                     _In_ PCFLT_RELATED_OBJECTS FltObjects,
                     _In_ PVOID CompletionContext,
                     _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(CompletionContext);

    if (Flags & FLTFL_POST_OPERATION_DRAINING)
        return FLT_POSTOP_FINISHED_PROCESSING;
    FolderContext* context = nullptr;
    auto status = FltGetFileContext(FltObjects->Instance, FltObjects->FileObject,
                               reinterpret_cast<PFLT_CONTEXT*>(&context));
    if (status == STATUS_NOT_FOUND || status == STATUS_NOT_SUPPORTED)
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    if (!NT_SUCCESS(status))
    {
        KdPrint(("Failed to get file context (0x%08X)\n", status));
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    context->~FolderContext(); 
    FltReleaseContext(context);
    FltDeleteContext(context);
    return FLT_POSTOP_FINISHED_PROCESSING;
}
