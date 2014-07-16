#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define debug(a, b) printf("%d:%d %s\n", __LINE__, a, b)

BOOL                    isService;
HANDLE                  IOPort;
SERVICE_STATUS          Status;
SERVICE_STATUS_HANDLE   StatusHandle;

VOID ReportStatus(DWORD status)
{
    static DWORD checkPoint = 0;
    if(!isService)
        return;

    Status.dwCurrentState = status;

    if ( status == SERVICE_START_PENDING)
         Status.dwControlsAccepted = 0;
    else Status.dwControlsAccepted = SERVICE_ACCEPT_STOP;

    if ((status == SERVICE_RUNNING) || (status == SERVICE_STOPPED) )
        checkPoint = 0;
    Status.dwCheckPoint = checkPoint++;

    SetServiceStatus(StatusHandle, &Status);
}

VOID WINAPI SvcCtrlHandler(DWORD dwCtrl)
{
    switch(dwCtrl)
    {
    case SERVICE_CONTROL_STOP:
        ReportStatus(SERVICE_STOP_PENDING);
    default:
        PostQueuedCompletionStatus(IOPort, dwCtrl, 0x1, NULL);
    }
}

VOID ServiceSetup(DWORD argc, LPTSTR *argv)
{
    if(!isService)
        return;
    StatusHandle = RegisterServiceCtrlHandler(argv[0], SvcCtrlHandler);
    ReportStatus(SERVICE_START_PENDING);
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv)
{
    BOOL ret;
    DWORD key, value;
    LPOVERLAPPED overlapped;
    HANDLE Job = INVALID_HANDLE_VALUE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limitInfo = {0};
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT portInfo = {0};
    STARTUPINFO startup = {0};
    PROCESS_INFORMATION procinfo = {0};

    ServiceSetup(argc, argv);

    if(!(Job = CreateJobObject(NULL, NULL))) {
        debug(GetLastError(), "CreateJobObject failed");
        goto error;
    }

    limitInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &limitInfo, sizeof(limitInfo))) {
        debug(GetLastError(), "JobObjectExtendedLimitInformation failed");
        goto error;
    }
    portInfo.CompletionKey = Job;
    portInfo.CompletionPort = IOPort;
    if(!SetInformationJobObject(Job, JobObjectAssociateCompletionPortInformation, &portInfo, sizeof(portInfo))) {
        debug(GetLastError(), "JobObjectAssociateCompletionPortInformation failed");
        goto error;
    }

restart:    

    ret = CreateProcess(
        NULL,       //lpApplicationName
        TEXT("C:\\Windows\\notepad.exe"),   //lpCommandLine
        NULL,       //lpProcessAttributes
        NULL,       //lpThreadAttributes
        FALSE,      //bInheritHandles
        CREATE_SUSPENDED,   //dwCreationFlags
        NULL,       //lpEnvironment
        NULL,       //lpCurrentDirectory
        &startup,   //lpStartupInfo
        &procinfo   //lpProcessInformation
    );
    if(!ret) {
        debug(GetLastError(), "CreateProcess Failed");
        goto error;
    }
    if(!AssignProcessToJobObject(Job, procinfo.hProcess)) {
        debug(GetLastError(), "AssignProcessToJobObject Failed (service in job?)");
        TerminateProcess(procinfo.hProcess, 0);
        CloseHandle(procinfo.hProcess);
        CloseHandle(procinfo.hThread);
        goto error;
    }
    ResumeThread(procinfo.hThread);
    CloseHandle(procinfo.hProcess);
    CloseHandle(procinfo.hThread);
    ReportStatus(SERVICE_RUNNING);
    while(GetQueuedCompletionStatus(IOPort, &value, &key, &overlapped, INFINITE)) {
        if(key == 1) {
            if(value == SERVICE_CONTROL_STOP)
                break;
        } else
            switch(value)
        {
        case JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO:
            debug(0, "JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO, restarting");
            goto restart;
        default:
            break;
        }
    }
    debug(0, "stop requested");
error:
    ReportStatus(SERVICE_STOPPED);
    CloseHandle(IOPort);
    CloseHandle(Job);
}

int wmain(int argc, wchar_t *argv[])
{
    SERVICE_TABLE_ENTRY DispatchTable[] =
    {
        { TEXT(""), (LPSERVICE_MAIN_FUNCTION) ServiceMain },
        { NULL, NULL }
    };

    isService = TRUE;

    if (!(IOPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1))) {
        debug(GetLastError(), "CreateIoCompletionPort failed");
        return;
    }

    Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;

    if(isService)
        StartServiceCtrlDispatcher(DispatchTable);
    else
        ServiceMain(0, NULL);
}