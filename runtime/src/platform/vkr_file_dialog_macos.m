#include "core/vkr_window.h"
#include "platform/vkr_file_dialog_internal.h"
#if defined(PLATFORM_APPLE)
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

void vkr_file_dialog_native_show(VkrWindow *window,
                                 const VkrFileDialogRequest *request,
                                 VkrAllocator *allocator,
                                 VkrFileDialogResult *result) {
  if (![NSThread isMainThread]) {
    snprintf(result->diagnostic, sizeof(result->diagnostic),
             "Native file dialogs must run on the main UI thread.");
    return;
  }
  @autoreleasepool {
    NSWindow *owner = (NSWindow *)vkr_window_get_cocoa_handle(window);
    const bool8_t captured = vkr_window_is_mouse_captured(window);
    NSSavePanel *panel = request->kind == VKR_FILE_DIALOG_SAVE_FILE
                             ? [NSSavePanel savePanel]
                             : [NSOpenPanel openPanel];
    if (request->kind != VKR_FILE_DIALOG_SAVE_FILE) {
      NSOpenPanel *open = (NSOpenPanel *)panel;
      [open
          setCanChooseDirectories:request->kind == VKR_FILE_DIALOG_OPEN_FOLDER];
      [open setCanChooseFiles:request->kind == VKR_FILE_DIALOG_OPEN_FILE];
      [open setAllowsMultipleSelection:request->multiple];
      [open setResolvesAliases:YES];
    }
    if (request->title) {
      NSString *title = [NSString stringWithUTF8String:request->title];
      if (!title) {
        snprintf(result->diagnostic, sizeof(result->diagnostic),
                 "Dialog title is not UTF-8.");
        return;
      }
      [panel setTitle:title];
    }
    if (request->initial_directory && request->initial_directory[0]) {
      NSString *directory =
          [NSString stringWithUTF8String:request->initial_directory];
      if (!directory) {
        snprintf(result->diagnostic, sizeof(result->diagnostic),
                 "Initial directory is not UTF-8.");
        return;
      }
      [panel setDirectoryURL:[NSURL fileURLWithPath:directory isDirectory:YES]];
    }
    if (request->suggested_name) {
      NSString *name = [NSString stringWithUTF8String:request->suggested_name];
      if (!name) {
        snprintf(result->diagnostic, sizeof(result->diagnostic),
                 "Suggested name is not UTF-8.");
        return;
      }
      [panel setNameFieldStringValue:name];
    }
    if (request->extension_count &&
        request->kind != VKR_FILE_DIALOG_OPEN_FOLDER) {
      NSMutableArray<UTType *> *types = [NSMutableArray array];
      for (uint32_t i = 0; i < request->extension_count; ++i) {
        UTType *type =
            [UTType typeWithFilenameExtension:
                        [NSString stringWithUTF8String:request->extensions[i]]];
        if (type) {
          [types addObject:type];
        }
      }
      [panel setAllowedContentTypes:types];
      [panel setAllowsOtherFileTypes:NO];
    }
    vkr_window_set_mouse_capture(window, false_v);
    __block bool8_t finished = false_v;
    __block NSModalResponse response = NSModalResponseCancel;
    [panel beginSheetModalForWindow:owner
                  completionHandler:^(NSModalResponse value) {
                    response = value;
                    finished = true_v;
                    [NSApp stopModal];
                  }];
    if (!finished) {
      [NSApp runModalForWindow:panel];
    }
    [owner makeKeyAndOrderFront:nil];
    vkr_window_set_mouse_capture(window, captured);
    // Native modal pumping consumes key-up events. Retire held/latching input
    // so the dismissing click/Enter cannot leak into the covered form.
    EventManager *events = window->input_state.event_manager;
    bool32_t initialized = window->input_state.is_initialized;
    MemZero(&window->input_state, sizeof(window->input_state));
    window->input_state.event_manager = events;
    window->input_state.is_initialized = initialized;
    if (response != NSModalResponseOK) {
      result->status = VKR_FILE_DIALOG_CANCELLED;
      return;
    }
    NSArray<NSURL *> *urls = request->kind == VKR_FILE_DIALOG_SAVE_FILE
                                 ? @[ [panel URL] ]
                                 : [(NSOpenPanel *)panel URLs];
    if ([urls count] > UINT32_MAX ||
        !vkr_file_dialog_allocate_paths(allocator, result,
                                        (uint32_t)[urls count])) {
      snprintf(result->diagnostic, sizeof(result->diagnostic),
               "Cannot allocate selected paths.");
      return;
    }
    for (uint32_t i = 0; i < result->path_count; ++i) {
      const char *path = [[[urls objectAtIndex:i] path] UTF8String];
      if (!path) {
        snprintf(result->diagnostic, sizeof(result->diagnostic),
                 "Selected path cannot be represented as UTF-8.");
        return;
      }
      size_t size = strlen(path) + 1;
      result->paths[i] =
          vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      if (!result->paths[i]) {
        snprintf(result->diagnostic, sizeof(result->diagnostic),
                 "Cannot allocate selected path.");
        return;
      }
      MemCopy(result->paths[i], path, size);
    }
    result->status = VKR_FILE_DIALOG_SELECTED;
  }
}
#endif
