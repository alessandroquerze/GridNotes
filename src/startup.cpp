#define INITGUID
#include <windows.h>
#include <taskschd.h>
#include <comdef.h>
#include "startup.h"



#include <string>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsuppw.lib")

static const wchar_t* kTaskName = L"MyAppStartup";

static std::wstring GetExePath()
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return exePath;
}

static std::wstring GetExeDir(const std::wstring& exePath)
{
    size_t pos = exePath.find_last_of(L"\\/");

    if (pos == std::wstring::npos)
        return L"";

    return exePath.substr(0, pos);
}


bool SetStartup(bool enabled)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInited = SUCCEEDED(hr);

    if (hr == RPC_E_CHANGED_MODE)
        comInited = false;

    ITaskService* service = nullptr;

    hr = CoCreateInstance(
        CLSID_TaskScheduler,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_ITaskService,
        (void**)&service);

    if (FAILED(hr))
        return false;


    hr = service->Connect(
        _variant_t(),
        _variant_t(),
        _variant_t(),
        _variant_t());

    if (FAILED(hr))
    {
        service->Release();
        return false;
    }


    ITaskFolder* rootFolder = nullptr;

    hr = service->GetFolder(
        _bstr_t(L"\\"),
        &rootFolder);

    if (FAILED(hr))
    {
        service->Release();
        return false;
    }


    if (!enabled)
    {
        rootFolder->DeleteTask(
            _bstr_t(kTaskName),
            0);

        rootFolder->Release();
        service->Release();

        if (comInited)
            CoUninitialize();

        return true;
    }


    ITaskDefinition* task = nullptr;

    hr = service->NewTask(0, &task);

    if (FAILED(hr))
    {
        rootFolder->Release();
        service->Release();
        return false;
    }


    IRegistrationInfo* regInfo = nullptr;

    if (SUCCEEDED(task->get_RegistrationInfo(&regInfo)))
    {
        regInfo->put_Author(_bstr_t(L"MyApp"));
        regInfo->Release();
    }


    IPrincipal* principal = nullptr;

    if (SUCCEEDED(task->get_Principal(&principal)))
    {
        principal->put_LogonType(
            TASK_LOGON_INTERACTIVE_TOKEN);

        // NO Highest privileges
        principal->put_RunLevel(
            TASK_RUNLEVEL_LUA);

        principal->Release();
    }


    ITaskSettings* settings = nullptr;

    if (SUCCEEDED(task->get_Settings(&settings)))
    {
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_RunOnlyIfIdle(VARIANT_FALSE);
        settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));
        settings->Release();
    }


    ITriggerCollection* triggers = nullptr;

    hr = task->get_Triggers(&triggers);

    if (FAILED(hr))
        return false;


    ITrigger* trigger = nullptr;

    hr = triggers->Create(
        TASK_TRIGGER_LOGON,
        &trigger);

    triggers->Release();

    if (FAILED(hr))
        return false;


    ILogonTrigger* logon = nullptr;

    hr = trigger->QueryInterface(
        IID_ILogonTrigger,
        (void**)&logon);

    trigger->Release();

    if (FAILED(hr))
        return false;


    logon->put_Enabled(VARIANT_TRUE);

    logon->Release();


    IActionCollection* actions = nullptr;

    hr = task->get_Actions(&actions);

    if (FAILED(hr))
        return false;


    IAction* action = nullptr;

    hr = actions->Create(
        TASK_ACTION_EXEC,
        &action);

    actions->Release();

    if (FAILED(hr))
        return false;


    IExecAction* exec = nullptr;

    hr = action->QueryInterface(
        IID_IExecAction,
        (void**)&exec);

    action->Release();

    if (FAILED(hr))
        return false;


    std::wstring exePath = GetExePath();
    std::wstring exeDir = GetExeDir(exePath);


    exec->put_Path(_bstr_t(exePath.c_str()));

    if (!exeDir.empty())
        exec->put_WorkingDirectory(
            _bstr_t(exeDir.c_str()));

    exec->Release();


    IRegisteredTask* registered = nullptr;

    hr = rootFolder->RegisterTaskDefinition(
        _bstr_t(kTaskName),
        task,
        TASK_CREATE_OR_UPDATE,
        _variant_t(),
        _variant_t(),
        TASK_LOGON_INTERACTIVE_TOKEN,
        _variant_t(L""),
        &registered);


    if (registered)
        registered->Release();


    task->Release();
    rootFolder->Release();
    service->Release();


    if (comInited)
        CoUninitialize();


    return SUCCEEDED(hr);
}