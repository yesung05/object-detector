/* Per-user tray host. Only this host's job children are stopped on exit. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>
#ifndef HUNIK_VERSION
#define HUNIK_VERSION "0.1.0-alpha.1"
#endif
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
#define APP_TITLE L"unmanned_detector " WIDEN(HUNIK_VERSION)
#define TRAY_MESSAGE (WM_APP+1)
#define ID_DASH 1001
#define ID_SHOW 1002
#define ID_HIDE 1003
#define ID_EXIT 1004
#define ID_LOGS 1005
static HWND window,status;
static HANDLE mutex,job,process,stop_event;
static NOTIFYICONDATAW icon;
static WCHAR root[1024],data[1024],stop_path[1100],alert_path[1100];
static FILETIME alert_seen;
static UINT taskbar_created;
static int stopping=0,dash_port=8080,stream_port=8081,start_hidden=0;
static ULONGLONG stop_at;
#define RUNNING_TEXT L"실행 관리자가 동작 중입니다.\n카메라·감지 상태는 대시보드에서 확인하세요."
/* The dashboard uses ES2020 syntax that legacy Edge (1809) and IE11 (LTSC) cannot parse,
   so a Chromium browser is preferred over the default one. App Paths resolves the bare
   exe names; SEE_MASK_FLAG_NO_UI keeps a missing browser from raising an error dialog. */
static void open_dashboard(void){
    static const WCHAR *browsers[]={L"msedge.exe",L"chrome.exe",NULL};
    WCHAR url[80];SHELLEXECUTEINFOW sei={sizeof(sei)};
    _snwprintf_s(url,80,_TRUNCATE,L"http://localhost:%d",dash_port);
    sei.fMask=SEE_MASK_FLAG_NO_UI;sei.lpVerb=L"open";sei.nShow=SW_SHOWNORMAL;
    for(int i=0;;i++){
        if(browsers[i]){sei.lpFile=browsers[i];sei.lpParameters=url;}else{sei.lpFile=url;sei.lpParameters=NULL;}
        if(ShellExecuteExW(&sei)||!browsers[i])return;
    }
}
static void add_icon(void){Shell_NotifyIconW(NIM_ADD,&icon);}
static void show_window(void){ShowWindow(window,SW_SHOW);SetForegroundWindow(window);}
/* run-all.ps1 writes logs\ALERT.txt (UTF-16LE with BOM) when the detector cannot stay up
   and deletes it after a healthy run. A new write time = new alert, shown once. */
