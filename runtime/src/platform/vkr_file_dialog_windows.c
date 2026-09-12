#include "core/vkr_window.h"
#include "platform/vkr_file_dialog_internal.h"
#if defined(PLATFORM_WINDOWS)
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <shobjidl.h>
#include <wchar.h>

// Native COM marshalling buffers use the COM task allocator and never escape
// this call. Published UTF-8 paths use the caller's VKR allocator.
static wchar_t *dialog_wide(const char *text) {
  if (!text) {
    return NULL;
  }
  int32_t count =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
  if (count <= 0) {
    return NULL;
  }
  wchar_t *wide = CoTaskMemAlloc((size_t)count * sizeof(*wide));
  if (!wide) {
    return NULL;
  }
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
                           count)) {
    CoTaskMemFree(wide);
    return NULL;
  }
  return wide;
}

static HRESULT dialog_copy_path(IShellItem *item, VkrAllocator *allocator,
                                char **output) {
  PWSTR wide = NULL;
  HRESULT hr = IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &wide);
  if (FAILED(hr)) {
    return hr;
  }
  int32_t count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                      NULL, 0, NULL, NULL);
  if (count <= 0) {
    CoTaskMemFree(wide);
    return E_INVALIDARG;
  }
  char *path = vkr_allocator_alloc(allocator, (uint64_t)count,
                                   VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!path) {
    CoTaskMemFree(wide);
    return E_OUTOFMEMORY;
  }
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, path, count,
                           NULL, NULL)) {
    vkr_allocator_free(allocator, path, (uint64_t)count,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    CoTaskMemFree(wide);
    return E_INVALIDARG;
  }
  CoTaskMemFree(wide);
  *output = path;
  return S_OK;
}

