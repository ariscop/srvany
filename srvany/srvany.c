#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0501
#define _WIN32_WINNT 0x0501

/* silence wcsncat warnings*/
#define _CRT_SECURE_NO_WARNINGS

#include <SDKDDKVer.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <Shlwapi.h>

#define debug(a) _debugprint(a)

void _debugprint(LPWSTR message)
{
    WCHAR buf[256];
    DWORD lastError = GetLastError();
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, 256, NULL);
    wprintf(L"%s: %d - %s", message, lastError, buf);
}

BOOL                    isService, installSvc, removeSvc;
HANDLE                  IOPort;
SERVICE_STATUS          Status;
SERVICE_STATUS_HANDLE   StatusHandle;
LPWSTR                  CommandLine;

#define cmdMax (32*1024)

BOOL readParam(LPWSTR service, LPWSTR param, DWORD type, LPBYTE out, LPDWORD size);

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

VOID ServiceSetup(DWORD argc, LPWSTR *argv)
{
    if(!isService)
        return;
    StatusHandle = RegisterServiceCtrlHandler(argv[0], SvcCtrlHandler);
    ReportStatus(SERVICE_START_PENDING);
}

VOID WINAPI ServiceMain(DWORD argc, LPWSTR *argv)
{
    BOOL ret;
    DWORD key, value, code;
    LPOVERLAPPED overlapped;
    HANDLE Job = INVALID_HANDLE_VALUE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limitInfo = {0};
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT portInfo = {0};
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION procinfo = {0};

    ServiceSetup(argc, argv);

restart:
    if(!(Job = CreateJobObject(NULL, NULL))) {
        debug(L"CreateJobObject failed");
        goto error;
    }

    if(isService) {
        DWORD size = cmdMax*sizeof(WCHAR);
        if(!(CommandLine = LocalAlloc(0, size)))
            goto error;
        if(!readParam(argv[0], L"Application", REG_SZ, (LPBYTE)CommandLine, &size))
            goto error;
    }

    limitInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &limitInfo, sizeof(limitInfo))) {
        debug(L"JobObjectExtendedLimitInformation failed");
        goto error;
    }
    portInfo.CompletionKey = Job;
    portInfo.CompletionPort = IOPort;
    if(!SetInformationJobObject(Job, JobObjectAssociateCompletionPortInformation, &portInfo, sizeof(portInfo))) {
        debug(L"JobObjectAssociateCompletionPortInformation failed");
        goto error;
    }

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
        debug(L"CreateProcess Failed");
        goto error;
    }
    if(!AssignProcessToJobObject(Job, procinfo.hProcess)) {
        debug(L"AssignProcessToJobObject Failed (service in job?)");
        TerminateProcess(procinfo.hProcess, 0);
        CloseHandle(procinfo.hProcess);
        CloseHandle(procinfo.hThread);
        goto error;
    }
    ResumeThread(procinfo.hThread);
    CloseHandle(procinfo.hThread);
    ReportStatus(SERVICE_RUNNING);
    while(GetQueuedCompletionStatus(IOPort, &value, &key, &overlapped, INFINITE)) {
        if(key == 1) {
            if(value == SERVICE_CONTROL_STOP)
                break;
        } else if (key == (DWORD)Job)
            switch(value)
        {
        case JOB_OBJECT_MSG_EXIT_PROCESS:
            if((DWORD)overlapped != procinfo.dwProcessId)
                continue;
            if(!GetExitCodeProcess(procinfo.hProcess, &code))
                debug(L"GetExitCodeProcess failed");
            debug(L"Main process exited, restarting");
            CloseHandle(procinfo.hProcess);
            CloseHandle(Job);
            goto restart;
        default:
            break;
        }
    }
    debug(L"stop requested");
error:
    ReportStatus(SERVICE_STOPPED);
    CloseHandle(IOPort);
    CloseHandle(Job);
}