static int check_alert(int notify){
    WIN32_FILE_ATTRIBUTE_DATA a;WCHAR text[512]={0};const WCHAR *msg=text;HANDLE f;DWORD n=0;
    if(!GetFileAttributesExW(alert_path,GetFileExInfoStandard,&a)){
        if(alert_seen.dwLowDateTime||alert_seen.dwHighDateTime){
            ZeroMemory(&alert_seen,sizeof(alert_seen));
            if(process&&!stopping){SetWindowTextW(status,RUNNING_TEXT);wcscpy_s(icon.szTip,128,APP_TITLE);Shell_NotifyIconW(NIM_MODIFY,&icon);}
        }
        return 0;
    }
    if(notify&&!CompareFileTime(&a.ftLastWriteTime,&alert_seen))return 1;
    f=CreateFileW(alert_path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(f==INVALID_HANDLE_VALUE)return 0;
    ReadFile(f,text,sizeof(text)-sizeof(WCHAR),&n,NULL);CloseHandle(f);
    alert_seen=a.ftLastWriteTime;
    if(*msg==0xFEFF)msg++;
    SetWindowTextW(status,msg);
    wcscpy_s(icon.szTip,128,L"unmanned_detector - 확인 필요");
    if(notify){
        icon.uFlags|=NIF_INFO;icon.dwInfoFlags=NIIF_WARNING;
        wcscpy_s(icon.szInfoTitle,64,L"unmanned_detector 확인 필요");wcsncpy_s(icon.szInfo,256,msg,_TRUNCATE);
    }
    Shell_NotifyIconW(NIM_MODIFY,&icon);icon.uFlags&=~NIF_INFO;
    show_window();return 1;
}
static void hide_window(void){ShowWindow(window,SW_HIDE);}
static void exit_app(void){
    HANDLE f;
    if(stopping)return;
    stopping=1;stop_at=GetTickCount64();
    SetWindowTextW(status,L"감시를 종료하고 기록을 정리하고 있습니다...");
    f=CreateFileW(stop_path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f!=INVALID_HANDLE_VALUE)CloseHandle(f);
    if(stop_event)SetEvent(stop_event);
    if(!process)DestroyWindow(window);
}
static int port_busy(int port){
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);struct sockaddr_in a={0};int busy;
    if(s==INVALID_SOCKET)return 1;
    a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons((u_short)port);
    busy=connect(s,(struct sockaddr*)&a,sizeof(a))==0;closesocket(s);return busy;
}
static int start_worker(void){
    WCHAR system[1024],powershell[1200],command[4096],event_name[100],logs[1100],logfile[1200];
    STARTUPINFOW si={0};PROCESS_INFORMATION pi={0};JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit={0};
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};HANDLE output,input;SYSTEMTIME st;
    if(port_busy(dash_port)||port_busy(stream_port)){
        MessageBoxW(window,L"대시보드 또는 스트림 포트가 사용 중입니다. 기존 unmanned_detector 실행본을 종료한 뒤 다시 실행하세요.",APP_TITLE,MB_ICONWARNING);return 0;
    }
    _snwprintf_s(logs,1100,_TRUNCATE,L"%s\\logs",data);SHCreateDirectoryExW(NULL,logs,NULL);
    _snwprintf_s(stop_path,1100,_TRUNCATE,L"%s\\STOP",logs);DeleteFileW(stop_path);
    _snwprintf_s(alert_path,1100,_TRUNCATE,L"%s\\ALERT.txt",logs);DeleteFileW(alert_path);
    GetLocalTime(&st);
    _snwprintf_s(logfile,1200,_TRUNCATE,L"%s\\launcher-%04u%02u%02u-%02u%02u%02u.log",logs,st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    output=CreateFileW(logfile,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);
    if(output==INVALID_HANDLE_VALUE||input==INVALID_HANDLE_VALUE){if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);return 0;}
    _snwprintf_s(event_name,100,_TRUNCATE,L"Local\\HUNIK.Stop.%lu",GetCurrentProcessId());
    stop_event=CreateEventW(NULL,TRUE,FALSE,event_name);
    SetEnvironmentVariableW(L"HUNIK_STOP_EVENT",event_name);
    GetSystemDirectoryW(system,1024);
    _snwprintf_s(powershell,1200,_TRUNCATE,L"%s\\WindowsPowerShell\\v1.0\\powershell.exe",system);
    _snwprintf_s(command,4096,_TRUNCATE,L"\"%s\" -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -File \"%s\\run-all.ps1\" -DataDir \"%s\" -Background -OpenDashboard -DashboardPort %d -StreamPort %d",powershell,root,data,dash_port,stream_port);
    job=CreateJobObjectW(NULL,NULL);limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job||!stop_event||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limit,sizeof(limit))){CloseHandle(input);CloseHandle(output);return 0;}
    si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=input;si.hStdOutput=output;si.hStdError=output;
    if(!CreateProcessW(powershell,command,NULL,NULL,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,NULL,data,&si,&pi)){
        CloseHandle(input);CloseHandle(output);return 0;
    }
    CloseHandle(input);CloseHandle(output);
    if(!AssignProcessToJobObject(job,pi.hProcess)){TerminateProcess(pi.hProcess,1);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return 0;}
    process=pi.hProcess;ResumeThread(pi.hThread);CloseHandle(pi.hThread);return 1;
}
static LRESULT CALLBACK handler(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==taskbar_created && taskbar_created){add_icon();return 0;}
    switch(msg){
    case WM_CREATE:
        status=CreateWindowW(L"STATIC",L"실행 준비 중...",WS_CHILD|WS_VISIBLE,22,22,460,60,hwnd,NULL,NULL,NULL);
        CreateWindowW(L"STATIC",L"창의 X를 누르면 트레이로 숨겨지고 감시는 계속됩니다.\n완전히 종료하려면 [완전 종료]를 선택하세요.\n카메라 1개는 자동 선택, 여러 개면 선택 창이 열립니다.",WS_CHILD|WS_VISIBLE,22,86,460,76,hwnd,NULL,NULL,NULL);
        CreateWindowW(L"BUTTON",L"대시보드 열기",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,22,180,145,34,hwnd,(HMENU)ID_DASH,NULL,NULL);
        CreateWindowW(L"BUTTON",L"트레이로 숨기기",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,177,180,145,34,hwnd,(HMENU)ID_HIDE,NULL,NULL);
        CreateWindowW(L"BUTTON",L"완전 종료",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,332,180,130,34,hwnd,(HMENU)ID_EXIT,NULL,NULL);return 0;
    case WM_COMMAND:
        switch(LOWORD(wp)){
        case ID_DASH:open_dashboard();break;
        case ID_SHOW:show_window();break;
        case ID_HIDE:hide_window();break;
        case ID_EXIT:exit_app();break;
        case ID_LOGS:ShellExecuteW(NULL,L"open",data,NULL,NULL,SW_SHOWNORMAL);break;
        }return 0;
    case TRAY_MESSAGE:
        if(lp==WM_LBUTTONDBLCLK)show_window();
        if(lp==WM_RBUTTONUP||lp==WM_CONTEXTMENU){
            POINT pt;HMENU menu=CreatePopupMenu();GetCursorPos(&pt);
            AppendMenuW(menu,MF_STRING,ID_DASH,L"대시보드 열기");AppendMenuW(menu,MF_STRING,ID_SHOW,L"실행 창 열기");
            AppendMenuW(menu,MF_STRING,ID_LOGS,L"설정·기록 폴더 열기");AppendMenuW(menu,MF_SEPARATOR,0,NULL);
            AppendMenuW(menu,MF_STRING,ID_EXIT,L"완전 종료");SetForegroundWindow(hwnd);
            TrackPopupMenu(menu,TPM_RIGHTBUTTON,pt.x,pt.y,0,hwnd,NULL);DestroyMenu(menu);PostMessageW(hwnd,WM_NULL,0,0);
        }return 0;
    case WM_CLOSE:hide_window();return 0;
    case WM_SIZE:if(wp==SIZE_MINIMIZED)hide_window();return 0;
    case WM_TIMER:
        if(stopping){if(!process||WaitForSingleObject(process,0)==WAIT_OBJECT_0||GetTickCount64()-stop_at>10000)DestroyWindow(hwnd);}
        else if(process&&WaitForSingleObject(process,0)==WAIT_OBJECT_0){
            CloseHandle(process);process=NULL;if(job){CloseHandle(job);job=NULL;}
            SetWindowTextW(status,L"실행 관리자가 종료되었습니다. 기록 폴더의 launcher 로그를 확인하세요.\n완전 종료 후 다시 실행할 수 있습니다.");
            wcscpy_s(icon.szTip,128,L"unmanned_detector - 실행 중지");Shell_NotifyIconW(NIM_MODIFY,&icon);show_window();
            check_alert(0);
        }
        else if(process)check_alert(1);
        return 0;
    case WM_QUERYENDSESSION:exit_app();return TRUE;
    case WM_ENDSESSION:if(wp)DestroyWindow(hwnd);return 0;
    case WM_DESTROY:
        KillTimer(hwnd,1);Shell_NotifyIconW(NIM_DELETE,&icon);if(job){CloseHandle(job);job=NULL;}PostQuitMessage(0);return 0;
    }return DefWindowProcW(hwnd,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show){
    WNDCLASSW cls={0};MSG msg;WCHAR *slash;WSADATA ws;int argc;LPWSTR *argv;
    (void)previous;(void)command;(void)show;
    mutex=CreateMutexW(NULL,FALSE,L"Local\\HUNIK.Installed.Launcher");
    if(!mutex)return 1;
    if(GetLastError()==ERROR_ALREADY_EXISTS){HWND other=FindWindowW(L"HUNIK.TrayHost",NULL);if(other){ShowWindow(other,SW_SHOW);SetForegroundWindow(other);}CloseHandle(mutex);return 0;}
    GetModuleFileNameW(NULL,root,1024);slash=wcsrchr(root,L'\\');if(slash)*slash=0;
    SHGetFolderPathW(NULL,CSIDL_LOCAL_APPDATA,NULL,SHGFP_TYPE_CURRENT,data);wcscat_s(data,1024,L"\\unmanned_detector");
    argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    for(int i=1;i+1<argc;i++)if(!wcscmp(argv[i],L"--data-dir")){wcscpy_s(data,1024,argv[++i]);}
    for(int i=1;i+1<argc;i++){
        if(!wcscmp(argv[i],L"--dashboard-port"))dash_port=_wtoi(argv[++i]);
        else if(!wcscmp(argv[i],L"--stream-port"))stream_port=_wtoi(argv[++i]);
    }
    if(dash_port<1||dash_port>65535||stream_port<1||stream_port>65535||dash_port==stream_port){LocalFree(argv);CloseHandle(mutex);return 1;}
    for(int i=1;i<argc;i++)if(!wcscmp(argv[i],L"--hidden"))start_hidden=1;
    LocalFree(argv);SHCreateDirectoryExW(NULL,data,NULL);WSAStartup(MAKEWORD(2,2),&ws);
    cls.lpfnWndProc=handler;cls.hInstance=instance;cls.lpszClassName=L"HUNIK.TrayHost";cls.hCursor=LoadCursor(NULL,IDC_ARROW);cls.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);cls.hIcon=LoadIcon(NULL,IDI_APPLICATION);RegisterClassW(&cls);
    window=CreateWindowW(cls.lpszClassName,APP_TITLE,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,510,275,NULL,NULL,instance,NULL);
    if(!window){CloseHandle(mutex);return 1;}
    icon.cbSize=sizeof(icon);icon.hWnd=window;icon.uID=1;icon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;icon.uCallbackMessage=TRAY_MESSAGE;icon.hIcon=cls.hIcon;
    wcscpy_s(icon.szTip,128,APP_TITLE);add_icon();taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");
    ShowWindow(window,start_hidden?SW_HIDE:SW_SHOW);SetTimer(window,1,250,NULL);
    if(!start_worker()){MessageBoxW(window,L"시작하지 못했습니다. 기록 경로와 실행 파일을 확인하세요.",APP_TITLE,MB_ICONERROR);DestroyWindow(window);}
    else SetWindowTextW(status,RUNNING_TEXT);
    while(GetMessageW(&msg,NULL,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    if(process)CloseHandle(process);if(stop_event)CloseHandle(stop_event);if(job)CloseHandle(job);CloseHandle(mutex);WSACleanup();return 0;
}
