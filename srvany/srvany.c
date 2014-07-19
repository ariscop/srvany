#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0500
#define _WIN32_WINNT 0x0500

#include <SDKDDKVer.h>
#include <windows.h>
#include <stdio.h>
#include <Shlwapi.h>

#define debug(a, b) printf("%d:%d %s\n", __LINE__, a, b)

BOOL                    isService, installSvc, removeSvc;
HANDLE                  IOPort;
SERVICE_STATUS          Status;
SERVICE_STATUS_HANDLE   StatusHandle;
LPWSTR                  CommandLine;

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

VOID ServiceSetup(DWORD argc, LPCSTR *argv)
{
    if(!isService)
        return;
    StatusHandle = RegisterServiceCtrlHandler(argv[0], SvcCtrlHandler);
    ReportStatus(SERVICE_START_PENDING);
}

VOID WINAPI ServiceMain(DWORD argc, LPCSTR *argv)
{
    BOOL ret;
    DWORD key, value;
    LPOVERLAPPED overlapped;
    HANDLE Job = INVALID_HANDLE_VALUE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limitInfo = {0};
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT portInfo = {0};
    STARTUPINFOW startup = {0};
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
    ret = CreateProcessW(
        NULL,       //lpApplicationName
        CommandLine,//lpCommandLine
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

wchar_t help_text[] = \
L"\tsrvany.exe [command line]\n" \
L"\t    Run command, restart on exit\n\n" \
L"\tsrvany.exe install service_name /arg [command line]\n" \
L"\t    Install command as a service\n\n" \
L"\tsrvany.exe remove service_name\n" \
L"\t    Remove service\n\n" \
L"\tsrvany.exe service\n" \
L"\t    Internal, do not use\n\n" \
L"";

static BOOL cmdtok(LPCWSTR *pos, LPWSTR out, int size)
{
    int bcount = 0, qcount = 0;
    LPCWSTR in = *pos;
    BOOL cmd = FALSE;

    while (*in)
    {
        if((*in == ' ' || *in == '\t') && qcount == 0) {
            *out = 0;
            *in++;
            if(cmd) {
                /* skip to begining of next argument */
                while((*in == ' ' || *in == '\t'))
                    in++;
                break;
            }
        } else if(*in == '"') {
            qcount++;
            qcount &= 1;
            in++;
        } else {
            /* append character */
            *out++ = *in++;
            cmd = TRUE;
        }
    }
    *pos = in;
    if(!cmd)
        SetLastError(ERROR_NO_MORE_ITEMS);
    return cmd;
}


int wmain(int argc, WCHAR *argv[])
{
    SERVICE_TABLE_ENTRY DispatchTable[] = {
        { TEXT(""), (LPSERVICE_MAIN_FUNCTION) ServiceMain },
        { NULL, NULL }
    };
    LPWSTR commandLine = GetCommandLineW();
    LPWSTR temp;
    WCHAR out[2048] = {0};

    cmdtok(&commandLine, out, 2048); /* skip executable name */
    temp = commandLine;
    if(!cmdtok(&commandLine, out, 2048)) {
        wprintf(L"%s\n", help_text);
        return;
    }

    if(       StrCmpIW(out, L"service") == 0) {
        isService = TRUE;
    } else if(StrCmpIW(out, L"install") == 0) {
        debug(0, "Not implemented");
        return;
    } else if(StrCmpIW(out, L"remove") == 0) {
        debug(0, "Not implemented");
        return;
    } else {
        commandLine = temp;
    }

    if (!(IOPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1))) {
        debug(GetLastError(), "CreateIoCompletionPort failed");
        return;
    }

    CommandLine = commandLine;

    Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;

    if(isService)
        StartServiceCtrlDispatcher(DispatchTable);
    else
        ServiceMain(0, NULL);
}