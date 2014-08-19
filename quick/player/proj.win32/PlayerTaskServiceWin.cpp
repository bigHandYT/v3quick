
#include <sstream>

#include "stdafx.h"
#include "PlayerTaskServiceWin.h"

PLAYER_NS_BEGIN

PlayerTaskWin *PlayerTaskWin::create(const std::string &name, const std::string &executePath, const std::string &commandLineArguments)
{
    PlayerTaskWin *task = new PlayerTaskWin(name, executePath, commandLineArguments);
    task->autorelease();
    return task;
}

PlayerTaskWin::PlayerTaskWin(const std::string &name,
                             const std::string &executePath,
                             const std::string &commandLineArguments)
                             : PlayerTask(name, executePath, commandLineArguments)
                             , _childStdInRead(NULL)
                             , _childStdInWrite(NULL)
                             , _childStdOutRead(NULL)
                             , _childStdOutWrite(NULL)
                             , _outputBuff(NULL)
{
    ZeroMemory(&_pi, sizeof(_pi));
}

PlayerTaskWin::~PlayerTaskWin()
{
    if (_outputBuff) delete[] _outputBuff;
}

bool PlayerTaskWin::run()
{
    if (!isIdle())
    {
        CCLOG("PlayerTaskWin::run() - task is not idle");
        return false;
    }

    //BOOL WINAPI CreateProcess(
    //    _In_opt_     LPCTSTR lpApplicationName,
    //    _Inout_opt_  LPTSTR lpCommandLine,
    //    _In_opt_     LPSECURITY_ATTRIBUTES lpProcessAttributes,
    //    _In_opt_     LPSECURITY_ATTRIBUTES lpThreadAttributes,
    //    _In_         BOOL bInheritHandles,
    //    _In_         DWORD dwCreationFlags,
    //    _In_opt_     LPVOID lpEnvironment,
    //    _In_opt_     LPCTSTR lpCurrentDirectory,
    //    _In_         LPSTARTUPINFO lpStartupInfo,
    //    _Out_        LPPROCESS_INFORMATION lpProcessInformation
    //);

    // http://msdn.microsoft.com/en-us/library/windows/desktop/ms682499(v=vs.85).aspx
    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // Create a pipe for the child process's STDOUT. 
    if (!CreatePipe(&_childStdOutRead, &_childStdOutWrite, &sa, 0) || !SetHandleInformation(_childStdOutRead, HANDLE_FLAG_INHERIT, 0))
    {
        CCLOG("PlayerTaskWin::run() - create stdout handle failed, for execute %s", _executePath.c_str());
        cleanup();
        return false;
    }

    // Create a pipe for the child process's STDIN. 
    if (!CreatePipe(&_childStdInRead, &_childStdInWrite, &sa, 0) || !SetHandleInformation(_childStdInWrite, HANDLE_FLAG_INHERIT, 0))
    {
        CCLOG("PlayerTaskWin::run() - create stdout handle failed, for execute %s", _executePath.c_str());
        cleanup();
        return false;
    }

    ZeroMemory(&_pi, sizeof(_pi));
    STARTUPINFO si = {0};

    si.cb = sizeof(STARTUPINFO);
    si.hStdError = _childStdOutWrite;
    si.hStdOutput = _childStdOutWrite;
    si.hStdInput = _childStdInRead;
    si.dwFlags |= STARTF_USESTDHANDLES;

    const std::u16string u16command = makeCommandLine();
    WCHAR command[MAX_PATH];
    wcscpy_s(command, MAX_PATH, (WCHAR*)u16command.c_str());

    BOOL success = CreateProcess(NULL,
                                 command,     // command line 
                                 NULL,          // process security attributes 
                                 NULL,          // primary thread security attributes 
                                 TRUE,          // handles are inherited 
                                 0,             // creation flags 
                                 NULL,          // use parent's environment 
                                 NULL,          // use parent's current directory 
                                 &si,  // STARTUPINFO pointer 
                                 &_pi);  // receives PROCESS_INFORMATION 

    if (!success)
    {
        CCLOG("PlayerTaskWin::run() - create process failed, for execute %s", _executePath.c_str());
        cleanup();
        return false;
    }

    _outputBuff = new CHAR[BUFF_SIZE];
    _state = STATE_RUNNING;

    cocos2d::Director::getInstance()->getScheduler()->scheduleUpdate(this, 0, false);
    return true;
}