void vkr_file_dialog_native_show(VkrWindow *window,
                                 const VkrFileDialogRequest *request,
                                 VkrAllocator *allocator,
                                 VkrFileDialogResult *result) {
  HWND owner = (HWND)vkr_window_get_win32_handle(window);
  if (GetWindowThreadProcessId(owner, NULL) != GetCurrentThreadId()) {
    snprintf(result->diagnostic, sizeof(result->diagnostic),
             "Native file dialogs must run on the window UI thread.");
    return;
  }
  HRESULT hr =
      CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(hr)) {
    snprintf(result->diagnostic, sizeof(result->diagnostic),
             "Cannot initialize file dialog COM apartment (0x%08lx).",
             (unsigned long)hr);
    return;
  }
  IFileDialog *dialog = NULL;
  IFileOpenDialog *open = NULL;
  IShellItemArray *items = NULL;
  IShellItem *item = NULL;
  wchar_t *title = dialog_wide(request->title);
  wchar_t *directory = dialog_wide(request->initial_directory);
  wchar_t *name = dialog_wide(request->suggested_name);
  if ((request->title && !title) ||
      (request->initial_directory && !directory) ||
      (request->suggested_name && !name)) {
    hr = E_INVALIDARG;
    goto cleanup;
  }
  hr = CoCreateInstance(
      request->kind == VKR_FILE_DIALOG_SAVE_FILE ? &CLSID_FileSaveDialog
                                                 : &CLSID_FileOpenDialog,
      NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog, (void **)&dialog);
  if (FAILED(hr)) {
    goto cleanup;
  }
  FILEOPENDIALOGOPTIONS options =
      FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST;
  if (request->kind == VKR_FILE_DIALOG_SAVE_FILE) {
    options |= FOS_OVERWRITEPROMPT;
  } else if (request->kind == VKR_FILE_DIALOG_OPEN_FOLDER) {
    options |= FOS_PICKFOLDERS;
  } else {
    options |= FOS_FILEMUSTEXIST;
  }
  if (request->multiple) {
    options |= FOS_ALLOWMULTISELECT;
  }
  hr = IFileDialog_SetOptions(dialog, options);
  if (FAILED(hr)) {
    goto cleanup;
  }
  if (title) {
    hr = IFileDialog_SetTitle(dialog, title);
    if (FAILED(hr)) {
      goto cleanup;
    }
  }
  if (name) {
    hr = IFileDialog_SetFileName(dialog, name);
    if (FAILED(hr)) {
      goto cleanup;
    }
  }
  if (directory && directory[0]) {
    hr = SHCreateItemFromParsingName(directory, NULL, &IID_IShellItem,
                                     (void **)&item);
    if (FAILED(hr)) {
      goto cleanup;
    }
    hr = IFileDialog_SetFolder(dialog, item);
    IShellItem_Release(item);
    item = NULL;
    if (FAILED(hr)) {
      goto cleanup;
    }
  }
  COMDLG_FILTERSPEC filters[65] = {0};
  wchar_t patterns[64][35] = {0};
  if (request->extension_count &&
      request->kind != VKR_FILE_DIALOG_OPEN_FOLDER) {
    for (uint32_t i = 0; i < request->extension_count; ++i) {
      patterns[i][0] = L'*';
      patterns[i][1] = L'.';
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, request->extensions[i],
                          -1, &patterns[i][2], 33);
      filters[i].pszName = patterns[i];
      filters[i].pszSpec = patterns[i];
    }
    filters[request->extension_count].pszName = L"All files";
    filters[request->extension_count].pszSpec = L"*.*";
    hr =
        IFileDialog_SetFileTypes(dialog, request->extension_count + 1, filters);
    if (FAILED(hr)) {
      goto cleanup;
    }
  }
  const bool8_t captured = vkr_window_is_mouse_captured(window);
  vkr_window_set_mouse_capture(window, false_v);
  hr = IFileDialog_Show(dialog, owner);
  SetForegroundWindow(owner);
  vkr_window_set_mouse_capture(window, captured);
  EventManager *events = window->input_state.event_manager;
  bool32_t initialized = window->input_state.is_initialized;
  MemZero(&window->input_state, sizeof(window->input_state));
  window->input_state.event_manager = events;
  window->input_state.is_initialized = initialized;
  if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    result->status = VKR_FILE_DIALOG_CANCELLED;
    goto cleanup;
  }
  if (FAILED(hr)) {
    goto cleanup;
  }
  DWORD count = 1;
  if (request->multiple) {
    hr = IFileDialog_QueryInterface(dialog, &IID_IFileOpenDialog,
                                    (void **)&open);
    if (FAILED(hr)) {
      goto cleanup;
    }
    hr = IFileOpenDialog_GetResults(open, &items);
    if (FAILED(hr)) {
      goto cleanup;
    }
    hr = IShellItemArray_GetCount(items, &count);
    if (FAILED(hr)) {
      goto cleanup;
    }
  }
  if (!vkr_file_dialog_allocate_paths(allocator, result, count)) {
    hr = E_OUTOFMEMORY;
    goto cleanup;
  }
  for (DWORD i = 0; i < count; ++i) {
    hr = items ? IShellItemArray_GetItemAt(items, i, &item)
               : IFileDialog_GetResult(dialog, &item);
    if (FAILED(hr)) {
      goto cleanup;
    }
    hr = dialog_copy_path(item, allocator, &result->paths[i]);
    IShellItem_Release(item);
    item = NULL;
    if (FAILED(hr)) {
      goto cleanup;
    }
  }
  result->status = VKR_FILE_DIALOG_SELECTED;
cleanup:
  if (result->status == VKR_FILE_DIALOG_ERROR) {
    snprintf(result->diagnostic, sizeof(result->diagnostic),
             "Native file dialog failed (0x%08lx).", (unsigned long)hr);
  }
  if (item) {
    IShellItem_Release(item);
  }
  if (items) {
    IShellItemArray_Release(items);
  }
  if (open) {
    IFileOpenDialog_Release(open);
  }
  if (dialog) {
    IFileDialog_Release(dialog);
  }
  CoTaskMemFree(name);
  CoTaskMemFree(directory);
  CoTaskMemFree(title);
  CoUninitialize();
}
#endif