wchar_t help_text[] = \
L"\tsrvany.exe [command line]\n" \
L"\tsrvany.exe run [command line]\n" \
L"\t    Run command, restart on exit\n\n" \
L"\tsrvany.exe install service_name /arg [command line]\n" \
L"\t    Install command as a service\n\n" \
L"\tsrvany.exe remove service_name\n" \
L"\t    Remove service\n\n" \
L"\tsrvany.exe list\n" \
L"\t    list installed srvany services\n\n" \
L"";

static BOOL cmdtok(LPCWSTR *pos, LPWSTR out, int size)
{
    int bcount = 0, qcount = 0;
    LPCWSTR in = *pos;
    BOOL cmd = FALSE;

    while (*in)
    {
        if((*in == ' ' || *in == '\t') && qcount == 0) {
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
    *out = 0;
    *pos = in;
    if(!cmd)
        SetLastError(ERROR_NO_MORE_ITEMS);
    return cmd;
}

/* Detect if we're launched outside of cmd by checking for other processes on our console */
BOOL isTerminal(void) {
    DWORD pid;
    return GetConsoleProcessList(&pid, 1) != 1;
}

HKEY getServicesKey(DWORD access)
{
    static HKEY services;
    static DWORD _access;
    DWORD status;
    if(!services && (_access & access) != access)
    {
        _access |= access;
        status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\services", 0, _access, &services);
        if(status) {
            SetLastError(status);
            services = NULL;
        }
    }
    return services;
}

HKEY getParamKey(LPWSTR service, DWORD access)
{
    HKEY hkservices, hkservice, hkparams;
    DWORD status;
    hkservices = getServicesKey(access);
    if(!hkservices)
        goto error;
    if(status = RegOpenKeyExW(hkservices, service, 0, access, &hkservice))
        goto error;
    if(access & KEY_CREATE_SUB_KEY)
        status = RegCreateKeyExW(hkservice, L"Parameters", 0, 0, 0, access, 0, &hkparams, NULL);
    else
        status = RegOpenKeyExW(hkservice, L"Parameters", 0, access, &hkparams);
    if(status)
        goto error;

    RegCloseKey(hkservice);
    return hkparams;

error:
    SetLastError(status);
    RegCloseKey(hkservice);
    return NULL;
}

BOOL readParam(LPWSTR service, LPWSTR param, DWORD type, LPBYTE out, LPDWORD size)
{
    DWORD _type, status;
    HKEY params = getParamKey(service, KEY_READ);
    if(!params)
        return FALSE;
    status = RegQueryValueExW(params, param, NULL, &_type, out, size);
    SetLastError(status);
    if(status || _type != type) {
        return FALSE;
    }
    return TRUE;
}

BOOL writeParam(LPWSTR service, LPWSTR param, DWORD type, LPBYTE value, DWORD size)
{
    DWORD status;
    HKEY params = getParamKey(service, KEY_WRITE);
    if(!params)
        return FALSE;
    status = RegSetValueExW(params, param, 0, type, value, size);
    SetLastError(status);
    return status == ERROR_SUCCESS;
}

void installService(LPWSTR name, LPWSTR command)
{
    SC_HANDLE Manager, service;
    WCHAR self[MAX_PATH] = {0};
    WCHAR out[MAX_PATH] = {0};
    WCHAR quote[2] = L"\"";
    /* ensure we can write the param key */
    if(!getServicesKey(KEY_WRITE))
        goto error;
    if(!(Manager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE)))
        goto error;

    GetModuleFileNameW(NULL, self, MAX_PATH);
    wcsncat(out, L"\"", MAX_PATH);
    wcsncat(out, self, MAX_PATH);
    wcsncat(out, L"\"", MAX_PATH);

    service = CreateServiceW(
        Manager,
        name,
        name,
        0,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        out,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL
    );
    if(!service)
        goto error;
    writeParam(name, L"Application", REG_SZ, (LPBYTE)command, wcslen(command)*sizeof(WCHAR));
    wprintf(L"Service installed successfully\n");
    return;

error:
    debug(L"Failed to install service");
    CloseServiceHandle(Manager);
}

void listService(void)
{
    SC_HANDLE Manager = NULL;
    BYTE buffer[256*1024];
    DWORD serviceCount, bytesNeeded, i;
    Manager = OpenSCManager(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if(!Manager)
        goto error;
  
    if(!EnumServicesStatusW(Manager,
                            SERVICE_WIN32_OWN_PROCESS,
                            SERVICE_STATE_ALL,
                            (LPENUM_SERVICE_STATUSW)buffer,
                            256*1024,
                            &bytesNeeded,
                            &serviceCount,
                            NULL))
    {
        goto error;
    }

    for(i = 0; i < serviceCount; i++)
    {
        DWORD cmdSize = cmdMax;
        WCHAR cmd[cmdMax];
        LPENUM_SERVICE_STATUSW service = &((LPENUM_SERVICE_STATUSW)buffer)[i];

        if(readParam(service->lpServiceName, L"Application", REG_SZ, (LPBYTE)cmd, &cmdSize))
            wprintf(L"%s - %.*s\n", service->lpServiceName, cmdSize, cmd);
    }
    goto done;

error:
    debug(L"Failed to enumerate services");
done:
    CloseServiceHandle(Manager);
    return;
};

void removeService(LPWSTR name)
{
    SC_HANDLE Manager, service = NULL;
    BYTE buf[cmdMax*sizeof(WCHAR)];
    DWORD size = cmdMax*sizeof(WCHAR), status;
    if(!(Manager = OpenSCManager(NULL, NULL, 0)))
        goto error;
    if(!readParam(name, L"Application", REG_SZ, buf, &size)) {
        debug(L"Not an srvany service");
        goto done;
    }
    if(!(service = OpenService(Manager, name, DELETE)))
        goto error;
    if(DeleteService(service)) {
        wprintf(L"Service removed successfully\n");
        goto done;
    }
error:
    debug(L"Failed to delete service");
done:
    CloseServiceHandle(service);
    CloseServiceHandle(Manager);
}

int wmain(int argc, WCHAR *argv[])
{
    SERVICE_TABLE_ENTRYW DispatchTable[] = {
        { L"", (LPSERVICE_MAIN_FUNCTIONW) ServiceMain },
        { NULL, NULL }
    };
    LPWSTR commandLine = GetCommandLineW();
    LPWSTR temp;
    LPWSTR out = (LPWSTR)LocalAlloc(0, sizeof(WCHAR)*2048);
    if(!out) {
        debug(L"Failed to allocate memory");
        return;
    }

    /* Hide terminal when launched from explorer */
    if(!isTerminal())
        FreeConsole();

    cmdtok(&commandLine, out, 2048); /* skip executable name */
    temp = commandLine;
    if(!cmdtok(&commandLine, out, 2048)) {
        wprintf(L"%s\n", help_text);
        isService = TRUE;
        goto startService;
    }

    if(StrCmpIW(out, L"install") == 0) {
        WCHAR name[256];
        cmdtok(&commandLine, name, 256);
        installService(name, commandLine);
        return;
    } else if(StrCmpIW(out, L"remove") == 0) {
        WCHAR name[256];
        cmdtok(&commandLine, name, 256);
        removeService(name);
        return;
    } else if(StrCmpIW(out, L"list") == 0) {
        listService();
        return;
    } else if(StrCmpIW(out, L"run") == 0) {
        commandLine = out;
    } else {
        commandLine = temp;
    }

    CommandLine = commandLine;

startService:

    if (!(IOPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1))) {
        debug(L"CreateIoCompletionPort failed");
        return;
    }

    Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;

    if(isService)
        StartServiceCtrlDispatcherW(DispatchTable);
    else
        ServiceMain(0, NULL);
}