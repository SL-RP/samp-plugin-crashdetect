// Copyright (c) 2011-2015 Zeex
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <functional>
#include <sstream>
#include <string>
#include <subhook.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <stdio.h>
#endif
#include "amxpathfinder.h"
#include "crashdetecthandler.h"
#include "fileutils.h"
#include "logprintf.h"
#include "natives.h"
#include "os.h"
#include "plugincommon.h"
#include "pluginversion.h"
#include "stringutils.h"

namespace {

subhook::Hook amx_exec_hook;

// Path to the last loaded AMX file. This is used to make a connection between
// *.amx files and their corresponding AMX instances.
std::string last_amx_path;

// Stores paths to loaded AMX files and is able to find a path by a pointer to
// an AMX instance.
AMXPathFinder amx_path_finder;

#ifdef _WIN32
  subhook::Hook create_file_hook;

  HANDLE WINAPI CreateFileAHook(
    _In_ LPCSTR lpFileName,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwShareMode,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ DWORD dwCreationDisposition,
    _In_ DWORD dwFlagsAndAttributes,
    _In_opt_ HANDLE hTemplateFile)
  {
    subhook::ScopedHookRemove _(&create_file_hook);
    const char *ext = fileutils::GetFileExtensionPtr(lpFileName);
    if (ext != 0 && stringutils::CompareIgnoreCase(ext, "amx") == 0) {
      last_amx_path = lpFileName;
    }
    return CreateFileA(
      lpFileName,
      dwDesiredAccess,
      dwShareMode,
      lpSecurityAttributes,
      dwCreationDisposition,
      dwFlagsAndAttributes,
      hTemplateFile);
  }
#else
  subhook::Hook fopen_hook;

  FILE *FopenHook(const char *filename, const char *mode) {
    subhook::ScopedHookRemove _(&fopen_hook);
    const char *ext = fileutils::GetFileExtensionPtr(filename);
    if (ext != 0 && stringutils::CompareIgnoreCase(ext, "amx") == 0) {
      last_amx_path = filename;
    }
    return fopen(filename, mode);
  }
#endif

int AMXAPI ProcessDebugHook(AMX *amx) {
  return CrashDetectHandler::GetHandler(amx)->ProcessDebugHook();
}

int AMXAPI ProcessCallback(AMX *amx, cell index, cell *result, cell *params) {
  CrashDetectHandler *handler = CrashDetectHandler::GetHandler(amx);
  return handler->ProcessCallback(index, result, params);
}

int AMXAPI ProcessExec(AMX *amx, cell *retval, int index) {
  if (amx->flags & AMX_FLAG_BROWSE) {
    return amx_Exec(amx, retval, index);
  }
  CrashDetectHandler *handler = CrashDetectHandler::GetHandler(amx);
  if (handler == 0) {
    return amx_Exec(amx, retval, index);
  }
  return handler->ProcessExec(retval, index);
}

void AMXAPI ProcessExecError(AMX *amx, cell index, cell *retval, int error) {
  CrashDetectHandler *handler = CrashDetectHandler::GetHandler(amx);
  handler->ProcessExecError(index, retval, error);
}

} // anonymous namespace

PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports() {
  return SUPPORTS_VERSION | SUPPORTS_AMX_NATIVES;
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void **ppData) {
  void **exports = reinterpret_cast<void**>(ppData[PLUGIN_DATA_AMX_EXPORTS]);
  ::logprintf = (logprintf_t)ppData[PLUGIN_DATA_LOGPRINTF];

  void *amx_Exec_ptr = exports[PLUGIN_AMX_EXPORT_Exec];
  void *amx_Exec_sub = subhook::Hook::ReadDst(amx_Exec_ptr);

  if (amx_Exec_sub == 0) {
    amx_exec_hook.Install(amx_Exec_ptr, (void*)ProcessExec);
  } else {
    std::string module =
      fileutils::GetFileName(os::GetModuleName(amx_Exec_sub));
    if (!module.empty()) {
      logprintf("  CrashDetect must be loaded before '%s'", module.c_str());
    }
    return false;
  }

  #if _WIN32
    create_file_hook.Install((void*)CreateFileA, (void*)CreateFileAHook);
  #else
    fopen_hook.Install((void*)fopen, (void*)FopenHook);
  #endif

  amx_path_finder.AddSearchPath("gamemodes");
  amx_path_finder.AddSearchPath("filterscripts");

  const char *amx_path_var = getenv("AMX_PATH");
  if (amx_path_var != 0) {
    stringutils::SplitString(
      amx_path_var,
      fileutils::kNativePathListSepChar,
      std::bind1st(std::mem_fun(&AMXPathFinder::AddSearchPath),
                   &amx_path_finder));
  }

  os::SetCrashHandler(CrashDetectHandler::OnCrash);
  os::SetInterruptHandler(CrashDetectHandler::OnInterrupt);

  logprintf("  CrashDetect plugin " PROJECT_VERSION_STRING);
  return true;
}

PLUGIN_EXPORT int PLUGIN_CALL AmxLoad(AMX *amx) {
  if (last_amx_path.length() != 0) {
    amx_path_finder.AddKnownFile(amx, last_amx_path);
  }

  CrashDetectHandler *handler = CrashDetectHandler::CreateHandler(amx);
  handler->set_amx_path_finder(&amx_path_finder);
  handler->Load();

  amx_SetDebugHook(amx, ProcessDebugHook);
  amx_SetCallback(amx, ProcessCallback);
  amx_SetExecErrorHandler(amx, ProcessExecError);

  RegisterNatives(amx);
  return AMX_ERR_NONE;
}

PLUGIN_EXPORT int PLUGIN_CALL AmxUnload(AMX *amx) {
  CrashDetectHandler::GetHandler(amx)->Unload();
  CrashDetectHandler::DestroyHandler(amx);
  return AMX_ERR_NONE;
}