void PlayerTaskWin::stop()
{
    if (_pi.hProcess)
    {
        TerminateProcess(_pi.hProcess, 0);
        _resultCode = -1;
    }
    cleanup();
}

void PlayerTaskWin::update(float dt)
{
    CC_UNUSED_PARAM(dt);

    DWORD resultCode = 0;
    BOOL ret = GetExitCodeProcess(_pi.hProcess, &resultCode);

    // read output
    for (;;)
    {
        DWORD readCount = 0;
        PeekNamedPipe(_childStdOutRead, NULL, NULL, NULL, &readCount, NULL);
        if (readCount == 0) break;

        readCount = 0;
        ZeroMemory(_outputBuff, BUFF_SIZE);
        BOOL success = ReadFile(_childStdOutRead, _outputBuff, BUFF_SIZE - 1, &readCount, NULL);
        CCLOG("readCount = %u", readCount);
        if (!success || readCount == 0) break;
        _outputStream.append(_outputBuff);
    }

    if (ret)
    {
        if (resultCode == STILL_ACTIVE) return;
        _resultCode = (int)resultCode;
    }
    else
    {
        // unexpected error
        _resultCode = (int)GetLastError();
    }

    cocos2d::Director::getInstance()->getScheduler()->unscheduleAllForTarget(this);
    cleanup();
}

void PlayerTaskWin::cleanup()
{
    if (_pi.hProcess) CloseHandle(_pi.hProcess);
    if (_pi.hThread) CloseHandle(_pi.hThread);
    ZeroMemory(&_pi, sizeof(_pi));

    if (_outputBuff) delete[] _outputBuff;
    _outputBuff = NULL;

    if (_childStdOutRead) CloseHandle(_childStdOutRead);
    if (_childStdOutWrite) CloseHandle(_childStdOutWrite);
    if (_childStdInRead) CloseHandle(_childStdInRead);
    if (_childStdInWrite) CloseHandle(_childStdInWrite);

    _childStdOutRead = NULL;
    _childStdOutWrite = NULL;
    _childStdInRead = NULL;
    _childStdInWrite = NULL;

    _state = STATE_COMPLETED;

    CCLOG("CMD: %s", _outputStream.c_str());
}

std::u16string PlayerTaskWin::makeCommandLine() const
{
    std::stringstream buf;
    buf << "\"";
    buf << _executePath;
    buf << "\" ";
    buf << _commandLineArguments;

    std::u16string u16command;
    cocos2d::StringUtils::UTF8ToUTF16(buf.str(), u16command);
    return u16command;
}

PlayerTaskServiceWin::PlayerTaskServiceWin(HWND hwnd)
    : _hwnd(hwnd)
{
}

PlayerTaskServiceWin::~PlayerTaskServiceWin()
{
    for (auto it = _tasks.begin(); it != _tasks.end(); ++it)
    {
        it->second->stop();
    }
}

PlayerTask *PlayerTaskServiceWin::createTask(const std::string &name,
                                             const std::string &executePath,
                                             const std::string &commandLineArguments)
{
    CCASSERT(_tasks.find(name) == _tasks.end(), "Task already exists.");
    PlayerTaskWin *task = PlayerTaskWin::create(name, executePath, commandLineArguments);
    _tasks.insert(name, task);
    return task;
}

PlayerTask *PlayerTaskServiceWin::getTask(const std::string &name)
{
    auto it = _tasks.find(name);
    return it != _tasks.end() ? it->second : nullptr;
}

PLAYER_NS_END
